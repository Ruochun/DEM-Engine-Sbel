//  Copyright (c) 2021, SBEL GPU Development Team
//  Copyright (c) 2021, University of Wisconsin - Madison
//
//	SPDX-License-Identifier: BSD-3-Clause

#include <core/ApiVersion.h>
#include <core/utils/ThreadManager.h>
#include <DEM/API.h>
#include <DEM/HostSideHelpers.hpp>
#include <DEM/utils/Samplers.hpp>

#include <cstdio>
#include <chrono>
#include <filesystem>
#include <map>
#include <random>
#include <cmath>

using namespace deme;
using namespace std::filesystem;

int main() {
    DEMSolver DEMSim;
    DEMSim.SetVerbosity(INFO);
    DEMSim.SetOutputFormat(OUTPUT_FORMAT::CSV);
    // DEMSim.SetOutputContent(OUTPUT_CONTENT::FAMILY);
    DEMSim.SetOutputContent(OUTPUT_CONTENT::XYZ);

    srand(759);

    //
    float kg_g_conv = 1;
    // Define materials
    auto mat_type_terrain = DEMSim.LoadMaterial({{"E", 2e9 * kg_g_conv}, {"nu", 0.3}, {"CoR", 0.3}, {"mu", 0.5}});
    auto mat_type_wheel = DEMSim.LoadMaterial({{"E", 1e9 * kg_g_conv}, {"nu", 0.3}, {"CoR", 0.3}, {"mu", 0.5}});

    // Define the simulation world
    double world_y_size = 2.2;
    double world_x_size = 4.4;
    DEMSim.InstructBoxDomainDimension(world_x_size, world_y_size, world_y_size);
    float bottom = -0.5;
    DEMSim.AddBCPlane(make_float3(0, 0, bottom), make_float3(0, 0, 1), mat_type_terrain);
    // Side bounding planes
    DEMSim.AddBCPlane(make_float3(0, world_y_size / 2, 0), make_float3(0, -1, 0), mat_type_terrain);
    DEMSim.AddBCPlane(make_float3(0, -world_y_size / 2, 0), make_float3(0, 1, 0), mat_type_terrain);

    // Define the wheel geometry
    float wheel_rad = 0.25;
    float wheel_width = 0.2;
    float wheel_mass = 10.0 * kg_g_conv;  // in kg or g
    // Our shelf wheel geometry is lying flat on ground with z being the axial direction
    float wheel_IZZ = wheel_mass * wheel_rad * wheel_rad / 2;
    float wheel_IXX = (wheel_mass / 12) * (3 * wheel_rad * wheel_rad + wheel_width * wheel_width);
    auto wheel_template =
        DEMSim.LoadClumpType(wheel_mass, make_float3(wheel_IXX, wheel_IXX, wheel_IZZ),
                             (GET_DATA_PATH() / "clumps/ViperWheelSimple.csv").string(), mat_type_wheel);
    // The file contains no wheel particles size info, so let's manually set them
    wheel_template->radii = std::vector<float>(wheel_template->nComp, 0.01);

    // Then the ground particle template
    DEMClumpTemplate shape_template;
    shape_template.ReadComponentFromFile((GET_DATA_PATH() / "clumps/triangular_flat.csv").string());
    // Calculate its mass and MOI
    float mass = 2.6e3 * 5.5886717 * kg_g_conv;  // in kg or g
    float3 MOI = make_float3(1.8327927, 2.1580013, 0.77010059) * 2.6e3 * kg_g_conv;
    // Scale the template we just created
    std::vector<std::shared_ptr<DEMClumpTemplate>> ground_particle_templates;
    std::vector<double> scales = {0.0014, 0.00063, 0.00033, 0.00022, 0.00015, 0.00009};
    std::for_each(scales.begin(), scales.end(), [](double& r) { r *= 10.; });
    for (double scaling : scales) {
        auto this_template = shape_template;
        this_template.mass = (double)mass * scaling * scaling * scaling;
        this_template.MOI.x = (double)MOI.x * (double)(scaling * scaling * scaling * scaling * scaling);
        this_template.MOI.y = (double)MOI.y * (double)(scaling * scaling * scaling * scaling * scaling);
        this_template.MOI.z = (double)MOI.z * (double)(scaling * scaling * scaling * scaling * scaling);
        std::cout << "Mass: " << this_template.mass << std::endl;
        std::cout << "MOIX: " << this_template.MOI.x << std::endl;
        std::cout << "MOIY: " << this_template.MOI.y << std::endl;
        std::cout << "MOIZ: " << this_template.MOI.z << std::endl;
        std::cout << "=====================" << std::endl;
        std::for_each(this_template.radii.begin(), this_template.radii.end(), [scaling](float& r) { r *= scaling; });
        std::for_each(this_template.relPos.begin(), this_template.relPos.end(), [scaling](float3& r) { r *= scaling; });
        this_template.materials = std::vector<std::shared_ptr<DEMMaterial>>(this_template.nComp, mat_type_terrain);
        ground_particle_templates.push_back(DEMSim.LoadClumpType(this_template));
    }

    // // Instantiate this wheel
    // auto wheel = DEMSim.AddClumps(wheel_template, make_float3(-0.5, 0, bottom + 0.38));
    // // Let's `flip' the wheel's initial position so... yeah, it's like how wheel operates normally
    // wheel->SetOriQ(make_float4(0.7071, 0, 0, 0.7071));
    // // Give the wheel a family number so we can potentially add prescription
    // wheel->SetFamily(101);
    // // Note that the added constant ang vel is wrt the wheel's own coord sys, therefore it should be on the z axis:
    // // in line with the orientation at which the wheel is loaded into the simulation system.
    // DEMSim.SetFamilyPrescribedAngVel(100, "0", "0", "-0.5", false);
    // DEMSim.SetFamilyFixed(101);
    // DEMSim.DisableContactBetweenFamilies(101, 0);
    // DEMSim.DisableContactBetweenFamilies(101, 1);
    // DEMSim.DisableContactBetweenFamilies(101, 2);
    // DEMSim.DisableContactBetweenFamilies(101, 3);
    // DEMSim.DisableContactBetweenFamilies(101, 4);
    // DEMSim.DisableContactBetweenFamilies(101, 5);
    // DEMSim.DisableContactBetweenFamilies(101, 101);
    // DEMSim.DisableContactBetweenFamilies(100, 100);

    // Now we load part1 clump locations from a part1 output file
    auto part1_clump_xyz = DEMSim.ReadClumpXyzFromCsv("GRC_2e6.csv");
    auto part1_clump_quaternion = DEMSim.ReadClumpQuatFromCsv("GRC_2e6.csv");
    std::vector<float3> in_xyz;
    std::vector<float4> in_quat;
    std::vector<std::shared_ptr<DEMClumpTemplate>> in_types;
    unsigned int t_num;
    for (int i = 0; i < scales.size(); i++) {
        // Our template names are 0001, 0002 etc.
        t_num++;
        char t_name[20];
        sprintf(t_name, "%04d", t_num);

        auto this_type_xyz = part1_clump_xyz[std::string(t_name)];
        auto this_type_quat = part1_clump_quaternion[std::string(t_name)];

        size_t n_clump_this_type = this_type_xyz.size();
        // Prepare clump type identification vector for loading into the system (don't forget type 0 in
        // ground_particle_templates is the template for rover wheel)
        std::vector<std::shared_ptr<DEMClumpTemplate>> this_type(n_clump_this_type,
                                                                 ground_particle_templates.at(t_num - 1));

        // Add them to the big long vector
        in_xyz.insert(in_xyz.end(), this_type_xyz.begin(), this_type_xyz.end());
        in_quat.insert(in_quat.end(), this_type_quat.begin(), this_type_quat.end());
        in_types.insert(in_types.end(), this_type.begin(), this_type.end());
    }
    // Remove some elements maybe? I feel this making the surface flatter
    std::vector<notStupidBool_t> elem_to_remove(in_xyz.size(), 0);
    for (size_t i = 0; i < in_xyz.size(); i++) {
        if (in_xyz.at(i).z > -0.44)
            elem_to_remove.at(i) = 1;
    }
    in_xyz.erase(
        std::remove_if(in_xyz.begin(), in_xyz.end(),
                       [&elem_to_remove, &in_xyz](const float3& i) { return elem_to_remove.at(&i - in_xyz.data()); }),
        in_xyz.end());
    in_quat.erase(
        std::remove_if(in_quat.begin(), in_quat.end(),
                       [&elem_to_remove, &in_quat](const float4& i) { return elem_to_remove.at(&i - in_quat.data()); }),
        in_quat.end());
    in_types.erase(
        std::remove_if(in_types.begin(), in_types.end(),
                       [&elem_to_remove, &in_types](const auto& i) { return elem_to_remove.at(&i - in_types.data()); }),
        in_types.end());

    // Finally, load the info into this batch
    DEMClumpBatch base_batch(in_xyz.size());
    base_batch.SetTypes(in_types);
    base_batch.SetPos(in_xyz);
    base_batch.SetOriQ(in_quat);

    // Based on the `base_batch', we can create more batches
    std::vector<float> x_shift_dist = {-1.54, -0.52, 0.52};
    std::vector<float> y_shift_dist = {-0.52, 0.52};
    // X-dir bounding planes (we currently have 6 patches, and X-dir spans from -2 to 1)
    DEMSim.AddBCPlane(make_float3(-2.2, 0, 0), make_float3(1, 0, 0), mat_type_terrain);
    DEMSim.AddBCPlane(make_float3(1.1, 0, 0), make_float3(-1, 0, 0), mat_type_terrain);
    // Add some patches of such graular bed
    for (float x_shift : x_shift_dist) {
        for (float y_shift : y_shift_dist) {
            DEMClumpBatch another_batch = base_batch;
            std::vector<float3> my_xyz = in_xyz;
            std::for_each(my_xyz.begin(), my_xyz.end(), [x_shift, y_shift](float3& xyz) {
                xyz.x += x_shift;
                xyz.y += y_shift;
            });
            another_batch.SetPos(my_xyz);
            DEMSim.AddClumps(another_batch);
        }
    }

    // Keep tab of the max velocity in simulation
    auto max_v_finder = DEMSim.CreateInspector("clump_max_absv");
    // Final void ratio inspection tool
    auto void_ratio_finder =
        DEMSim.CreateInspector("clump_volume", "return (abs(X) <= 0.48) && (abs(Y) <= 0.48) && (Z <= -0.45);");
    float total_volume = 0.96 * 0.96 * 0.05;
    // // Mimic the compressor from the previous run
    // auto compressor = DEMSim.AddExternalObject();
    // compressor->AddPlane(make_float3(0, 0, 0), make_float3(0, 0, -1), mat_type_terrain);
    // compressor->SetFamily(RESERVED_FAMILY_NUM);
    // auto compressor_tracker = DEMSim.Track(compressor);

    // Make ready for simulation
    float step_size = 5e-7;
    DEMSim.SetCoordSysOrigin("center");
    DEMSim.SetInitTimeStep(step_size);
    DEMSim.SetGravitationalAcceleration(make_float3(0, 0, -9.8));
    // If you want to use a large UpdateFreq then you have to expand spheres to ensure safety
    DEMSim.SetCDUpdateFreq(10);
    // DEMSim.SetExpandFactor(1e-3);
    DEMSim.SetMaxVelocity(3.);
    DEMSim.SetExpandSafetyParam(1.1);
    DEMSim.SetInitBinSize(scales.at(3));
    DEMSim.Initialize();

    unsigned int fps = 200;
    unsigned int out_steps = (unsigned int)(1.0 / (fps * step_size));

    path out_dir = current_path();
    out_dir += "/DEMdemo_GRCPrep_Part3";
    create_directory(out_dir);
    unsigned int currframe = 0;
    unsigned int curr_step = 0;

    float settle_batch_time = 0.5;
    // bool change_step_size = false;
    // float compressor_v = 0.05 / settle_batch_time;
    // float now_z = -0.43;
    // compressor_tracker->SetPos(make_float3(0, 0, now_z));
    for (float t = 0; t < settle_batch_time; t += step_size, curr_step++) {
        if (curr_step % out_steps == 0) {
            std::cout << "Frame: " << currframe << std::endl;
            float max_v = max_v_finder->GetValue();
            std::cout << "Max vel in simulation is " << max_v << std::endl;
            DEMSim.ShowThreadCollaborationStats();
            char filename[200];
            sprintf(filename, "%s/DEMdemo_output_%04d.csv", out_dir.c_str(), currframe++);
            DEMSim.WriteSphereFile(std::string(filename));
        }
        // now_z += compressor_v * step_size;
        // compressor_tracker->SetPos(make_float3(0, 0, now_z));
        DEMSim.DoDynamics(step_size);
        // if (t>0.1 && !change_step_size) {
        //     DEMSim.DoDynamicsThenSync(0);
        //     DEMSim.SetInitTimeStep(2*step_size);
        //     DEMSim.UpdateSimParams();
        //     change_step_size = true;
        // }
    }

    float matter_volume = void_ratio_finder->GetValue();
    std::cout << "Void ratio after settling " << (total_volume - matter_volume) / matter_volume << std::endl;

    DEMSim.DoDynamicsThenSync(0);
    char cp_filename[200];
    sprintf(cp_filename, "%s/GRC_10e6.csv", out_dir.c_str());
    DEMSim.WriteClumpFile(std::string(cp_filename));

    // DEMSim.ChangeFamily(101, 100);
    // for (double t = 0; t < (double)time_end; t += step_size, curr_step++) {
    //     if (curr_step % out_steps == 0) {
    //         std::cout << "Frame: " << currframe << std::endl;
    //         DEMSim.ShowThreadCollaborationStats();
    //         char filename[100];
    //         sprintf(filename, "%s/DEMdemo_output_%04d.csv", out_dir.c_str(), currframe);
    //         DEMSim.WriteSphereFile(std::string(filename));
    //         currframe++;
    //     }

    //     DEMSim.DoDynamics(step_size);
    // }

    DEMSim.ClearThreadCollaborationStats();

    std::cout << "DEMdemo_GRCPrep_Part3 exiting..." << std::endl;
    return 0;
}
