//  Copyright (c) 2021, SBEL GPU Development Team
//  Copyright (c) 2021, University of Wisconsin - Madison
//
//	SPDX-License-Identifier: BSD-3-Clause

#include <core/ApiVersion.h>
#include <core/utils/ThreadManager.h>
#include <DEM/API.h>
#include <DEM/HostSideHelpers.hpp>
#include <DEM/utils/Samplers.hpp>

#include <filesystem>
#include <cstdio>
#include <time.h>
#include <filesystem>

using namespace deme;
using namespace std::filesystem;

int main() {
    DEMSolver DEMSim;
    DEMSim.SetVerbosity(DEBUG);
    DEMSim.SetOutputFormat(OUTPUT_FORMAT::CSV);
    DEMSim.SetContactOutputContent(OWNERS | FORCE | POINT | COMPONENT | NORMAL | TORQUE_ONLY_FORCE);

    // srand(time(NULL));
    srand(4150);

    auto mat_type_1 = DEMSim.LoadMaterial({{"E", 1e9}, {"nu", 0.3}, {"CoR", 0.8}});

    auto sph_type_1 = DEMSim.LoadSphereType(11728., 1., mat_type_1);

    std::vector<float3> input_xyz1, input_xyz2;
    std::vector<float3> input_vel1, input_vel2;
    std::vector<std::shared_ptr<DEMClumpTemplate>> input_clump_type(1, sph_type_1);
    // std::vector<unsigned int> input_clump_type(1, sph_type_1);

    // Inputs are just 2 sphere
    float sphPos = 1.2f;
    input_xyz1.push_back(make_float3(-sphPos, 0, 0));
    input_xyz2.push_back(make_float3(sphPos, 0, 0));
    input_vel1.push_back(make_float3(1.f, 0, 0));
    input_vel2.push_back(make_float3(-1.f, 0, 0));

    auto particles1 = DEMSim.AddClumps(input_clump_type, input_xyz1);
    particles1->SetVel(input_vel1);
    particles1->SetFamily(0);
    auto tracker1 = DEMSim.Track(particles1);

    DEMSim.DisableContactBetweenFamilies(0, 1);

    DEMSim.AddBCPlane(make_float3(0, 0, -1.25), make_float3(0, 0, 1), mat_type_1);

    // Create a inspector to find out stuff
    auto max_z_finder = DEMSim.CreateInspector("clump_max_z");
    float max_z;
    auto max_v_finder = DEMSim.CreateInspector("clump_max_absv");
    float max_v;

    DEMSim.InstructBoxDomainNumVoxel(22, 21, 21, 3e-11);

    DEMSim.SetCoordSysOrigin("center");
    DEMSim.SetInitTimeStep(2e-5);
    DEMSim.SetGravitationalAcceleration(make_float3(0, 0, -9.8));
    // Velocity not to exceed 3.0
    DEMSim.SetCDUpdateFreq(10);
    DEMSim.SetMaxVelocity(3.);
    DEMSim.SetExpandSafetyParam(2.);
    DEMSim.SetIntegrator(TIME_INTEGRATOR::CENTERED_DIFFERENCE);

    DEMSim.Initialize();

    // You can add more clumps to simulation after initialization, like this...
    DEMSim.ClearCache();
    auto particles2 = DEMSim.AddClumps(input_clump_type, input_xyz2);
    particles2->SetVel(input_vel2);
    particles2->SetFamily(1);
    auto tracker2 = DEMSim.Track(particles2);
    DEMSim.UpdateClumps();

    // Ready simulation
    path out_dir = current_path();
    out_dir += "/DEMdemo_SingleSphereCollide";
    create_directory(out_dir);
    bool changed_family = false;
    for (int i = 0; i < 100; i++) {
        std::cout << "Frame: " << i << std::endl;

        if ((!changed_family) && i >= 10) {
            // DEMSim.ChangeFamily(1, 0);
            DEMSim.EnableContactBetweenFamilies(0, 1);
            changed_family = true;
        }

        char filename[100];
        sprintf(filename, "%s/DEMdemo_output_%04d.csv", out_dir.c_str(), i);
        DEMSim.WriteSphereFile(std::string(filename));

        char cnt_filename[100];
        sprintf(cnt_filename, "%s/Contact_pairs_%04d.csv", out_dir.c_str(), i);
        DEMSim.WriteContactFile(std::string(cnt_filename));

        DEMSim.DoDynamicsThenSync(1e-2);
        max_z = max_z_finder->GetValue();
        max_v = max_v_finder->GetValue();
        std::cout << "Max Z coord is " << max_z << std::endl;
        std::cout << "Max velocity of any point is " << max_v << std::endl;
        std::cout << "Particle 1 X coord is " << tracker1->Pos().x << std::endl;
        std::cout << "Particle 2 X coord is " << tracker2->Pos().x << std::endl;
    }

    std::cout << "DEMdemo_SingleSphereCollide exiting..." << std::endl;
    return 0;
}
