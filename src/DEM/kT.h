//  Copyright (c) 2021, SBEL GPU Development Team
//  Copyright (c) 2021, University of Wisconsin - Madison
//
//	SPDX-License-Identifier: BSD-3-Clause

#ifndef DEME_KT
#define DEME_KT

#include <mutex>
#include <vector>
#include <thread>
#include <unordered_map>
// #include <set>

#include <core/ApiVersion.h>
#include <core/utils/ManagedAllocator.hpp>
#include <core/utils/ThreadManager.h>
#include <core/utils/GpuManager.h>
#include <nvmath/helper_math.cuh>
#include <core/utils/GpuError.h>

#include <DEM/BdrsAndObjs.h>
#include <DEM/Defines.h>
#include <DEM/Structs.h>

// #include <core/utils/JitHelper.h>

// Forward declare jitify::Program to avoid downstream dependency
namespace jitify {
class Program;
}

namespace deme {

// Implementation-level classes
class DEMKinematicThread;
class DEMDynamicThread;
class DEMSolverStateData;

class DEMKinematicThread {
  protected:
    WorkerReportChannel* pPagerToMain;
    ThreadManager* pSchedSupport;
    GpuManager* pGpuDistributor;

    // kT verbosity
    VERBOSITY verbosity = INFO;

    // Some behavior-related flags
    SolverFlags solverFlags;

    // The std::thread that binds to this instance
    std::thread th;

    // Friend system DEMDynamicThread
    DEMDynamicThread* dT;

    // Object which stores the device and stream IDs for this thread
    GpuManager::StreamInfo streamInfo;

    // A class that contains scratch pad and system status data (constructed with the number of temp arrays we need)
    DEMSolverStateData stateOfSolver_resources = DEMSolverStateData(6);

    size_t m_approx_bytes_used = 0;

    // kT should break out of its inner loop and return to a state where it awaits a `start' call at the outer loop
    bool kTShouldReset = false;

    // Pointers to simulation params-related arrays
    DEMSimParams* simParams;

    // Pointers to those data arrays defined below, stored in a struct
    DEMDataKT* granData;

    // Buffer arrays for storing info from the dT side.
    // dT modifies these arrays; kT uses them only.

    // // kT gets clump locations and rotations from dT
    // // The voxel ID
    // std::vector<voxelID_t, ManagedAllocator<voxelID_t>> voxelID_buffer;
    // // The XYZ local location inside a voxel
    // std::vector<subVoxelPos_t, ManagedAllocator<subVoxelPos_t>> locX_buffer;
    // std::vector<subVoxelPos_t, ManagedAllocator<subVoxelPos_t>> locY_buffer;
    // std::vector<subVoxelPos_t, ManagedAllocator<subVoxelPos_t>> locZ_buffer;
    // // The clump quaternion
    // std::vector<oriQ_t, ManagedAllocator<oriQ_t>> oriQ0_buffer;
    // std::vector<oriQ_t, ManagedAllocator<oriQ_t>> oriQ1_buffer;
    // std::vector<oriQ_t, ManagedAllocator<oriQ_t>> oriQ2_buffer;
    // std::vector<oriQ_t, ManagedAllocator<oriQ_t>> oriQ3_buffer;
    // std::vector<family_t, ManagedAllocator<family_t>> familyID_buffer;

    // kT's copy of family map
    // std::unordered_map<unsigned int, family_t> familyUserImplMap;
    // std::unordered_map<family_t, unsigned int> familyImplUserMap;

    // Managed arrays for dT's personal use (not transfer buffer)

    // Those are the template array of the unique component information. Note that these arrays may be thousands-element
    // long, and only a part of it is jitified. The jitified part of it is typically the frequently used clump and maybe
    // triangle tempates; the other part may be the components for a few large clump bodies which are not frequently
    // used. Component sphere's radius
    std::vector<float, ManagedAllocator<float>> radiiSphere;
    // The distinct sphere local position (wrt CoM) values
    std::vector<float, ManagedAllocator<float>> relPosSphereX;
    std::vector<float, ManagedAllocator<float>> relPosSphereY;
    std::vector<float, ManagedAllocator<float>> relPosSphereZ;

