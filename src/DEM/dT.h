//  Copyright (c) 2021, SBEL GPU Development Team
//  Copyright (c) 2021, University of Wisconsin - Madison
//
//	SPDX-License-Identifier: BSD-3-Clause

#ifndef DEME_DT
#define DEME_DT

#include <mutex>
#include <vector>
#include <thread>
#include <unordered_map>
#include <set>

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

/// DynamicThread class
class DEMDynamicThread {
  protected:
    WorkerReportChannel* pPagerToMain;
    ThreadManager* pSchedSupport;
    GpuManager* pGpuDistributor;

    // dT verbosity
    VERBOSITY verbosity = INFO;

    // Some behavior-related flags
    SolverFlags solverFlags;

    // The std::thread that binds to this instance
    std::thread th;

    // Number of items in the buffer array (which is not a managed vector, due to our need to explicitly control where
    // it is allocated)
    size_t buffer_size;

    // Object which stores the device and stream IDs for this thread
    GpuManager::StreamInfo streamInfo;

    // A class that contains scratch pad and system status data (constructed with the number of temp arrays we need)
    DEMSolverStateData stateOfSolver_resources = DEMSolverStateData(7);

    // The number of for iterations dT does for a specific user "run simulation" call
    double cycleDuration;

    // Buffer arrays for storing info from the dT side.
    // kT modifies these arrays; dT uses them only.

    // dT gets contact pair/location/history map info from kT
    // std::vector<bodyID_t, ManagedAllocator<bodyID_t>> idGeometryA_buffer;
    // std::vector<bodyID_t, ManagedAllocator<bodyID_t>> idGeometryB_buffer;
    // std::vector<contact_t, ManagedAllocator<contact_t>> contactType_buffer;
    // std::vector<contactPairs_t, ManagedAllocator<contactPairs_t>> contactMapping_buffer;

    // Pointers to simulation params-related arrays
    DEMSimParams* simParams;

    // Pointers to those data arrays defined below, stored in a struct
    DEMDataDT* granData;

    // Body-related arrays in managed memory, for dT's personal use (not transfer buffer)

    // Those are the smaller ones, the unique, template ones
    // The mass values
    std::vector<float, ManagedAllocator<float>> massOwnerBody;

    // The components of MOI values
    std::vector<float, ManagedAllocator<float>> mmiXX;
    std::vector<float, ManagedAllocator<float>> mmiYY;
    std::vector<float, ManagedAllocator<float>> mmiZZ;

    // Volume values
    std::vector<float, ManagedAllocator<float>> volumeOwnerBody;

    // The distinct sphere radii values
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

    // What type is this owner? Clump? Analytical object? Meshed object?
    std::vector<ownerType_t, ManagedAllocator<ownerType_t>> ownerTypes;

    // Those are the large ones, ones that have the same length as the number of clumps
    // The mass/MOI offsets
    std::vector<inertiaOffset_t, ManagedAllocator<inertiaOffset_t>> inertiaPropOffsets;

    // Clump's family identification code. Used in determining whether they can be contacts between two families, and
    // whether a family has prescribed motions.
    std::vector<family_t, ManagedAllocator<family_t>> familyID;

    // The (impl-level) family IDs whose entities should not be outputted to files
    std::vector<family_t, ManagedAllocator<family_t>> familiesNoOutput;

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

    // Linear velocity
    std::vector<float, ManagedAllocator<float>> vX;
    std::vector<float, ManagedAllocator<float>> vY;
    std::vector<float, ManagedAllocator<float>> vZ;

    // Local angular velocity
    std::vector<float, ManagedAllocator<float>> omgBarX;
    std::vector<float, ManagedAllocator<float>> omgBarY;
    std::vector<float, ManagedAllocator<float>> omgBarZ;

    // Linear acceleration
    std::vector<float, ManagedAllocator<float>> aX;
    std::vector<float, ManagedAllocator<float>> aY;
    std::vector<float, ManagedAllocator<float>> aZ;

    // Local angular acceleration
    std::vector<float, ManagedAllocator<float>> alphaX;
    std::vector<float, ManagedAllocator<float>> alphaY;
    std::vector<float, ManagedAllocator<float>> alphaZ;

