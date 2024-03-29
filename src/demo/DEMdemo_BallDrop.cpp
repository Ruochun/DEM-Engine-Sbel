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

using namespace deme;
using namespace std::filesystem;

int main() {
    DEMSolver DEMSim;
    DEMSim.SetVerbosity(DEBUG);
    DEMSim.SetOutputFormat(OUTPUT_FORMAT::CSV);
    DEMSim.SetOutputContent(OUTPUT_CONTENT::ABSV);

    // E, nu, CoR, mu, Crr...
    auto mat_type_ball = DEMSim.LoadMaterial({{"E", 1e10}, {"nu", 0.3}, {"CoR", 0.3}, {"mu", 0.0}, {"Crr", 0.0}});
    auto mat_type_terrain = DEMSim.LoadMaterial({{"E", 5e9}, {"nu", 0.3}, {"CoR", 0.2}, {"mu", 0.0}, {"Crr", 0.0}});

    auto projectile = DEMSim.AddWavefrontMeshObject((GET_DATA_PATH() / "mesh/sphere.obj").string(), mat_type_ball);
    std::cout << "Total num of triangles: " << projectile->GetNumTriangles() << std::endl;

    // float ball_rad = 0.2;
    // auto template_ball =
    //     DEMSim.LoadSphereType(ball_rad * ball_rad * ball_rad * 7e3 * 4 / 3 * 3.14, ball_rad, mat_type_ball);

    float terrain_rad = 0.005;
    auto template_terrain = DEMSim.LoadSphereType(terrain_rad * terrain_rad * terrain_rad * 2.6e3 * 4 / 3 * 3.14,
                                                  terrain_rad, mat_type_terrain);

    float step_size = 1e-5;
    double world_size = 1.5;
    // DEMSim.InstructBoxDomainNumVoxel(21, 21, 22, world_size / std::pow(2, 16) / std::pow(2, 21));
    DEMSim.InstructBoxDomainDimension(world_size, world_size, 2 * world_size, SPATIAL_DIR::X);
    DEMSim.InstructBoxDomainBoundingBC("top_open", mat_type_terrain);
    DEMSim.AddBCPlane(make_float3(0, 0, -world_size / 2), make_float3(0, 0, 1), mat_type_terrain);
    DEMSim.SetCoordSysOrigin("center");
    DEMSim.SetInitTimeStep(step_size);
    DEMSim.SetGravitationalAcceleration(make_float3(0, 0, -9.8));
    // If you want to use a large UpdateFreq then you have to expand spheres to ensure safety
    DEMSim.SetCDUpdateFreq(10);
    // DEMSim.SetExpandFactor(1e-3);
    DEMSim.SetMaxVelocity(2.);
    DEMSim.SetExpandSafetyParam(1.1);
    DEMSim.SetInitBinSize(4 * terrain_rad);
    DEMSim.Initialize();

    path out_dir = current_path();
    out_dir += "/DEMdemo_BallDrop";
    create_directory(out_dir);

    float sim_end = 20.0;
    unsigned int fps = 20;
    float frame_time = 1.0 / fps;
    unsigned int out_steps = (unsigned int)(1.0 / (fps * step_size));

    std::cout << "Output at " << fps << " FPS" << std::endl;
    unsigned int currframe = 0;
    unsigned int curr_step = 0;

    ///////////////////////////////////////////////////////////////
    // We first show a method to gradually increase the number of
    // particles in simulation, until it reaches a certain number
    ///////////////////////////////////////////////////////////////
    float offset_z = 4 * terrain_rad - world_size / 2;
    while (DEMSim.GetNumClumps() < 100000) {
        DEMSim.ClearCache();
        float3 sample_center = make_float3(0, 0, offset_z);
        float sample_halfheight = 0.0001;
        float sample_halfwidth = world_size / 2 * 0.95;
        auto input_xyz = DEMBoxGridSampler(
            sample_center, make_float3(sample_halfwidth, sample_halfwidth, sample_halfheight), 2.01 * terrain_rad);
        DEMSim.AddClumps(template_terrain, input_xyz);
        DEMSim.UpdateClumps();

        // Allow for some settling
        // Must DoDynamicsThenSync (not DoDynamics), as adding entities to the simulation is only allowed at a sync-ed
        // point of time.
        DEMSim.DoDynamicsThenSync(0.1);

        DEMSim.ShowThreadCollaborationStats();
        char filename[200];
        sprintf(filename, "%s/DEMdemo_output_%04d.csv", out_dir.c_str(), currframe++);
        DEMSim.WriteSphereFile(std::string(filename));

        // Then prepare for adding another layer
        offset_z += 2.5 * terrain_rad;

        std::cout << "Current number of clumps: " << DEMSim.GetNumClumps() << std::endl;
    }
    DEMSim.DoDynamicsThenSync(0.2);

    // for (double t = 0; t < (double)sim_end; t += frame_time, currframe++) {
    //     std::cout << "Frame: " << currframe << std::endl;
    //     DEMSim.ShowThreadCollaborationStats();
    //     char filename[200];
    //     sprintf(filename, "%s/DEMdemo_output_%04d.csv", out_dir.c_str(), currframe);
    //     DEMSim.WriteSphereFile(std::string(filename));

    //     DEMSim.DoDynamics(frame_time);

    //     //
    // }

    std::cout << "DEMdemo_BallDrop exiting..." << std::endl;
    return 0;
}