    // Triangles (templates) are given a special place (unlike other analytical shapes), b/c we expect them to appear
    // frequently as meshes.
    std::vector<float3, ManagedAllocator<float3>> relPosNode1;
    std::vector<float3, ManagedAllocator<float3>> relPosNode2;
    std::vector<float3, ManagedAllocator<float3>> relPosNode3;

    // External object's components may need the following arrays to store some extra defining features of them. We
    // assume there are usually not too many of them in a simulation.
    // Relative position w.r.t. the owner. For example, the following 3 arrays may hold center points for plates, or tip
    // positions for cones.
    std::vector<float, ManagedAllocator<float>> relPosEntityX;
    std::vector<float, ManagedAllocator<float>> relPosEntityY;
    std::vector<float, ManagedAllocator<float>> relPosEntityZ;
    // Some orientation specifiers. For example, the following 3 arrays may hold normal vectors for planes, or center
    // axis vectors for cylinders.
    std::vector<float, ManagedAllocator<float>> oriEntityX;
    std::vector<float, ManagedAllocator<float>> oriEntityY;
    std::vector<float, ManagedAllocator<float>> oriEntityZ;
    // Some size specifiers. For example, the following 3 arrays may hold top, bottom and length information for finite
    // cylinders.
    std::vector<float, ManagedAllocator<float>> sizeEntity1;
    std::vector<float, ManagedAllocator<float>> sizeEntity2;
    std::vector<float, ManagedAllocator<float>> sizeEntity3;

    // The voxel ID (split into 3 parts, representing XYZ location)
    std::vector<voxelID_t, ManagedAllocator<voxelID_t>> voxelID;

    // The XYZ local location inside a voxel
    std::vector<subVoxelPos_t, ManagedAllocator<subVoxelPos_t>> locX;
    std::vector<subVoxelPos_t, ManagedAllocator<subVoxelPos_t>> locY;
    std::vector<subVoxelPos_t, ManagedAllocator<subVoxelPos_t>> locZ;

    // The clump quaternion
    std::vector<oriQ_t, ManagedAllocator<oriQ_t>> oriQw;
    std::vector<oriQ_t, ManagedAllocator<oriQ_t>> oriQx;
    std::vector<oriQ_t, ManagedAllocator<oriQ_t>> oriQy;
    std::vector<oriQ_t, ManagedAllocator<oriQ_t>> oriQz;

    // Clump's family identification code. Used in determining whether they can be contacts between two families, and
    // whether a family has prescribed motions.
    std::vector<family_t, ManagedAllocator<family_t>> familyID;

    // A long array (usually 32640 elements) registering whether between 2 families there should be contacts
    std::vector<notStupidBool_t, ManagedAllocator<notStupidBool_t>> familyMaskMatrix;

    // kT computed contact pair info
    std::vector<bodyID_t, ManagedAllocator<bodyID_t>> idGeometryA;
    std::vector<bodyID_t, ManagedAllocator<bodyID_t>> idGeometryB;
    std::vector<contact_t, ManagedAllocator<contact_t>> contactType;

    // Contact pair info at the previous time step. This is needed by dT so persistent contacts are identified in
    // history-based models.
    std::vector<bodyID_t, ManagedAllocator<bodyID_t>> previous_idGeometryA;
    std::vector<bodyID_t, ManagedAllocator<bodyID_t>> previous_idGeometryB;
    std::vector<contact_t, ManagedAllocator<contact_t>> previous_contactType;
    std::vector<contactPairs_t, ManagedAllocator<contactPairs_t>> contactMapping;

    // Sphere-related arrays in managed memory
    // Owner body ID of this component
    std::vector<bodyID_t, ManagedAllocator<bodyID_t>> ownerClumpBody;
    std::vector<bodyID_t, ManagedAllocator<bodyID_t>> ownerMesh;