    // Contact pair/location, for dT's personal use!!
    std::vector<bodyID_t, ManagedAllocator<bodyID_t>> idGeometryA;
    std::vector<bodyID_t, ManagedAllocator<bodyID_t>> idGeometryB;
    std::vector<contact_t, ManagedAllocator<contact_t>> contactType;
    // std::vector<contactPairs_t, ManagedAllocator<contactPairs_t>> contactMapping;

    // Some of dT's own work arrays
    // Force of each contact event. It is the force that bodyA feels.
    std::vector<float3, ManagedAllocator<float3>> contactForces;
    // An imaginary `force' in each contact event that produces torque only, and does not affect the linear motion. It
    // will rise in our default rolling resistance model, which is just a torque model; yet, our contact registration is
    // contact pair-based, meaning we do not know the specs of each contact body, so we can register force only, not
    // torque. Therefore, this vector arises.
    std::vector<float3, ManagedAllocator<float3>> contactTorque_convToForce;
    // Local position of contact point of contact w.r.t. the reference frame of body A and B
    std::vector<float3, ManagedAllocator<float3>> contactPointGeometryA;
    std::vector<float3, ManagedAllocator<float3>> contactPointGeometryB;
    // Wildcard (extra property) arrays associated with contacts and owners
    std::vector<std::vector<float, ManagedAllocator<float>>,
                ManagedAllocator<std::vector<float, ManagedAllocator<float>>>>
        contactWildcards;
    std::vector<std::vector<float, ManagedAllocator<float>>,
                ManagedAllocator<std::vector<float, ManagedAllocator<float>>>>
        ownerWildcards;
    // std::vector<float, ManagedAllocator<float>> contactWildcards[DEME_MAX_WILDCARD_NUM];
    // std::vector<float, ManagedAllocator<float>> ownerWildcards[DEME_MAX_WILDCARD_NUM];
    // An example of such wildcard arrays is contact history: how much did the contact point move on the geometry
    // surface compared to when the contact first emerged?

    // std::vector<float3, ManagedAllocator<float3>> contactHistory;
    // // Durations in time of persistent contact pairs
    // std::vector<float, ManagedAllocator<float>> contactDuration;
    // The velocity of the contact points in the global frame: can be useful in determining the time step size
    // std::vector<float3, ManagedAllocator<float3>> contactPointVel;

    size_t m_approx_bytes_used = 0;

    // Time elapsed in current simulation
    float timeElapsed = 0.f;
    // dT's total steps run (since last time the collaboration stats cache is cleared)
    uint64_t nTotalSteps = 0;

    // If true, dT needs to re-process idA- and idB-related data arrays before collecting forces, as those arrays are
    // freshly obtained from kT.
    bool contactPairArr_isFresh = true;

    // Template-related arrays in managed memory
    // Belonged-body ID
    std::vector<bodyID_t, ManagedAllocator<bodyID_t>> ownerClumpBody;
    std::vector<bodyID_t, ManagedAllocator<bodyID_t>> ownerMesh;
    std::vector<bodyID_t> ownerAnalBody;

    // The ID that maps this sphere component's geometry-defining parameters, when this component is jitified
    std::vector<clumpComponentOffset_t, ManagedAllocator<clumpComponentOffset_t>> clumpComponentOffset;
    // The ID that maps this sphere component's geometry-defining parameters, when this component is not jitified (too
    // many templates)
    std::vector<clumpComponentOffsetExt_t, ManagedAllocator<clumpComponentOffsetExt_t>> clumpComponentOffsetExt;
    // The ID that maps this analytical entity component's geometry-defining parameters, when this component is jitified
    // std::vector<clumpComponentOffset_t, ManagedAllocator<clumpComponentOffset_t>> analComponentOffset;

    // The ID that maps this entity's material
    std::vector<materialsOffset_t, ManagedAllocator<materialsOffset_t>> sphereMaterialOffset;
    std::vector<materialsOffset_t, ManagedAllocator<materialsOffset_t>> triMaterialOffset;

