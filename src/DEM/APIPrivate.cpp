//  Copyright (c) 2021, SBEL GPU Development Team
//  Copyright (c) 2021, University of Wisconsin - Madison
//
//	SPDX-License-Identifier: BSD-3-Clause

#include <core/ApiVersion.h>
#include <DEM/API.h>
#include <DEM/Defines.h>
#include <DEM/HostSideHelpers.hpp>

#include <iostream>
#include <fstream>
#include <thread>
#include <chrono>
#include <cstring>
#include <limits>
#include <algorithm>

namespace deme {

void DEMSolver::generatePolicyResources() {
    // Process the loaded materials. The pre-process of external objects and clumps could add more materials, so this
    // call need to go after those pre-process ones.
    figureOutMaterialProxies();

    // Based on user input, prepare family_mask_matrix (family contact map matrix)
    figureOutFamilyMasks();

    // Decide bin size (for contact detection)
    decideBinSize();
}

void DEMSolver::generateEntityResources() {
    /*
    // Dan and Ruochun decided not to extract unique input values.
    // Instead, we trust users: we simply store all clump template info users give.
    // So the unique-value-extractor block is disabled and commented.
    size_t input_num_clump_types = m_template_clump_mass.size();
    // Put unique clump mass values in a set.
    m_template_mass_types.insert(m_template_clump_mass.begin(), m_template_clump_mass.end());
    for (size_t i = 0; i < input_num_clump_types; i++) {
        // Put unique sphere radii values in a set.
        m_template_sp_radii_types.insert(m_template_sp_radii.at(i).begin(), m_template_sp_radii.at(i).end());
        // Put unique clump sphere component locations in a set.
        m_clumps_sp_location_types.insert(m_template_sp_relPos.at(i).begin(), m_template_sp_relPos.at(i).end());
    }
    // Now rearrange so the original input mass and sphere radii are now stored as the offsets to their respective
    // uniques sets.
    for (size_t i = 0; i < input_num_clump_types; i++) {
        m_template_mass_type_offset.push_back(
            std::distance(m_template_mass_types.begin(), m_template_mass_types.find(m_template_clump_mass.at(i))));
        std::vector<distinctSphereRadiiOffset_default_t> sp_radii_type_offset(m_template_sp_radii.at(i).size(), 0);
        std::vector<distinctSphereRelativePositions_default_t> sp_location_type_offset(
            m_template_sp_relPos.at(i).size(), 0);
        for (size_t j = 0; j < sp_radii_type_offset.size(); j++) {
            sp_radii_type_offset.at(j) = std::distance(m_template_sp_radii_types.begin(),
                                                       m_template_sp_radii_types.find(m_template_sp_radii.at(i).at(j)));
            sp_location_type_offset.at(j) =
                std::distance(m_clumps_sp_location_types.begin(),
                              m_clumps_sp_location_types.find(m_template_sp_relPos.at(i).at(j)));
        }
        m_template_sp_radii_type_offset.push_back(sp_radii_type_offset);
        m_clumps_sp_location_type_offset.push_back(sp_location_type_offset);
    }

    nDistinctClumpBodyTopologies = m_template_mass_types.size();
    nMatTuples = m_loaded_materials.size();

    nDistinctSphereRadii_computed = m_template_sp_radii_types.size();
    nDistinctSphereRelativePositions_computed = m_clumps_sp_location_types.size();
    */

    // Figure out the parameters related to the simulation `world', if need to
    if (!explicit_nv_override) {
        figureOutNV();
    }
    figureOutOrigin();
    addWorldBoundingBox();

    // Flatten cached clump templates (from ClumpTemplate structs to float arrays), make ready for transferring to kTdT
    preprocessClumpTemplates();

    // Flatten some input clump information, to figure out the size of the input, and their associated family numbers
    preprocessClumps();

    // Figure out info about external objects/clump templates and whether they can be jitified
    preprocessAnalyticalObjs();

    // Count how many triangle tempaltes are there and flatten them
    preprocessTriangleObjs();
}

void DEMSolver::postResourceGen() {
    // Compute stats
    updateTotalEntityNum();

    // If these `computed' numbers are larger than types like materialsOffset_t can hold, then we should error out and
    // let the user re-compile (or, should we somehow change the header automatically?)
    postResourceGenChecksAndTabKeeping();
}

void DEMSolver::updateTotalEntityNum() {
    nDistinctClumpBodyTopologies = m_template_clump_mass.size();
    nDistinctMassProperties = nDistinctClumpBodyTopologies + nExtObj + nTriMeshes;

    // Also, external objects may introduce more material types
    nMatTuples = m_loaded_materials.size();

    // Finally, with both user inputs and jit info processed, we can derive the number of owners that we have now
    nOwnerBodies = nExtObj + nOwnerClumps + nTriMeshes;
}

void DEMSolver::postResourceGenChecksAndTabKeeping() {
    // There is this very cumbersome check if the user wish to jitify clump templates
    if (jitify_clump_templates) {
        // Can we jitify all clump templates?
        bool unable_jitify_all = false;
        nDistinctClumpComponents = 0;
        nJitifiableClumpComponents = 0;
        for (unsigned int i = 0; i < nDistinctClumpBodyTopologies; i++) {
            nDistinctClumpComponents += m_template_sp_radii.at(i).size();
            // Keep an eye on if the accumulated DistinctClumpComponents gets too many
            if ((!unable_jitify_all) && (nDistinctClumpComponents > THRESHOLD_CANT_JITIFY_ALL_COMP)) {
                nJitifiableClumpTopo = i;
                nJitifiableClumpComponents = nDistinctClumpComponents - m_template_sp_radii.at(i).size();
                unable_jitify_all = true;
            }
        }
        if (unable_jitify_all) {
            DEME_WARNING(
                "There are %u clump templates loaded, but only %u templates (totalling %u components) are jitifiable "
                "due to some of the clumps are big and/or there are many types of clumps.\nIt is probably because you "
                "have external objects represented by spherical decomposition (a.k.a. have big clumps).\nIn this case, "
                "I strongly suggest against calling SetJitifyClumpTemplates to use jitification for clump templates.",
                nDistinctClumpBodyTopologies, nJitifiableClumpTopo, nJitifiableClumpComponents);
        } else {
            nJitifiableClumpTopo = nDistinctClumpBodyTopologies;
            nJitifiableClumpComponents = nDistinctClumpComponents;
        }
    }

    if (jitify_mass_moi) {
        // Sanity check for final number of mass properties/inertia offsets
        if (nDistinctMassProperties >= std::numeric_limits<inertiaOffset_t>::max()) {
            DEME_ERROR(
                "%u different mass properties (from the contribution of clump templates, analytical objects and meshed "
                "objects) are loaded, but the max allowance is %u (No.%u is reserved).\nIn general, I suggest against "
                "calling SetJitifyMassProperties to use jitification for mass properties and here, you definitely "
                "should not use it.",
                nDistinctMassProperties, std::numeric_limits<inertiaOffset_t>::max() - 1,
                std::numeric_limits<inertiaOffset_t>::max());
        }
    }

    // Do we have more bins that our data type can handle?
    if (m_num_bins > std::numeric_limits<binID_t>::max()) {
        DEME_ERROR(
            "The simulation world has %zu bins (for domain partitioning in contact detection), but the largest bin ID "
            "that we can have is %zu.\nYou can try to make bins larger via SetInitBinSize, or redefine binID_t and "
            "recompile.",
            m_num_bins, std::numeric_limits<binID_t>::max());
    }

    // Sanity check for analytical geometries
    if (nAnalGM > DEME_THRESHOLD_TOO_MANY_ANAL_GEO) {
        DEME_WARNING(
            "%u analytical geometries are loaded. Because all analytical geometries are jitified, this is a relatively "
            "large amount.\nIf just-in-time compilation fails or kernels run slowly, this could be a cause.",
            nAnalGM);
    }

    // Keep tab of some quatities... It has to be done this late, because initialization may add analytical objects to
    // the system.
    nLastTimeClumpTemplateLoad = nClumpTemplateLoad;
    nLastTimeExtObjLoad = nExtObjLoad;
    nLastTimeBatchClumpsLoad = nBatchClumpsLoad;
    nLastTimeTriObjLoad = nTriObjLoad;
    nLastTimeMatNum = m_loaded_materials.size();
    nLastTimeClumpTemplateNum = m_templates.size();
    nLastTimeFamilyPreNum = m_input_family_prescription.size();
}

void DEMSolver::addAnalCompTemplate(const objType_t type,
                                    const std::shared_ptr<DEMMaterial>& material,
                                    const unsigned int owner,
                                    const float3 pos,
                                    const float3 rot,
                                    const float d1,
                                    const float d2,
                                    const float d3,
                                    const objNormal_t normal) {
    m_anal_types.push_back(type);
    m_anal_materials.push_back(material->load_order);
    m_anal_owner.push_back(owner);
    m_anal_comp_pos.push_back(pos);
    m_anal_comp_rot.push_back(rot);
    m_anal_size_1.push_back(d1);
    m_anal_size_2.push_back(d2);
    m_anal_size_3.push_back(d3);
    m_anal_normals.push_back(normal);
}

void DEMSolver::jitifyKernels() {
    equipClumpTemplates(m_subs);
    equipSimParams(m_subs);
    equipMassMoiVolume(m_subs);
    equipMaterials(m_subs);
    equipAnalGeoTemplates(m_subs);
    equipFamilyPrescribedMotions(m_subs);
    equipFamilyOnFlyChanges(m_subs);
    equipForceModel(m_subs);
    equipIntegrationScheme(m_subs);
    kT->jitifyKernels(m_subs);
    dT->jitifyKernels(m_subs);

    // Now, inspectors need to be jitified too... but the current design jitify inspector kernels at the first time they
    // are used. for (auto& insp : m_inspectors) {
    //     insp->Initialize(Subs);
    // }
}

void DEMSolver::figureOutNV() {
    // Rank the size of XYZ, ascending
    float XYZ[3] = {m_user_boxSize.x, m_user_boxSize.y, m_user_boxSize.z};
    SPATIAL_DIR rankXYZ[3] = {SPATIAL_DIR::X, SPATIAL_DIR::Y, SPATIAL_DIR::Z};
    for (int i = 0; i < 3 - 1; i++)
        for (int j = i + 1; j < 3; j++)
            if (XYZ[i] > XYZ[j]) {
                elemSwap(XYZ + i, XYZ + j);
                elemSwap(rankXYZ + i, rankXYZ + j);
            }
    // Record the size ranking
    float userSize321[3] = {XYZ[0], XYZ[1], XYZ[2]};
    // Inspect how many times larger the larger one is. Say it is 2 times larger, then one more bit is given to the
    // larger one; say 4 times larger, then 2 more bits are given to the larger one. If in between (2, 4), then if it's
    // more than sqrt(2) * 2 times larger, then 2 morebits; otherwise, 1 more bit. Why? Do that math then maybe you can
    // agree this wastes as little bits as possible.
    int n_more_bits_for_me[2] = {0, 0};
    while (XYZ[0] < XYZ[1]) {
        if (sqrt(2.) * XYZ[0] > XYZ[1]) {
            break;
        }
        n_more_bits_for_me[0]++;
        XYZ[0] *= 2.;
    }
    while (XYZ[1] < XYZ[2]) {
        if (sqrt(2.) * XYZ[1] > XYZ[2]) {
            break;
        }
        n_more_bits_for_me[1]++;
        XYZ[1] *= 2.;
    }

    DEME_DEBUG_PRINTF("2nd place uses %d more bits than 3rd, and 1st place uses %d more bits than 2rd.",
                      n_more_bits_for_me[0], n_more_bits_for_me[1]);

    // Then we know how many bits each one would have
    int base_bits = ((int)VOXEL_COUNT_POWER2 - n_more_bits_for_me[0] - n_more_bits_for_me[1]) / 3;
    int left_over = ((int)VOXEL_COUNT_POWER2 - n_more_bits_for_me[0] - n_more_bits_for_me[1]) % 3;
    int bits_3rd = base_bits;
    int bits_2nd = bits_3rd + n_more_bits_for_me[0];
    int bits_1st = bits_2nd + n_more_bits_for_me[1];
    while (left_over > 0) {
        // Try giving to losers... unless the loser did not suffer bits penalty, in which case give to the larger one
        if (bits_3rd < bits_2nd) {
            bits_3rd++;
        } else if (bits_2nd < bits_1st) {
            bits_2nd++;
        } else {
            bits_1st++;
        }
        left_over--;
    }
    DEME_DEBUG_PRINTF("After assigning left-overs, 3rd, 2nd and 1st have bits: %d, %d, %d.", bits_3rd, bits_2nd,
                      bits_1st);

    int bits[3] = {bits_3rd, bits_2nd, bits_1st};
    if (m_box_dir_length_is_exact == SPATIAL_DIR::NONE) {
        // Have to use the largest l, given the voxel budget
        double l3 =
            (double)userSize321[0] / (double)std::pow(2., (int)VOXEL_RES_POWER2) / (double)std::pow(2., bits_3rd);
        double l2 =
            (double)userSize321[1] / (double)std::pow(2., (int)VOXEL_RES_POWER2) / (double)std::pow(2., bits_2nd);
        double l1 =
            (double)userSize321[2] / (double)std::pow(2., (int)VOXEL_RES_POWER2) / (double)std::pow(2., bits_1st);
        l = std::max(l3, std::max(l2, l1));
    } else {
        // Find which dir user wants to be exact
        int exact_dir_no = find_array_offset(rankXYZ, m_box_dir_length_is_exact, 3);
        int not_exact_dir[2];
        if (exact_dir_no == 0) {
            not_exact_dir[0] = 1;
            not_exact_dir[1] = 2;
        } else if (exact_dir_no == 1) {
            not_exact_dir[0] = 0;
            not_exact_dir[1] = 2;
        } else {
            not_exact_dir[0] = 0;
            not_exact_dir[1] = 1;
        }
        // We hope this l is big enough...
        l = (double)userSize321[exact_dir_no] / (double)std::pow(2., (int)VOXEL_RES_POWER2) /
            (double)std::pow(2., bits[exact_dir_no]);
        while (l * (double)std::pow(2., (int)VOXEL_RES_POWER2) * (double)std::pow(2., bits[not_exact_dir[1]]) <
               userSize321[not_exact_dir[1]]) {
            // Borrow a bit from this dir...
            bits[exact_dir_no] -= 1;
            bits[not_exact_dir[1]] += 1;
            l = (double)userSize321[exact_dir_no] / (double)std::pow(2., (int)VOXEL_RES_POWER2) /
                (double)std::pow(2., bits[exact_dir_no]);
        }
        while (l * (double)std::pow(2., (int)VOXEL_RES_POWER2) * (double)std::pow(2., bits[not_exact_dir[0]]) <
               userSize321[not_exact_dir[0]]) {
            // Borrow a bit from this dir...
            bits[exact_dir_no] -= 1;
            bits[not_exact_dir[0]] += 1;
            l = (double)userSize321[exact_dir_no] / (double)std::pow(2., (int)VOXEL_RES_POWER2) /
                (double)std::pow(2., bits[exact_dir_no]);
        }
    }
    DEME_DEBUG_PRINTF(
        "After final tweak concerning possible exact direction requirements, 3rd, 2nd and 1st have bits: %d, %d, %d.",
        bits[0], bits[1], bits[2]);
    nvXp2 = bits[find_array_offset(rankXYZ, SPATIAL_DIR::X, 3)];
    nvYp2 = bits[find_array_offset(rankXYZ, SPATIAL_DIR::Y, 3)];
    nvZp2 = bits[find_array_offset(rankXYZ, SPATIAL_DIR::Z, 3)];

    // Calculating `world' size by the input nvXp2 and l
    m_voxelSize = (double)((size_t)1 << VOXEL_RES_POWER2) * (double)l;
    m_boxX = m_voxelSize * (double)((size_t)1 << nvXp2);
    m_boxY = m_voxelSize * (double)((size_t)1 << nvYp2);
    m_boxZ = m_voxelSize * (double)((size_t)1 << nvZp2);
}

void DEMSolver::decideBinSize() {
    // find the smallest radius
    for (auto elem : m_template_sp_radii) {
        for (auto radius : elem) {
            if (radius < m_smallest_radius) {
                m_smallest_radius = radius;
            }
        }
    }

    // TODO: What should be a default bin size?
    if (m_smallest_radius > DEME_TINY_FLOAT) {
        if (!use_user_defined_bin_size) {
            m_binSize = 2.0 * m_smallest_radius;
        }
    } else {
        if (!use_user_defined_bin_size) {
            DEME_ERROR(
                "There are spheres in clump templates that have 0 radii, and the user did not specify the bin size "
                "(for contact detection)!\nBecause the bin size is supposed to be defaulted to the size of the "
                "smallest sphere, now the solver does not know what to do.");
        } else {
            DEME_WARNING(
                "There are spheres in clump templates that have 0 radii!! Please make sure this is intentional.");
        }
    }

    nbX = (binID_t)(m_voxelSize * (double)((size_t)1 << nvXp2) / m_binSize) + 1;
    nbY = (binID_t)(m_voxelSize * (double)((size_t)1 << nvYp2) / m_binSize) + 1;
    nbZ = (binID_t)(m_voxelSize * (double)((size_t)1 << nvZp2) / m_binSize) + 1;
    m_num_bins = (uint64_t)nbX * (uint64_t)nbY * (uint64_t)nbZ;
    // It's better to compute num of bins this way, rather than...
    // (uint64_t)(m_boxX / m_binSize + 1) * (uint64_t)(m_boxY / m_binSize + 1) * (uint64_t)(m_boxZ / m_binSize + 1);
    // because the space bins and voxels can cover may be larger than the user-defined sim domain
}

void DEMSolver::reportInitStats() const {
    DEME_INFO("Number of total active devices: %d", dTkT_GpuManager->getNumDevices());

    DEME_INFO("The dimension of the simulation world: %.17g, %.17g, %.17g", m_boxX, m_boxY, m_boxZ);
    DEME_INFO("Simulation world X range: [%.7g, %.7g]", m_boxLBF.x, m_boxLBF.x + m_boxX);
    DEME_INFO("Simulation world Y range: [%.7g, %.7g]", m_boxLBF.y, m_boxLBF.y + m_boxY);
    DEME_INFO("Simulation world Z range: [%.7g, %.7g]", m_boxLBF.z, m_boxLBF.z + m_boxZ);
    DEME_INFO("User-specified dimensions are not larger than the above simulation world.");
    DEME_INFO("User-specified X-dimension range: [%.7g, %.7g]", m_boxLBF.x, m_boxLBF.x + m_user_boxSize.x);
    DEME_INFO("User-specified Y-dimension range: [%.7g, %.7g]", m_boxLBF.y, m_boxLBF.y + m_user_boxSize.y);
    DEME_INFO("User-specified Z-dimension range: [%.7g, %.7g]", m_boxLBF.z, m_boxLBF.z + m_user_boxSize.z);
    DEME_INFO("The length unit in this simulation is: %.17g", l);
    DEME_INFO("The edge length of a voxel: %.17g", m_voxelSize);

    DEME_INFO("The edge length of a bin: %.17g", m_binSize);
    DEME_INFO("The total number of bins: %zu", m_num_bins);

    DEME_INFO("The total number of clumps: %zu", nOwnerClumps);
    DEME_INFO("The combined number of component spheres: %zu", nSpheresGM);
    DEME_INFO("The total number of analytical objects: %u", nExtObj);
    DEME_INFO("The total number of meshes: %u", nTriMeshes);
    DEME_INFO("Grand total number of owners: %zu", nOwnerBodies);

    if (m_expand_factor > 0.0) {
        DEME_INFO("All geometries are enlarged/thickened by %.9g for contact detection purpose", m_expand_factor);
        DEME_INFO("This in the case of the smallest sphere, means enlarging radius by %.9g%%",
                  (m_expand_factor / m_smallest_radius) * 100.0);
    }

    DEME_INFO("The number of material types: %u", nMatTuples);
    switch (m_force_model->type) {
        case (FORCE_MODEL::HERTZIAN):
            DEME_INFO("History-based Hertzian contact model is in use");
            break;
        case (FORCE_MODEL::HERTZIAN_FRICTIONLESS):
            DEME_INFO("Frictionless Hertzian contact model is in use");
            break;
        case (FORCE_MODEL::CUSTOM):
            DEME_INFO("A user-custom force model is in use");
            break;
        default:
            DEME_INFO("An unknown force model is in use, this is probably not going well...");
    }

    // Debug outputs
    DEME_DEBUG_EXEC(printf("These owners are tracked: ");
                    for (const auto& tracked
                         : m_tracked_objs) { printf("%zu, ", tracked->ownerID); } printf("\n"););
}

void DEMSolver::preprocessAnalyticalObjs() {
    // nExtObj can increase in mid-simulation if the user re-initialize using an `Add' flavor
    nExtObj += cached_extern_objs.size();
    unsigned int thisExtObj = 0;
    for (const auto& ext_obj : cached_extern_objs) {
        // Load mass and MOI properties into arrays waiting to be transfered to kTdT
        m_ext_obj_mass.push_back(ext_obj->mass);
        m_ext_obj_moi.push_back(ext_obj->MOI);

        //// TODO: If CoM is not all-0, all components should be offsetted
        // float3 CoM = ext_obj->CoM;
        // float4 CoM_oriQ = ext_obj->CoM_oriQ;

        // Then load this ext obj's components
        unsigned int this_num_anal_ent = 0;
        auto comp_params = ext_obj->entity_params;
        auto comp_mat = ext_obj->materials;
        m_input_ext_obj_xyz.push_back(ext_obj->init_pos);
        //// TODO: init_oriQ?????
        m_input_ext_obj_family.push_back(ext_obj->family_code);
        for (unsigned int i = 0; i < ext_obj->types.size(); i++) {
            auto param = comp_params.at(this_num_anal_ent);
            this_num_anal_ent++;
            switch (ext_obj->types.at(i)) {
                case OBJ_COMPONENT::PLANE:
                    addAnalCompTemplate(ANAL_OBJ_TYPE_PLANE, comp_mat.at(i), thisExtObj, param.plane.position,
                                        param.plane.normal);
                    break;
                case OBJ_COMPONENT::PLATE:
                    addAnalCompTemplate(ANAL_OBJ_TYPE_PLATE, comp_mat.at(i), thisExtObj, param.plate.center,
                                        param.plate.normal, param.plate.h_dim_x, param.plate.h_dim_y);
                    break;
                default:
                    DEME_ERROR("There is at least one analytical boundary that has a type not supported.");
            }
        }
        nAnalGM += this_num_anal_ent;
        m_ext_obj_comp_num.push_back(this_num_anal_ent);
        thisExtObj++;
    }
}

void DEMSolver::preprocessClumpTemplates() {
    // We really only have to sort clump templates if we wish to jitify clump templates
    if (jitify_clump_templates) {
        // A sort based on the number of components of each clump type is needed, so larger clumps are near the end of
        // the array, so we can always jitify the smaller clumps, and leave larger ones in GPU global memory
        std::sort(m_templates.begin(), m_templates.end(),
                  [](auto& left, auto& right) { return left->nComp < right->nComp; });
        // A mapping is needed to transform the user-defined clump type array so that it matches the new, rearranged
        // clump template array
        std::unordered_map<unsigned int, unsigned int> old_mark_to_new;
        for (unsigned int i = 0; i < m_templates.size(); i++) {
            old_mark_to_new[m_templates.at(i)->mark] = i;
            DEME_DEBUG_PRINTF("Clump template re-order: %u->%u, nComp: %u", m_templates.at(i)->mark, i,
                              m_templates.at(i)->nComp);
        }
        // If the user then add more clumps to the system (without adding templates, which mandates a
        // re-initialization), mapping again is not needed, because now we redefine each template's mark to be the same
        // as their current position in template array
        for (unsigned int i = 0; i < m_templates.size(); i++) {
            m_templates.at(i)->mark = i;
        }
    }

    // Build the clump template number--name map
    for (const auto& clump_template : m_templates) {
        m_template_number_name_map[clump_template->mark] = clump_template->m_name;
    }

    // Now we can flatten clump templates
    for (const auto& clump : m_templates) {
        m_template_clump_mass.push_back(clump->mass);
        m_template_clump_moi.push_back(clump->MOI);
        m_template_sp_radii.push_back(clump->radii);
        m_template_sp_relPos.push_back(clump->relPos);
        m_template_clump_volume.push_back(clump->volume);

        // m_template_sp_mat_ids is an array of ints that represent the indices of the material array
        std::vector<unsigned int> this_clump_sp_mat_ids;
        for (const std::shared_ptr<DEMMaterial>& this_material : clump->materials) {
            this_clump_sp_mat_ids.push_back(this_material->load_order);
        }
        m_template_sp_mat_ids.push_back(this_clump_sp_mat_ids);
        DEME_DEBUG_EXEC(printf("Input clump No.%d has material types: ", m_template_clump_mass.size() - 1);
                        for (unsigned int i = 0; i < this_clump_sp_mat_ids.size();
                             i++) { printf("%d, ", this_clump_sp_mat_ids.at(i)); } printf("\n"););
    }
}

void DEMSolver::preprocessClumps() {
    for (const auto& a_batch : cached_input_clump_batches) {
        nOwnerClumps += a_batch->GetNumClumps();
        for (size_t i = 0; i < a_batch->GetNumClumps(); i++) {
            unsigned int nComp = a_batch->types.at(i)->nComp;
            nSpheresGM += nComp;
        }
        // Family number is flattened here, only because figureOutFamilyMasks() needs it
        m_input_clump_family.insert(m_input_clump_family.end(), a_batch->families.begin(), a_batch->families.end());
    }
}

void DEMSolver::preprocessTriangleObjs() {
    nTriMeshes += cached_mesh_objs.size();
    unsigned int thisMeshObj = 0;
    for (const auto& mesh_obj : cached_mesh_objs) {
        if (!(mesh_obj->isMaterialSet)) {
            DEME_ERROR(
                "A meshed object is loaded by does not have associated material.\nPlease assign material to meshes via "
                "SetMaterial.");
        }
        m_mesh_obj_mass.push_back(mesh_obj->mass);
        m_mesh_obj_moi.push_back(mesh_obj->MOI);
        //// TODO: If CoM is not all-0, all components should be offsetted
        // float3 CoM = ext_obj->CoM;
        // float4 CoM_oriQ = ext_obj->CoM_oriQ;

        m_input_mesh_obj_xyz.push_back(mesh_obj->init_pos);
        m_input_mesh_obj_rot.push_back(mesh_obj->init_oriQ);
        m_input_mesh_obj_family.push_back(mesh_obj->family_code);
        m_mesh_facet_owner.insert(m_mesh_facet_owner.end(), mesh_obj->GetNumTriangles(), thisMeshObj);
        for (unsigned int i = 0; i < mesh_obj->GetNumTriangles(); i++) {
            m_mesh_facet_materials.push_back(mesh_obj->materials.at(i)->load_order);
            DEMTriangle tri = mesh_obj->GetTriangle(i);
            // If we wish to correct surface orientation based on given vertex normals, rather than using RHR...
            if (mesh_obj->use_mesh_normals) {
                int normal_i = mesh_obj->face_n_indices.at(i).x;  // normals at each vertex of this triangle
                float3 normal = mesh_obj->normals.at(normal_i);

                // Generate normal using RHR from nodes 1, 2, and 3
                float3 AB = tri.p2 - tri.p1;
                float3 AC = tri.p3 - tri.p1;
                float3 cross_product = cross(AB, AC);

                // If the normal created by a RHR traversal is not correct, switch two vertices
                if (dot(cross_product, normal) < 0) {
                    float3 tmp = tri.p2;
                    tri.p2 = tri.p3;
                    tri.p3 = tmp;
                }
            }
            m_mesh_facets.push_back(tri);
        }

        nTriGM += mesh_obj->GetNumTriangles();
        thisMeshObj++;
    }
}

void DEMSolver::figureOutMaterialProxies() {
    // It now got completely integrated to the jitification part
}

void DEMSolver::figureOutFamilyMasks() {
    // Figure out the unique family numbers for a sanity check
    std::vector<unsigned int> unique_clump_families = hostUniqueVector<unsigned int>(m_input_clump_family);
    if (any_of(unique_clump_families.begin(), unique_clump_families.end(),
               [](unsigned int i) { return i == RESERVED_FAMILY_NUM; })) {
        DEME_WARNING(
            "Some clumps are instructed to have family number %u.\nThis family number is reserved for "
            "completely fixed boundaries. Using it on your simulation entities will make them fixed, regardless of "
            "your specification.\nYou can change family_t if you indeed need more families to work with.",
            RESERVED_FAMILY_NUM);
    }

    // We always know the size of the mask matrix, and we init it as all-allow
    m_family_mask_matrix.clear();
    m_family_mask_matrix.resize((NUM_AVAL_FAMILIES - 1) * NUM_AVAL_FAMILIES / 2, DONT_PREVENT_CONTACT);

    // Then we figure out the masks
    for (const auto& a_pair : m_input_no_contact_pairs) {
        // Convert user-input pairs into impl-level pairs
        unsigned int implID1 = a_pair.ID1;
        unsigned int implID2 = a_pair.ID2;
        // Now fill in the mask matrix
        unsigned int posInMat = locateMaskPair<unsigned int>(implID1, implID2);
        m_family_mask_matrix.at(posInMat) = PREVENT_CONTACT;
    }

    // Then, figure out each family's prescription info and put it into an array
    // Multiple user prescription input entries can work on the same array entry
    m_unique_family_prescription.resize(NUM_AVAL_FAMILIES);
    for (const auto& preInfo : m_input_family_prescription) {
        unsigned int user_family = preInfo.family;

        auto& this_family_info = m_unique_family_prescription.at(user_family);

        this_family_info.used = true;
        this_family_info.family = user_family;
        if (preInfo.linPosX != "none")
            this_family_info.linPosX = preInfo.linPosX;
        if (preInfo.linPosY != "none")
            this_family_info.linPosY = preInfo.linPosY;
        if (preInfo.linPosZ != "none")
            this_family_info.linPosZ = preInfo.linPosZ;
        if (preInfo.oriQ != "none")
            this_family_info.oriQ = preInfo.oriQ;
        if (preInfo.linVelX != "none")
            this_family_info.linVelX = preInfo.linVelX;
        if (preInfo.linVelY != "none")
            this_family_info.linVelY = preInfo.linVelY;
        if (preInfo.linVelZ != "none")
            this_family_info.linVelZ = preInfo.linVelZ;
        if (preInfo.rotVelX != "none")
            this_family_info.rotVelX = preInfo.rotVelX;
        if (preInfo.rotVelY != "none")
            this_family_info.rotVelY = preInfo.rotVelY;
        if (preInfo.rotVelZ != "none")
            this_family_info.rotVelZ = preInfo.rotVelZ;
        this_family_info.linVelPrescribed = this_family_info.linVelPrescribed || preInfo.linVelPrescribed;
        this_family_info.rotVelPrescribed = this_family_info.rotVelPrescribed || preInfo.rotVelPrescribed;
        this_family_info.rotPosPrescribed = this_family_info.rotPosPrescribed || preInfo.rotPosPrescribed;
        this_family_info.linPosPrescribed = this_family_info.linPosPrescribed || preInfo.linPosPrescribed;

        this_family_info.externPos = this_family_info.externPos || preInfo.externPos;
        this_family_info.externVel = this_family_info.externVel || preInfo.externVel;

        DEME_DEBUG_PRINTF("User family %u has prescribed lin vel: %s, %s, %s", user_family,
                          this_family_info.linVelX.c_str(), this_family_info.linVelY.c_str(),
                          this_family_info.linVelZ.c_str());
        DEME_DEBUG_PRINTF("User family %u has prescribed ang vel: %s, %s, %s", user_family,
                          this_family_info.rotVelX.c_str(), this_family_info.rotVelY.c_str(),
                          this_family_info.rotVelZ.c_str());
    }
}

void DEMSolver::figureOutOrigin() {
    if (m_user_instructed_origin == "explicit") {
        return;
    }
    float3 O;
    if (m_user_instructed_origin == "center") {
        O = -(m_user_boxSize) / 2.0;
        m_boxLBF = O;
    } else {
        DEME_ERROR("Unrecognized location of system origin.");
    }
}

void DEMSolver::addWorldBoundingBox() {
    // Now, add the bounding box for the simulation `world' if instructed.
    // Note the positions to add these planes are determined by the user-wanted box sizes, not m_boxXYZ which is the max
    // possible box size.
    if (m_user_add_bounding_box == "none")
        return;
    auto box = this->AddExternalObject();
    box->AddPlane(host_make_float3(m_boxLBF.x + m_user_boxSize.x / 2., m_boxLBF.y + m_user_boxSize.y / 2., m_boxLBF.z),
                  host_make_float3(0, 0, 1), m_bounding_box_material);
    if (m_user_add_bounding_box == "only_bottom")
        return;
    box->AddPlane(host_make_float3(m_boxLBF.x, m_boxLBF.y + m_user_boxSize.y / 2., m_boxLBF.z + m_user_boxSize.z / 2.),
                  host_make_float3(1, 0, 0), m_bounding_box_material);
    box->AddPlane(host_make_float3(m_boxLBF.x + m_user_boxSize.x, m_boxLBF.y + m_user_boxSize.y / 2.,
                                   m_boxLBF.z + m_user_boxSize.z / 2.),
                  host_make_float3(-1, 0, 0), m_bounding_box_material);
    box->AddPlane(host_make_float3(m_boxLBF.x + m_user_boxSize.x / 2., m_boxLBF.y, m_boxLBF.z + m_user_boxSize.z / 2.),
                  host_make_float3(0, 1, 0), m_bounding_box_material);
    box->AddPlane(host_make_float3(m_boxLBF.x + m_user_boxSize.x / 2., m_boxLBF.y + m_user_boxSize.y,
                                   m_boxLBF.z + m_user_boxSize.z / 2.),
                  host_make_float3(0, -1, 0), m_bounding_box_material);
    if (m_user_add_bounding_box == "top_open")
        return;
    // Finally, if == all, add a top boundary
    box->AddPlane(host_make_float3(m_boxLBF.x + m_user_boxSize.x / 2., m_boxLBF.y + m_user_boxSize.y / 2.,
                                   m_boxLBF.z + m_user_boxSize.z),
                  host_make_float3(0, 0, -1), m_bounding_box_material);
}

// This is generally used to pass individual instructions on how the solver should behave
void DEMSolver::transferSolverParams() {
    kT->verbosity = verbosity;
    dT->verbosity = verbosity;

    // I/O policies (only output content matters for worker threads)
    dT->solverFlags.outputFlags = m_out_content;
    dT->solverFlags.cntOutFlags = m_cnt_out_content;

    // Transfer historyless-ness
    kT->solverFlags.isHistoryless = (m_force_model->m_contact_wildcards.size() == 0);
    dT->solverFlags.isHistoryless = (m_force_model->m_contact_wildcards.size() == 0);

    // Time step constant-ness and expand factor constant-ness
    dT->solverFlags.isStepConst = ts_size_is_const;
    kT->solverFlags.isExpandFactorFixed = use_user_defined_expand_factor;

    // Jitify or not
    dT->solverFlags.useClumpJitify = jitify_clump_templates;
    dT->solverFlags.useMassJitify = jitify_mass_moi;
    kT->solverFlags.useClumpJitify = jitify_clump_templates;

    // CD strategy
    kT->solverFlags.useOneBinPerThread = use_one_bin_per_thread;

    // Tell kT and dT if this run is async
    kT->solverFlags.isAsync = !(m_updateFreq == 0);
    dT->solverFlags.isAsync = !(m_updateFreq == 0);
    // Make sure dT kT understand the lock--waiting policy of this run
    dTkT_InteractionManager->dynamicRequestedUpdateFrequency = m_updateFreq;

    // Tell kT and dT whether the user enforeced potential on-the-fly family number changes
    kT->solverFlags.canFamilyChange = famnum_can_change_conditionally;
    dT->solverFlags.canFamilyChange = famnum_can_change_conditionally;

    kT->solverFlags.should_sort_pairs = kT_should_sort;

    // NOTE: compact force calculation (in the hope to use shared memory) is not implemented
    kT->solverFlags.use_compact_force_kernel = use_compact_sweep_force_strat;
}

void DEMSolver::transferSimParams() {
    // Figure out ts size and the envelope to add to geometries (for CD safety)
    if ((!use_user_defined_expand_factor) && ts_size_is_const) {
        m_expand_factor = m_approx_max_vel * m_ts_size * m_updateFreq * m_expand_safety_param;
    }
    if (m_expand_factor * m_expand_safety_param <= 0.0 && m_updateFreq > 0 && ts_size_is_const) {
        DEME_WARNING(
            "You instructed that the physics can stretch %u time steps into the future, but did not instruct the "
            "geometries to expand via SetExpandFactor or SetMaxVelocity. The contact detection procedure will likely "
            "fail to detect some contact events before it is too late, hindering the simulation accuracy and "
            "stability.",
            m_updateFreq);
    }
    // Compute the number of wildcards in our force model
    unsigned int nContactWildcards = m_force_model->m_contact_wildcards.size();
    unsigned int nOwnerWildcards = m_force_model->m_owner_wildcards.size();
    if (nContactWildcards > DEME_MAX_WILDCARD_NUM || nOwnerWildcards > DEME_MAX_WILDCARD_NUM) {
        DEME_ERROR(
            "You defined too many contact or owner wildcards! Currently the max amount is %u.\nYou can change constant "
            "DEME_MAX_WILDCARD_NUM and re-compile, if you indeed would like more wildcards.",
            DEME_MAX_WILDCARD_NUM);
    }
    DEME_DEBUG_PRINTF("%u contact wildcards are in the force model.", nContactWildcards);

    dT->setSimParams(nvXp2, nvYp2, nvZp2, l, m_voxelSize, m_binSize, nbX, nbY, nbZ, m_boxLBF, G, m_ts_size,
                     m_expand_factor, m_approx_max_vel, m_expand_safety_param, nContactWildcards, nOwnerWildcards);
    kT->setSimParams(nvXp2, nvYp2, nvZp2, l, m_voxelSize, m_binSize, nbX, nbY, nbZ, m_boxLBF, G, m_ts_size,
                     m_expand_factor, m_approx_max_vel, m_expand_safety_param, nContactWildcards, nOwnerWildcards);
}

void DEMSolver::allocateGPUArrays() {
    // Resize managed arrays based on the statistical data we had from the previous step
    std::thread dThread = std::move(std::thread([this]() {
        this->dT->allocateManagedArrays(this->nOwnerBodies, this->nOwnerClumps, this->nExtObj, this->nTriMeshes,
                                        this->nSpheresGM, this->nTriGM, this->nAnalGM, this->nDistinctMassProperties,
                                        this->nDistinctClumpBodyTopologies, this->nDistinctClumpComponents,
                                        this->nJitifiableClumpComponents, this->nMatTuples);
    }));
    std::thread kThread = std::move(std::thread([this]() {
        this->kT->allocateManagedArrays(this->nOwnerBodies, this->nOwnerClumps, this->nExtObj, this->nTriMeshes,
                                        this->nSpheresGM, this->nTriGM, this->nAnalGM, this->nDistinctMassProperties,
                                        this->nDistinctClumpBodyTopologies, this->nDistinctClumpComponents,
                                        this->nJitifiableClumpComponents, this->nMatTuples);
    }));
    dThread.join();
    kThread.join();
}

void DEMSolver::initializeGPUArrays() {
    // Pack clump templates together... that's easier to pass to dT kT
    ClumpTemplateFlatten flattened_clump_templates(m_template_clump_mass, m_template_clump_moi, m_template_sp_mat_ids,
                                                   m_template_sp_radii, m_template_sp_relPos, m_template_clump_volume);

    // Now we can feed those GPU-side arrays with the cached API-level simulation info
    dT->initManagedArrays(
        // Clump batchs' initial stats
        cached_input_clump_batches,
        // Analytical objects' initial stats
        m_input_ext_obj_xyz, m_input_ext_obj_family,
        // Meshed objects' initial stats
        cached_mesh_objs, m_input_mesh_obj_xyz, m_input_mesh_obj_rot, m_input_mesh_obj_family, m_mesh_facet_owner,
        m_mesh_facet_materials, m_mesh_facets,
        // Clump template name mapping
        m_template_number_name_map,
        // Clump template info (mass, sphere components, materials etc.)
        flattened_clump_templates,
        // Analytical obj `template' properties
        m_ext_obj_mass, m_ext_obj_moi, m_ext_obj_comp_num,
        // Meshed obj `template' properties
        m_mesh_obj_mass, m_mesh_obj_moi,
        // Universal template info
        m_loaded_materials,
        // Family mask
        m_family_mask_matrix,
        // I/O and misc.
        m_no_output_families, m_tracked_objs);

    kT->initManagedArrays(
        // Clump batchs' initial stats
        cached_input_clump_batches,
        // Analytical objects' initial stats
        m_input_ext_obj_family,
        // Meshed objects' initial stats
        m_input_mesh_obj_family, m_mesh_facet_owner, m_mesh_facets,
        // Family mask
        m_family_mask_matrix,
        // Templates and misc.
        flattened_clump_templates);
}

/// When more clumps/meshed objects got loaded, this method should be called to transfer them to the GPU-side in
/// mid-simulation. This method cannot handle the addition of extra templates or analytical entities, which require
/// re-compilation.
void DEMSolver::updateClumpMeshArrays(size_t nOwners,
                                      size_t nClumps,
                                      size_t nSpheres,
                                      size_t nTriMesh,
                                      size_t nFacets) {
    // Pack clump templates together... that's easier to pass to dT kT
    ClumpTemplateFlatten flattened_clump_templates(m_template_clump_mass, m_template_clump_moi, m_template_sp_mat_ids,
                                                   m_template_sp_radii, m_template_sp_relPos, m_template_clump_volume);

    dT->updateClumpMeshArrays(
        // Clump batchs' initial stats
        cached_input_clump_batches,
        // Analytical objects' initial stats
        m_input_ext_obj_xyz, m_input_ext_obj_family,
        // Meshed objects' initial stats
        cached_mesh_objs, m_input_mesh_obj_xyz, m_input_mesh_obj_rot, m_input_mesh_obj_family, m_mesh_facet_owner,
        m_mesh_facet_materials, m_mesh_facets,
        // Clump template info (mass, sphere components, materials etc.)
        flattened_clump_templates,
        // Analytical obj `template' properties
        m_ext_obj_mass, m_ext_obj_moi, m_ext_obj_comp_num,
        // Meshed obj `template' properties
        m_mesh_obj_mass, m_mesh_obj_moi,
        // Universal template info
        m_loaded_materials,
        // Family mask
        m_family_mask_matrix,
        // I/O and misc.
        m_no_output_families, m_tracked_objs,
        // Number of entities, old
        nOwners, nClumps, nSpheres, nTriMesh, nFacets);
    kT->updateClumpMeshArrays(
        // Clump batchs' initial stats
        cached_input_clump_batches,
        // Analytical objects' initial stats
        m_input_ext_obj_family,
        // Meshed objects' initial stats
        m_input_mesh_obj_family, m_mesh_facet_owner, m_mesh_facets,
        // Family mask
        m_family_mask_matrix,
        // Templates and misc.
        flattened_clump_templates,
        // Number of entities, old
        nOwners, nClumps, nSpheres, nTriMesh, nFacets);
}

void DEMSolver::packDataPointers() {
    dT->packDataPointers();
    kT->packDataPointers();
    // Each worker thread needs pointers used for data transfering. Note this step must be done after packDataPointers
    // are called, so each thread has its own pointers packed.
    dT->packTransferPointers(kT);
    kT->packTransferPointers(dT);
}

void DEMSolver::validateUserInputs() {
    // Then some checks...
    // if (m_templates.size() == 0) {
    //     DEME_ERROR("Before initializing the system, at least one clump type should be defined via
    //     LoadClumpType.");
    // }

    if (m_user_boxSize.x <= 0.f || m_user_boxSize.y <= 0.f || m_user_boxSize.z <= 0.f) {
        DEME_ERROR(
            "The size of the simulation world is set to be (or default to be) %f by %f by %f. It is impossibly small.",
            m_user_boxSize.x, m_user_boxSize.y, m_user_boxSize.z);
    }

    if (m_updateFreq < 0) {
        DEME_WARNING(
            "The physics of the DEM system can drift into the future as much as it wants compared to contact "
            "detections, because SetCDUpdateFreq was called with a negative argument. Please make sure this is "
            "intended.");
    }

    if ((!ts_size_is_const) && (!use_user_defined_expand_factor) && (m_approx_max_vel <= 0.f)) {
        DEME_ERROR(
            "When using variable time step size, this solver needs your approximated maximum body/point velocity to "
            "add margins for a safe contact detection.\nYou should either supply that via SetMaxVelocity, or "
            "explicitly set a expand factor via SetExpandFactor.");
    }

    // Fix the reserved family (reserved family number is in user family, not in impl family)
    SetFamilyFixed(RESERVED_FAMILY_NUM);
}

// inline unsigned int stash_material_in_templates(std::vector<std::shared_ptr<DEMMaterial>>& loaded_materials,
//                                                 const std::shared_ptr<DEMMaterial>& this_material) {
//     auto is_same = [&](const std::shared_ptr<DEMMaterial>& ptr) { return is_material_same(ptr, this_material); };
//     // Is this material already loaded? (most likely yes)
//     auto it_mat = std::find_if(loaded_materials.begin(), loaded_materials.end(), is_same);
//     if (it_mat != loaded_materials.end()) {
//         // Already in, then just get where it's located in the m_loaded_materials array
//         return std::distance(loaded_materials.begin(), it_mat);
//     } else {
//         // Not already in, come on. Load it, and then get it into this_clump_sp_mat_ids. This is unlikely, unless the
//         // users made a shared_ptr themselves.
//         loaded_materials.push_back(this_material);
//         return loaded_materials.size() - 1;
//     }
// }

inline void DEMSolver::equipForceModel(std::unordered_map<std::string, std::string>& strMap) {
    // Analyze this model... what does it require?
    std::string model = m_force_model->m_force_model;
    // It should have those following names in it
    const std::set<std::string> contact_wildcard_names = m_force_model->m_contact_wildcards;
    const std::set<std::string> owner_wildcard_names = m_force_model->m_owner_wildcards;
    // If we spot that the force model requires an ingredient, we make sure that order goes to the ingredient
    // acquisition module
    std::string ingredient_definition = " ", wildcard_acquisition = " ", ingredient_acquisition_A = " ",
                ingredient_acquisition_B = " ", wildcard_write_back = " ", wildcard_destroy_record = " ";
    equip_force_model_ingr_acq(ingredient_definition, ingredient_acquisition_A, ingredient_acquisition_B, model);

    // Check if force, and wildcards are all defined in the model
    std::string non_match;
    if (!all_whole_word_match(model, contact_wildcard_names, non_match))
        DEME_WARNING(
            "Contact wildcard %s is not used/set in your custom force model. Your force model will probably not "
            "produce what you expect.",
            non_match.c_str());
    if (!all_whole_word_match(model, owner_wildcard_names, non_match))
        DEME_WARNING(
            "Owner wildcard %s is not used/set in your custom force model. Your force model will probably not produce "
            "what you expect.",
            non_match.c_str());
    if (!all_whole_word_match(model, {"force"}, non_match)) {
        DEME_WARNING(
            "Your custom force model does not set the variable %s at all. You probably will not see any contact in the "
            "simulation.",
            non_match.c_str());
    }

    // For contact wildcards, it needs to be brought from the global memory, and we expect the user's force model to use
    // and modify them, and in the end we will write them back to global mem.
    equip_contact_wildcards(wildcard_acquisition, wildcard_write_back, wildcard_destroy_record, contact_wildcard_names);

    if (m_ensure_kernel_line_num) {
        model = compact_code(model);
        ingredient_definition = compact_code(ingredient_definition);
        ingredient_acquisition_A = compact_code(ingredient_acquisition_A);
        ingredient_acquisition_B = compact_code(ingredient_acquisition_B);
    }
    strMap["_DEMForceModel_"] = model;
    strMap["_forceModelIngredientDefinition_"] = ingredient_definition;
    strMap["_forceModelIngredientAcqForA_"] = ingredient_acquisition_A;
    strMap["_forceModelIngredientAcqForB_"] = ingredient_acquisition_B;

    strMap["_forceModelContactWildcardAcq_"] = wildcard_acquisition;
    strMap["_forceModelContactWildcardWrite_"] = wildcard_write_back;
    strMap["_forceModelContactWildcardDestroy_"] = wildcard_destroy_record;

    DEME_DEBUG_PRINTF("Wildcard acquisition:\n%s", wildcard_acquisition.c_str());
    DEME_DEBUG_PRINTF("Wildcard write-back:\n%s", wildcard_write_back.c_str());
    DEME_DEBUG_PRINTF("Wildcard destroy inactive contacts:\n%s", wildcard_destroy_record.c_str());
}

inline void DEMSolver::equipFamilyOnFlyChanges(std::unordered_map<std::string, std::string>& strMap) {
    std::string condStr = " ";
    unsigned int n_rules = m_family_change_pairs.size();
    for (unsigned int i = 0; i < n_rules; i++) {
        // User family num and internal family num are not the same
        // Convert user-input pairs into impl-level pairs
        unsigned int implID1 = m_family_change_pairs.at(i).ID1;
        unsigned int implID2 = m_family_change_pairs.at(i).ID2;

        // The conditions will be handled by a series of if statements
        std::string cond = "if (family_code == " + std::to_string(implID1) + ") { bool shouldMakeChange = false;";
        std::string user_str = replace_pattern(m_family_change_conditions.at(i), "return", "shouldMakeChange = ");
        if (m_ensure_kernel_line_num) {
            user_str = compact_code(user_str);
        }
        cond += user_str;
        cond += "if (shouldMakeChange) {granData->familyID[myOwner] = " + std::to_string(implID2) + ";}";
        cond += "}";
        condStr += cond;
    }

    strMap["_nRulesOfChange_"] = std::to_string(n_rules);
    strMap["_familyChangeRules_"] = condStr;
}

inline void DEMSolver::equipFamilyPrescribedMotions(std::unordered_map<std::string, std::string>& strMap) {
    std::string velStr = " ", posStr = " ";
    for (const auto& preInfo : m_unique_family_prescription) {
        if (!preInfo.used) {
            continue;
        }
        velStr += "case " + std::to_string(preInfo.family) + ": {";
        posStr += "case " + std::to_string(preInfo.family) + ": {";
        if (!preInfo.externVel) {
            if (preInfo.linVelX != "none")
                velStr += "vX = " + preInfo.linVelX + ";";
            if (preInfo.linVelY != "none")
                velStr += "vY = " + preInfo.linVelY + ";";
            if (preInfo.linVelZ != "none")
                velStr += "vZ = " + preInfo.linVelZ + ";";
            if (preInfo.rotVelX != "none")
                velStr += "omgBarX = " + preInfo.rotVelX + ";";
            if (preInfo.rotVelY != "none")
                velStr += "omgBarY = " + preInfo.rotVelY + ";";
            if (preInfo.rotVelZ != "none")
                velStr += "omgBarZ = " + preInfo.rotVelZ + ";";
            velStr += "LinPrescribed = " + std::to_string(preInfo.linVelPrescribed) + ";";
            velStr += "RotPrescribed = " + std::to_string(preInfo.rotVelPrescribed) + ";";
        }  // TODO: add externVel==True case, loading from external vectors
        velStr += "break; }";
        if (!preInfo.externPos) {
            if (preInfo.linPosX != "none")
                posStr += "X = " + preInfo.linPosX + ";";
            if (preInfo.linPosY != "none")
                posStr += "Y = " + preInfo.linPosY + ";";
            if (preInfo.linPosZ != "none")
                posStr += "Z = " + preInfo.linPosZ + ";";
            if (preInfo.oriQ != "none") {
                posStr += "float4 myOriQ = " + preInfo.oriQ + ";";
                posStr += "oriQw = myOriQ.w; oriQx = myOriQ.x; oriQy = myOriQ.y; oriQz = myOriQ.z;";
            }
            posStr += "LinPrescribed = " + std::to_string(preInfo.linPosPrescribed) + ";";
            posStr += "RotPrescribed = " + std::to_string(preInfo.rotPosPrescribed) + ";";
        }  // TODO: add externPos==True case, loading from external vectors
        posStr += "break; }";
    }
    strMap["_velPrescriptionStrategy_"] = velStr;
    strMap["_posPrescriptionStrategy_"] = posStr;
}

// Family mask is no longer jitified... but stored in global array
// inline void DEMSolver::equipFamilyMasks(std::unordered_map<std::string, std::string>& strMap) {
//     std::string maskMat;
//     strMap["_nFamilyMaskEntries_"] = std::to_string(m_family_mask_matrix.size());
//     for (unsigned int i = 0; i < m_family_mask_matrix.size(); i++) {
//         maskMat += std::to_string(m_family_mask_matrix.at(i)) + ",";
//     }
//     // Put some junk in if it is empty
//     if (m_family_mask_matrix.size() == 0) {
//         maskMat += "0";
//     }
//     strMap["_familyMasks_"] = maskMat;
// }

inline void DEMSolver::equipAnalGeoTemplates(std::unordered_map<std::string, std::string>& strMap) {
    // Some sim systems can have 0 boundary entities in them. In this case, we have to ensure jitification does not fail
    std::string objOwner, objType, objMat, objNormal, objRelPosX, objRelPosY, objRelPosZ, objRotX, objRotY, objRotZ,
        objSize1, objSize2, objSize3, objMass;
    for (unsigned int i = 0; i < nAnalGM; i++) {
        // External objects will be owners, and their IDs are following template-loaded simulation clumps
        bodyID_t myOwner = nOwnerClumps + m_anal_owner.at(i);
        objOwner += std::to_string(myOwner) + ",";
        objType += std::to_string(m_anal_types.at(i)) + ",";
        objMat += std::to_string(m_anal_materials.at(i)) + ",";
        objNormal += std::to_string(m_anal_normals.at(i)) + ",";
        objRelPosX += to_string_with_precision(m_anal_comp_pos.at(i).x) + ",";
        objRelPosY += to_string_with_precision(m_anal_comp_pos.at(i).y) + ",";
        objRelPosZ += to_string_with_precision(m_anal_comp_pos.at(i).z) + ",";
        objRotX += to_string_with_precision(m_anal_comp_rot.at(i).x) + ",";
        objRotY += to_string_with_precision(m_anal_comp_rot.at(i).y) + ",";
        objRotZ += to_string_with_precision(m_anal_comp_rot.at(i).z) + ",";
        objSize1 += to_string_with_precision(m_anal_size_1.at(i)) + ",";
        objSize2 += to_string_with_precision(m_anal_size_2.at(i)) + ",";
        objSize3 += to_string_with_precision(m_anal_size_3.at(i)) + ",";
        // As for analytical object components, we just need to store mass, no MOI needed, since it's for for
        // calculation only. When collecting acceleration of analytical owners, it gets that from long global arrays.
        objMass += to_string_with_precision(m_ext_obj_mass.at(m_anal_owner.at(i))) + ",";
    }
    if (nAnalGM == 0) {
        // If the user looks for trouble, jitifies 0 analytical entities, then put some junk there to make it
        // compilable: those kernels won't be executed anyway
        objOwner += "0";
        objType += "0";
        objMat += "0";
        objNormal += "0";
        objRelPosX += "0";
        objRelPosY += "0";
        objRelPosZ += "0";
        objRotX += "0";
        objRotY += "0";
        objRotZ += "0";
        objSize1 += "0";
        objSize2 += "0";
        objSize3 += "0";
        objMass += "0";
    }

    std::unordered_map<std::string, std::string> array_content;
    array_content["_objOwner_"] = objOwner;
    array_content["_objType_"] = objType;
    array_content["_objMaterial_"] = objMat;
    array_content["_objNormal_"] = objNormal;
    array_content["_objRelPosX_"] = objRelPosX;
    array_content["_objRelPosY_"] = objRelPosY;
    array_content["_objRelPosZ_"] = objRelPosZ;
    array_content["_objRotX_"] = objRotX;
    array_content["_objRotY_"] = objRotY;
    array_content["_objRotZ_"] = objRotZ;
    array_content["_objSize1_"] = objSize1;
    array_content["_objSize2_"] = objSize2;
    array_content["_objSize3_"] = objSize3;
    array_content["_objMass_"] = objMass;

    std::string analyticalEntityDefs = ANALYTICAL_COMPONENT_DEFINITIONS_JITIFIED();
    analyticalEntityDefs = replace_patterns(analyticalEntityDefs, array_content);
    if (m_ensure_kernel_line_num) {
        analyticalEntityDefs = compact_code(analyticalEntityDefs);
    }
    strMap["_analyticalEntityDefs_"] = analyticalEntityDefs;

    // There is a special owner-only version used by force collection kernels. We have it here so we don't have not-used
    // variable warnings while jitifying
    strMap["_objOwner_"] = objOwner;
}

inline void DEMSolver::equipMassMoiVolume(std::unordered_map<std::string, std::string>& strMap) {
    std::string massDefs, moiDefs, massAcqStrat, moiAcqStrat;
    // We only need to jitify and mass info offsets to kernels if we jitify them. If not, we just bring mass info as
    // floats from global memory.
    if (jitify_mass_moi) {
        std::string MassProperties, moiX, moiY, moiZ;
        // Loop through all templates to jitify them
        for (unsigned int i = 0; i < m_template_clump_mass.size(); i++) {
            MassProperties += to_string_with_precision(m_template_clump_mass.at(i)) + ",";
            moiX += to_string_with_precision(m_template_clump_moi.at(i).x) + ",";
            moiY += to_string_with_precision(m_template_clump_moi.at(i).y) + ",";
            moiZ += to_string_with_precision(m_template_clump_moi.at(i).z) + ",";
        }
        for (unsigned int i = 0; i < m_ext_obj_mass.size(); i++) {
            MassProperties += to_string_with_precision(m_ext_obj_mass.at(i)) + ",";
            moiX += to_string_with_precision(m_ext_obj_moi.at(i).x) + ",";
            moiY += to_string_with_precision(m_ext_obj_moi.at(i).y) + ",";
            moiZ += to_string_with_precision(m_ext_obj_moi.at(i).z) + ",";
        }
        for (unsigned int i = 0; i < m_mesh_obj_mass.size(); i++) {
            MassProperties += to_string_with_precision(m_mesh_obj_mass.at(i)) + ",";
            moiX += to_string_with_precision(m_mesh_obj_moi.at(i).x) + ",";
            moiY += to_string_with_precision(m_mesh_obj_moi.at(i).y) + ",";
            moiZ += to_string_with_precision(m_mesh_obj_moi.at(i).z) + ",";
        }
        if (nDistinctMassProperties == 0) {
            MassProperties += "0";
            moiX += "0";
            moiY += "0";
            moiZ += "0";
        }
        std::unordered_map<std::string, std::string> array_content;
        array_content["_MassProperties_"] = MassProperties;
        array_content["_moiX_"] = moiX;
        array_content["_moiY_"] = moiY;
        array_content["_moiZ_"] = moiZ;

        massDefs = MASS_DEFINITIONS_JITIFIED();
        moiDefs = MOI_DEFINITIONS_JITIFIED();

        massDefs = replace_patterns(massDefs, array_content);
        moiDefs = replace_patterns(moiDefs, array_content);

        massAcqStrat = MASS_ACQUISITION_JITIFIED();
        moiAcqStrat = MOI_ACQUISITION_JITIFIED();
    } else {
        // Here, no need to jitify any mass/MOI templates
        massDefs = " ";
        moiDefs = " ";

        massAcqStrat = MASS_ACQUISITION_FLATTENED();
        moiAcqStrat = MOI_ACQUISITION_FLATTENED();
    }

    // Right now we always jitify clump volume info. This is because we don't use volume that often, probably only at
    // void ratio computation. So let's save some memory...
    std::string volumeDefs = "__constant__ __device__ float volumeProperties[] = {";
    for (unsigned int i = 0; i < m_template_clump_volume.size(); i++) {
        volumeDefs += to_string_with_precision(m_template_clump_volume.at(i)) + ",";
    }
    if (nDistinctMassProperties == 0) {
        volumeDefs += "0";
    }
    volumeDefs += "};\n";

    if (m_ensure_kernel_line_num) {
        massDefs = compact_code(massDefs);
        moiDefs = compact_code(moiDefs);
        massAcqStrat = compact_code(massAcqStrat);
        moiAcqStrat = compact_code(moiAcqStrat);
        volumeDefs = compact_code(volumeDefs);
    }
    strMap["_massDefs_"] = massDefs;
    strMap["_moiDefs_"] = moiDefs;
    strMap["_massAcqStrat_"] = massAcqStrat;
    strMap["_moiAcqStrat_"] = moiAcqStrat;
    strMap["_volumeDefs_"] = volumeDefs;

    DEME_DEBUG_PRINTF("Volume properties in kernel:");
    DEME_DEBUG_PRINTF("%s", volumeDefs.c_str());
}

inline void DEMSolver::equipMaterials(std::unordered_map<std::string, std::string>& strMap) {
    // Depending on the force model, there could be a few material properties that should be specified by the user
    const std::set<std::string> mat_prop_that_must_exist = m_force_model->m_must_have_mat_props;
    // Those must-haves will be added to the pool
    m_material_prop_names.insert(mat_prop_that_must_exist.begin(), mat_prop_that_must_exist.end());
    std::string materialDefs = " ";

    if (m_material_prop_names.size() == 0)
        return;

    // Construct material arrays line by line
    const std::string line_header = "__constant__ __device__ float ";
    for (const auto& prop_name : m_material_prop_names) {
        materialDefs += line_header + prop_name + "[] = {";
        // See what each material says...
        for (const auto& a_mat : m_loaded_materials) {
            const auto& name_val_pairs = a_mat->mat_prop;
            float val = 0.0;
            if (check_exist(name_val_pairs, prop_name)) {
                val = name_val_pairs.at(prop_name);
            } else {  // If no such key exists, val defaults to 0
                // If prop_name does not exist for this material, then if prop_name is one of the
                // mat_prop_that_must_exist, the user should know there is trouble...
                if (check_exist(mat_prop_that_must_exist, prop_name))
                    DEME_WARNING(
                        "A material does not have %s property specified. However, the force model you are using "
                        "requires that information, so we are using default 0.\nPlease be sure this is intentional.",
                        prop_name.c_str());
            }
            materialDefs += to_string_with_precision(val) + ",";
        }
        // If the user makes trouble and loaded 0 material, then we add some junk in it as placeholder
        if (m_loaded_materials.size() == 0) {
            materialDefs += "0";
        }
        // End the line
        materialDefs += "};\n";
    }
    DEME_DEBUG_PRINTF("Material properties in kernel:");
    DEME_DEBUG_PRINTF("%s", materialDefs.c_str());
    // Try imagining something like this...
    //      steel   plastic
    // E    1e9     1e8
    // nu   0.33    0.3
    // CoR  0.6     0.4

    if (m_ensure_kernel_line_num) {
        materialDefs = compact_code(materialDefs);
    }
    strMap["_materialDefs_"] = materialDefs;
}

inline void DEMSolver::equipClumpTemplates(std::unordered_map<std::string, std::string>& strMap) {
    // We only need to jitify and bring template offset to kernels if we jitify clump templates. If not, we just bring
    // clump component info as floats from global memory.
    std::string clump_template_arrays, componentAcqStrat;
    if (jitify_clump_templates) {
        // Prepare jitified clump template
        clump_template_arrays = CLUMP_COMPONENT_DEFINITIONS_JITIFIED();
        {
            std::unordered_map<std::string, std::string> array_content;
            std::string CDRadii, Radii, CDRelPosX, CDRelPosY, CDRelPosZ;
            // Loop through all clump templates to jitify them, but without going over the shared memory limit
            for (unsigned int i = 0; i < nJitifiableClumpTopo; i++) {
                for (unsigned int j = 0; j < m_template_sp_radii.at(i).size(); j++) {
                    Radii += to_string_with_precision(m_template_sp_radii.at(i).at(j)) + ",";
                    CDRadii += to_string_with_precision(m_template_sp_radii.at(i).at(j) + m_expand_factor) + ",";
                    CDRelPosX += to_string_with_precision(m_template_sp_relPos.at(i).at(j).x) + ",";
                    CDRelPosY += to_string_with_precision(m_template_sp_relPos.at(i).at(j).y) + ",";
                    CDRelPosZ += to_string_with_precision(m_template_sp_relPos.at(i).at(j).z) + ",";
                }
            }
            if (nJitifiableClumpTopo == 0) {
                // If the user looks for trouble, jitifies 0 template, then put some junk there to make it compilable:
                // those kernels won't be executed anyway
                Radii += "0";
                CDRadii += "0";
                CDRelPosX += "0";
                CDRelPosY += "0";
                CDRelPosZ += "0";
            }
            array_content["_Radii_"] = Radii;
            array_content["_CDRadii_"] = CDRadii;
            array_content["_CDRelPosX_"] = CDRelPosX;
            array_content["_CDRelPosY_"] = CDRelPosY;
            array_content["_CDRelPosZ_"] = CDRelPosZ;
            clump_template_arrays = replace_patterns(clump_template_arrays, array_content);
        }

        // Then prepare the acquisition rules for jitified templates. It's so much trouble.
        // This part is different depending on whether we have clump templates that are in global memory only
        if (nJitifiableClumpTopo == nDistinctClumpBodyTopologies) {
            // In this case, all clump templates can be jitified
            componentAcqStrat = CLUMP_COMPONENT_ACQUISITION_ALL_JITIFIED();
        } else if (nJitifiableClumpTopo < nDistinctClumpBodyTopologies) {
            // In this case, some clump templates are in the global memory
            componentAcqStrat = CLUMP_COMPONENT_ACQUISITION_PARTIALLY_JITIFIED();
        }

    } else {
        // Compared to the jitified case, non-jitified version is much simpler: just bring them from global memory
        componentAcqStrat = CLUMP_COMPONENT_ACQUISITION_FLATTENED();
        // And we do not need to define clump template in kernels, in this case
        clump_template_arrays = " ";
    }

    if (m_ensure_kernel_line_num) {
        clump_template_arrays = compact_code(clump_template_arrays);
        componentAcqStrat = compact_code(componentAcqStrat);
    }
    strMap["_clumpTemplateDefs_"] = clump_template_arrays;
    strMap["_componentAcqStrat_"] = componentAcqStrat;
}

inline void DEMSolver::equipIntegrationScheme(std::unordered_map<std::string, std::string>& strMap) {
    std::string strat;
    switch (m_integrator) {
        case (TIME_INTEGRATOR::FORWARD_EULER):
            strat = VEL_TO_PASS_ON_FORWARD_EULER();
            break;
        case (TIME_INTEGRATOR::CENTERED_DIFFERENCE):
            strat = VEL_TO_PASS_ON_CENTERED_DIFF();
            break;
        case (TIME_INTEGRATOR::EXTENDED_TAYLOR):
            strat = VEL_TO_PASS_ON_EXTENDED_TAYLOR();
            break;
        default:
            DEME_ERROR("The integration type is unknown or not implemented. Please select another via SetIntegrator.");
    }
    strMap["_integrationVelocityPassOnStrategy_"] = strat;
}

inline void DEMSolver::equipSimParams(std::unordered_map<std::string, std::string>& strMap) {
    strMap["_nvXp2_"] = std::to_string(nvXp2);
    strMap["_nvYp2_"] = std::to_string(nvYp2);
    strMap["_nvZp2_"] = std::to_string(nvZp2);

    // strMap["_nbX_"] = std::to_string(nbX);
    // strMap["_nbY_"] = std::to_string(nbY);
    // strMap["_nbZ_"] = std::to_string(nbZ);

    strMap["_l_"] = to_string_with_precision(l);
    strMap["_voxelSize_"] = to_string_with_precision(m_voxelSize);
    // strMap["_binSize_"] = to_string_with_precision(m_binSize);

    // strMap["_nOwnerBodies_"] = std::to_string(nOwnerBodies);
    // strMap["_nSpheresGM_"] = std::to_string(nSpheresGM);

    strMap["_LBFX_"] = to_string_with_precision(m_boxLBF.x);
    strMap["_LBFY_"] = to_string_with_precision(m_boxLBF.y);
    strMap["_LBFZ_"] = to_string_with_precision(m_boxLBF.z);
    // strMap["_Gx_"] = to_string_with_precision(G.x);
    // strMap["_Gy_"] = to_string_with_precision(G.y);
    // strMap["_Gz_"] = to_string_with_precision(G.z);
    // strMap["_beta_"] = to_string_with_precision(m_expand_factor);

    // Some constants that we should consider using or not using
    // strMap["_nAnalGM_"] = std::to_string(nAnalGM);
    strMap["_nActiveLoadingThreads_"] = std::to_string(NUM_ACTIVE_TEMPLATE_LOADING_THREADS);
    // nTotalBodyTopologies includes clump topologies and ext obj topologies
    strMap["_nDistinctMassProperties_"] = std::to_string(nDistinctMassProperties);
    strMap["_nJitifiableClumpComponents_"] = std::to_string(nJitifiableClumpComponents);
    strMap["_nMatTuples_"] = std::to_string(nMatTuples);
}

}  // namespace deme