    // The ID that maps this sphere component's geometry-defining parameters, when this component is jitified
    std::vector<clumpComponentOffset_t, ManagedAllocator<clumpComponentOffset_t>> clumpComponentOffset;
    // The ID that maps this sphere component's geometry-defining parameters, when this component is not jitified (too
    // many templates)
    std::vector<clumpComponentOffsetExt_t, ManagedAllocator<clumpComponentOffsetExt_t>> clumpComponentOffsetExt;
    // The ID that maps this analytical entity component's geometry-defining parameters, when this component is jitified
    // std::vector<clumpComponentOffset_t, ManagedAllocator<clumpComponentOffset_t>> analComponentOffset;

    // kT's timers
    std::vector<std::string> timer_names = {"Discretize domain",      "Find contact pairs", "Build history map",
                                            "Unpack updates from dT", "Send to dT buffer",  "Wait for dT update"};
    SolverTimers timers = SolverTimers(timer_names);

  public:
    friend class DEMSolver;
    friend class DEMDynamicThread;

    DEMKinematicThread(WorkerReportChannel* pPager,
                       ThreadManager* pSchedSup,
                       GpuManager* pGpuDist,
                       DEMDynamicThread* dT)
        : pPagerToMain(pPager), pSchedSupport(pSchedSup), pGpuDistributor(pGpuDist) {
        GPU_CALL(cudaMallocManaged(&simParams, sizeof(DEMSimParams), cudaMemAttachGlobal));
        GPU_CALL(cudaMallocManaged(&granData, sizeof(DEMDataKT), cudaMemAttachGlobal));

        // Get a device/stream ID to use from the GPU Manager
        streamInfo = pGpuDistributor->getAvailableStream();

        // My friend dT
        this->dT = dT;

        pPagerToMain->userCallDone = false;
        pSchedSupport->kinematicShouldJoin = false;
        pSchedSupport->kinematicStarted = false;

        // Launch a worker thread bound to this instance
        th = std::move(std::thread([this]() { this->workerThread(); }));
    }
    ~DEMKinematicThread() {
        // std::cout << "Kinematic thread closing..." << std::endl;

        // kT as the aux helper thread, it could be hanging waiting for dT updates when the destructor is called, so we
        // release it from that status here
        breakWaitingStatus();

        // It says start thread but it's actually just telling it to join
        pSchedSupport->kinematicShouldJoin = true;
        startThread();
        th.join();

        cudaStreamDestroy(streamInfo.stream);
    }

    // buffer exchange methods
    void setDestinationBufferPointers();

    // Break inner loop hanging status and wait in the outer loop. Note we must ensure resetUserCallStat is called
    // shortly after breakWaitingStatus is called, since kinematicOwned_Cons2ProdBuffer_isFresh and kTShouldReset can be
    // vulnerable if kT exited through dynamicsDone rather than control variable-based release.
    void breakWaitingStatus();

    // Called each time when the user calls DoDynamicsThenSync.
    void startThread();

    // The actual kernel things go here.
    // It is called upon construction.
    void workerThread();

    /// Reset kT--dT interaction coordinator stats
    void resetUserCallStat();
    /// Return the approximate RAM usage
    size_t estimateMemUsage() const;

    /// Resize managed arrays (and perhaps Instruct/Suggest their preferred residence location as well?)
    void allocateManagedArrays(size_t nOwnerBodies,
                               size_t nOwnerClumps,
                               unsigned int nExtObj,
                               size_t nTriMeshes,
                               size_t nSpheresGM,
                               size_t nTriGM,
                               unsigned int nAnalGM,
                               unsigned int nMassProperties,
                               unsigned int nClumpTopo,
                               unsigned int nClumpComponents,
                               unsigned int nJitifiableClumpComponents,
                               unsigned int nMatTuples);