    // dT's copy of family map
    // std::unordered_map<unsigned int, family_t> familyUserImplMap;
    // std::unordered_map<family_t, unsigned int> familyImplUserMap;

    // A long array (usually 32640 elements) registering whether between 2 families there should be contacts
    std::vector<notStupidBool_t, ManagedAllocator<notStupidBool_t>> familyMaskMatrix;

    // dT's copy of "clump template and their names" map
    std::unordered_map<unsigned int, std::string> templateNumNameMap;

    // dT's timers
    std::vector<std::string> timer_names = {"Calculate contact forces", "Collect contact forces", "Integration",
                                            "Unpack updates from kT",   "Send to kT buffer",      "Wait for kT update"};
    SolverTimers timers = SolverTimers(timer_names);

  public:
    friend class DEMSolver;
    friend class DEMKinematicThread;

    DEMDynamicThread(WorkerReportChannel* pPager, ThreadManager* pSchedSup, GpuManager* pGpuDist)
        : pPagerToMain(pPager), pSchedSupport(pSchedSup), pGpuDistributor(pGpuDist) {
        GPU_CALL(cudaMallocManaged(&simParams, sizeof(DEMSimParams), cudaMemAttachGlobal));
        GPU_CALL(cudaMallocManaged(&granData, sizeof(DEMDataDT), cudaMemAttachGlobal));

        cycleDuration = 0;

        // Get a device/stream ID to use from the GPU Manager
        streamInfo = pGpuDistributor->getAvailableStream();

        pPagerToMain->userCallDone = false;
        pSchedSupport->dynamicShouldJoin = false;
        pSchedSupport->dynamicStarted = false;

        // Launch a worker thread bound to this instance
        th = std::move(std::thread([this]() { this->workerThread(); }));
    }
    ~DEMDynamicThread() {
        // std::cout << "Dynamic thread closing..." << std::endl;
        pSchedSupport->dynamicShouldJoin = true;
        startThread();
        th.join();
        cudaStreamDestroy(streamInfo.stream);
    }

    void setCycleDuration(double val) { cycleDuration = val; }

    // buffer exchange methods
    void setDestinationBufferPointers();

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

    /// Compute total KE of all entities
    float getKineticEnergy();

    /// Get this owner's position in user unit
    float3 getOwnerPos(bodyID_t ownerID) const;
    /// Get this owner's angular velocity
    float3 getOwnerAngVel(bodyID_t ownerID) const;
    /// Get this owner's quaternion
    float4 getOwnerOriQ(bodyID_t ownerID) const;
    /// Get this owner's velocity
    float3 getOwnerVel(bodyID_t ownerID) const;
    /// Get this owner's acceleration
    float3 getOwnerAcc(bodyID_t ownerID) const;
    /// Get this owner's angular acceleration
    float3 getOwnerAngAcc(bodyID_t ownerID) const;

    /// Set this owner's position in user unit
    void setOwnerPos(bodyID_t ownerID, float3 pos);
    /// Set this owner's angular velocity
    void setOwnerAngVel(bodyID_t ownerID, float3 angVel);
    /// Set this owner's quaternion
    void setOwnerOriQ(bodyID_t ownerID, float4 oriQ);
    /// Set this owner's velocity
    void setOwnerVel(bodyID_t ownerID, float3 vel);

    /// Change all entities with (user-level) family number ID_from to have a new number ID_to
    void changeFamily(unsigned int ID_from, unsigned int ID_to);

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