    // initManagedArrays's components
    void registerPolicies(const std::vector<notStupidBool_t>& family_mask_matrix);
    void populateEntityArrays(const std::vector<std::shared_ptr<DEMClumpBatch>>& input_clump_batches,
                              const std::vector<unsigned int>& input_ext_obj_family,
                              const std::vector<unsigned int>& input_mesh_obj_family,
                              const std::vector<unsigned int>& input_mesh_facet_owner,
                              const std::vector<DEMTriangle>& input_mesh_facets,
                              const ClumpTemplateFlatten& clump_templates,
                              size_t nExistOwners,
                              size_t nExistSpheres,
                              size_t nExistingFacets);

    /// Initialize managed arrays
    void initManagedArrays(const std::vector<std::shared_ptr<DEMClumpBatch>>& input_clump_batches,
                           const std::vector<unsigned int>& input_ext_obj_family,
                           const std::vector<unsigned int>& input_mesh_obj_family,
                           const std::vector<unsigned int>& input_mesh_facet_owner,
                           const std::vector<DEMTriangle>& input_mesh_facets,
                           const std::vector<notStupidBool_t>& family_mask_matrix,
                           const ClumpTemplateFlatten& clump_templates);

    /// Add more clumps and/or meshes into the system, without re-initialization. It must be clump/mesh-addition only,
    /// no other changes to the system.
    void updateClumpMeshArrays(const std::vector<std::shared_ptr<DEMClumpBatch>>& input_clump_batches,
                               const std::vector<unsigned int>& input_ext_obj_family,
                               const std::vector<unsigned int>& input_mesh_obj_family,
                               const std::vector<unsigned int>& input_mesh_facet_owner,
                               const std::vector<DEMTriangle>& input_mesh_facets,
                               const std::vector<notStupidBool_t>& family_mask_matrix,
                               const ClumpTemplateFlatten& clump_templates,
                               size_t nExistingOwners,
                               size_t nExistingClumps,
                               size_t nExistingSpheres,
                               size_t nExistingTriMesh,
                               size_t nExistingFacets);

    /// Set SimParams items
    void setSimParams(unsigned char nvXp2,
                      unsigned char nvYp2,
                      unsigned char nvZp2,
                      float l,
                      double voxelSize,
                      double binSize,
                      binID_t nbX,
                      binID_t nbY,
                      binID_t nbZ,
                      float3 LBFPoint,
                      float3 G,
                      double ts_size,
                      float expand_factor,
                      float approx_max_vel,
                      float expand_safety_param,
                      unsigned int nContactWildcards,
                      unsigned int nOwnerWildcards);

    // Put sim data array pointers in place
    void packDataPointers();
    void packTransferPointers(DEMDynamicThread*& dT);

    /// Return timing inforation for this current run
    void getTiming(std::vector<std::string>& names, std::vector<double>& vals);

    /// Reset the timers
    void resetTimers() {
        for (const auto& name : timer_names) {
            timers.GetTimer(name).reset();
        }
    }

    /// Change all entities with (user-level) family number ID_from to have a new number ID_to
    void changeFamily(unsigned int ID_from, unsigned int ID_to);

    /// Change radii and relPos info of these owners (if these owners are clumps)
    void changeOwnerSizes(const std::vector<bodyID_t>& IDs, const std::vector<float>& factors);

    // Jitify kT kernels (at initialization) based on existing knowledge of this run
    void jitifyKernels(const std::unordered_map<std::string, std::string>& Subs);

  private:
    const std::string Name = "kT";

    // Bring kT buffer array data to its working arrays
    void unpackMyBuffer();
    // Send produced data to dT-owned biffers
    void sendToTheirBuffer();
    // Resize dT's buffer arrays based on the number of contact pairs
    inline void transferArraysResize(size_t nContactPairs);

    // Just-in-time compiled kernels
    // jitify::Program bin_occupation_kernels = JitHelper::buildProgram("bin_occupation_kernels", " ");
    std::shared_ptr<jitify::Program> bin_occupation_kernels;
    std::shared_ptr<jitify::Program> contact_detection_kernels;
    std::shared_ptr<jitify::Program> history_kernels;
    std::shared_ptr<jitify::Program> misc_kernels;

};  // kT ends

}  // namespace deme

#endif