    // Components of initManagedArrays
    void buildTrackedObjs(const std::vector<std::shared_ptr<DEMClumpBatch>>& input_clump_batches,
                          const std::vector<float3>& input_ext_obj_xyz,
                          const std::vector<std::shared_ptr<DEMMeshConnected>>& input_mesh_objs,
                          std::vector<std::shared_ptr<DEMTrackedObj>>& tracked_objs,
                          size_t nExistOwners,
                          size_t nExistingFacets);
    void populateEntityArrays(const std::vector<std::shared_ptr<DEMClumpBatch>>& input_clump_batches,
                              const std::vector<float3>& input_ext_obj_xyz,
                              const std::vector<unsigned int>& input_ext_obj_family,
                              const std::vector<float3>& input_mesh_obj_xyz,
                              const std::vector<float4>& input_mesh_obj_rot,
                              const std::vector<unsigned int>& input_mesh_obj_family,
                              const std::vector<unsigned int>& mesh_facet_owner,
                              const std::vector<materialsOffset_t>& mesh_facet_materials,
                              const std::vector<DEMTriangle>& mesh_facets,
                              const ClumpTemplateFlatten& clump_templates,
                              const std::vector<float>& ext_obj_mass_types,
                              const std::vector<float3>& ext_obj_moi_types,
                              const std::vector<unsigned int>& ext_obj_comp_num,
                              const std::vector<float>& mesh_obj_mass_types,
                              const std::vector<float3>& mesh_obj_moi_types,
                              size_t nExistOwners,
                              size_t nExistSpheres,
                              size_t nExistingFacets);
    void registerPolicies(const std::unordered_map<unsigned int, std::string>& template_number_name_map,
                          const ClumpTemplateFlatten& clump_templates,
                          const std::vector<float>& ext_obj_mass_types,
                          const std::vector<float3>& ext_obj_moi_types,
                          const std::vector<float>& mesh_obj_mass_types,
                          const std::vector<float3>& mesh_obj_moi_types,
                          const std::vector<std::shared_ptr<DEMMaterial>>& loaded_materials,
                          const std::vector<notStupidBool_t>& family_mask_matrix,
                          const std::set<unsigned int>& no_output_families);

    /// Initialized managed arrays
    void initManagedArrays(const std::vector<std::shared_ptr<DEMClumpBatch>>& input_clump_batches,
                           const std::vector<float3>& input_ext_obj_xyz,
                           const std::vector<unsigned int>& input_ext_obj_family,
                           const std::vector<std::shared_ptr<DEMMeshConnected>>& input_mesh_objs,
                           const std::vector<float3>& input_mesh_obj_xyz,
                           const std::vector<float4>& input_mesh_obj_rot,
                           const std::vector<unsigned int>& input_mesh_obj_family,
                           const std::vector<unsigned int>& mesh_facet_owner,
                           const std::vector<materialsOffset_t>& mesh_facet_materials,
                           const std::vector<DEMTriangle>& mesh_facets,
                           const std::unordered_map<unsigned int, std::string>& template_number_name_map,
                           const ClumpTemplateFlatten& clump_templates,
                           const std::vector<float>& ext_obj_mass_types,
                           const std::vector<float3>& ext_obj_moi_types,
                           const std::vector<unsigned int>& ext_obj_comp_num,
                           const std::vector<float>& mesh_obj_mass_types,
                           const std::vector<float3>& mesh_obj_moi_types,
                           const std::vector<std::shared_ptr<DEMMaterial>>& loaded_materials,
                           const std::vector<notStupidBool_t>& family_mask_matrix,
                           const std::set<unsigned int>& no_output_families,
                           std::vector<std::shared_ptr<DEMTrackedObj>>& tracked_objs);

    /// Add more clumps and/or meshes into the system, without re-initialization. It must be clump/mesh-addition only,
    /// no other changes to the system.
    void updateClumpMeshArrays(const std::vector<std::shared_ptr<DEMClumpBatch>>& input_clump_batches,
                               const std::vector<float3>& input_ext_obj_xyz,
                               const std::vector<unsigned int>& input_ext_obj_family,
                               const std::vector<std::shared_ptr<DEMMeshConnected>>& input_mesh_objs,
                               const std::vector<float3>& input_mesh_obj_xyz,
                               const std::vector<float4>& input_mesh_obj_rot,
                               const std::vector<unsigned int>& input_mesh_obj_family,
                               const std::vector<unsigned int>& mesh_facet_owner,
                               const std::vector<materialsOffset_t>& mesh_facet_materials,
                               const std::vector<DEMTriangle>& mesh_facets,
                               const ClumpTemplateFlatten& clump_templates,
                               const std::vector<float>& ext_obj_mass_types,
                               const std::vector<float3>& ext_obj_moi_types,
                               const std::vector<unsigned int>& ext_obj_comp_num,
                               const std::vector<float>& mesh_obj_mass_types,
                               const std::vector<float3>& mesh_obj_moi_types,
                               const std::vector<std::shared_ptr<DEMMaterial>>& loaded_materials,
                               const std::vector<notStupidBool_t>& family_mask_matrix,
                               const std::set<unsigned int>& no_output_families,
                               std::vector<std::shared_ptr<DEMTrackedObj>>& tracked_objs,
                               size_t nExistingOwners,
                               size_t nExistingClumps,
                               size_t nExistingSpheres,
                               size_t nExistingTriMesh,
                               size_t nExistingFacets);

    /// Change radii and relPos info of these owners (if these owners are clumps)
    void changeOwnerSizes(const std::vector<bodyID_t>& IDs, const std::vector<float>& factors);

    /// Put sim data array pointers in place
    void packDataPointers();
    void packTransferPointers(DEMKinematicThread*& kT);

    void writeSpheresAsChpf(std::ofstream& ptFile) const;
    void writeSpheresAsCsv(std::ofstream& ptFile) const;
    void writeClumpsAsChpf(std::ofstream& ptFile) const;
    void writeClumpsAsCsv(std::ofstream& ptFile) const;
    void writeContactsAsCsv(std::ofstream& ptFile) const;

    /// Called each time when the user calls DoDynamicsThenSync.
    void startThread();

    // The actual kernel things go here.
    // It is called upon construction.
    void workerThread();

    // Reset kT--dT interaction coordinator stats
    void resetUserCallStat();
    // Return the approximate RAM usage
    size_t estimateMemUsage() const;

    /// Return timing inforation for this current run
    void getTiming(std::vector<std::string>& names, std::vector<double>& vals);

    /// Reset the timers
    void resetTimers() {
        for (const auto& name : timer_names) {
            timers.GetTimer(name).reset();
        }
    }

    // Jitify dT kernels (at initialization) based on existing knowledge of this run
    void jitifyKernels(const std::unordered_map<std::string, std::string>& Subs);

    // Execute this kernel, then return the reduced value
    float* inspectCall(const std::shared_ptr<jitify::Program>& inspection_kernel,
                       const std::string& kernel_name,
                       size_t n,
                       CUB_REDUCE_FLAVOR reduce_flavor,
                       bool all_domain);

  private:
    const std::string Name = "dT";

    // Number of trackers I already processed before (if I see a tracked_obj array longer than this in initialization, I
    // know I have to process the new-comers)
    unsigned int nTrackersProcessed = 0;

    // Migrate contact history to fit the structure of the newly received contact array
    inline void migratePersistentContacts();

    // Update clump-based acceleration array based on sphere-based force array
    inline void calculateForces();

    // Update clump pos/oriQ and vel/omega based on acceleration
    inline void integrateOwnerMotions();

    // If kT provides fresh CD results, we unpack and use it
    inline void ifProduceFreshThenUseItAndSendNewOrder();

    // Some per-step checks/modification, done before integration, but after force calculation (thus sort of in the
    // mid-step stage)
    inline void routineChecks();

    // Bring dT buffer array data to its working arrays
    void unpackMyBuffer();
    // Send produced data to kT-owned biffers
    void sendToTheirBuffer();
    // Resize some work arrays based on the number of contact pairs provided by kT
    void contactEventArraysResize(size_t nContactPairs);

    // Just-in-time compiled kernels
    std::shared_ptr<jitify::Program> prep_force_kernels;
    std::shared_ptr<jitify::Program> cal_force_kernels;
    std::shared_ptr<jitify::Program> collect_force_kernels;
    std::shared_ptr<jitify::Program> integrator_kernels;
    // std::shared_ptr<jitify::Program> quarry_stats_kernels;
    std::shared_ptr<jitify::Program> mod_kernels;
    std::shared_ptr<jitify::Program> misc_kernels;
};  // dT ends

}  // namespace deme

#endif
