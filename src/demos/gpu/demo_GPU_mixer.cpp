// =============================================================================
// PROJECT CHRONO - http://projectchrono.org
//
// Copyright (c) 2019 projectchrono.org
// All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be found
// in the LICENSE file at the top level of the distribution and at
// http://projectchrono.org/license-chrono.txt.
//
// =============================================================================
// Authors: Nic Olsen
// =============================================================================
// Chrono::Gpu evaluation of several simple mixer designs. Material
// consisting of spherical particles is let to aggitate in a rotating mixer.
// Metrics on the performance of each mixer can be determined in post-
// processing.
// =============================================================================

#include <cmath>
#include <iostream>
#include <string>

#include "chrono/core/ChGlobal.h"
#include "chrono/utils/ChUtilsSamplers.h"
#include "chrono_gpu/ChGpuData.h"
#include "chrono_gpu/api/ChApiGpuChrono.h"
#include "chrono_gpu/physics/ChGpu.h"
#include "chrono_gpu/physics/ChGpuTriMesh.h"
#include "chrono_gpu/utils/ChGpuJsonParser.h"
#include "chrono_thirdparty/filesystem/path.h"

using namespace chrono;
using namespace chrono::gpu;

void ShowUsage(std::string name) {
    std::cout << "usage: " + name + " <json_file>" << std::endl;
}

int main(int argc, char* argv[]) {
    sim_param_holder params;

    if (argc != 2 || ParseJSON(argv[1], params) == false) {
        ShowUsage(argv[0]);
        return 1;
    }

    const float Bx = params.box_X;
    const float By = Bx;
    const float chamber_height = Bx / 3;  // TODO
    const float fill_height = chamber_height;
    const float Bz = chamber_height + fill_height;
    std::cout << "Box Dims: " << Bx << " " << By << " " << Bz << std::endl;

    float iteration_step = params.step_size;

    ChGpuSMCtrimesh_API apiSMC_TriMesh(params.sphere_radius, params.sphere_density, make_float3(Bx, By, Bz));

    ChSystemGpuSMC_trimesh& gpu_sys = apiSMC_TriMesh.getSystem();

    gpu_sys.set_K_n_SPH2SPH(params.normalStiffS2S);
    gpu_sys.set_K_n_SPH2WALL(params.normalStiffS2W);
    gpu_sys.set_K_n_SPH2MESH(params.normalStiffS2M);

    gpu_sys.set_K_t_SPH2SPH(params.tangentStiffS2S);
    gpu_sys.set_K_t_SPH2WALL(params.tangentStiffS2W);
    gpu_sys.set_K_t_SPH2MESH(params.tangentStiffS2M);

    gpu_sys.set_Gamma_n_SPH2SPH(params.normalDampS2S);
    gpu_sys.set_Gamma_n_SPH2WALL(params.normalDampS2W);
    gpu_sys.set_Gamma_n_SPH2MESH(params.normalDampS2M);

    gpu_sys.set_Gamma_t_SPH2SPH(params.tangentDampS2S);
    gpu_sys.set_Gamma_t_SPH2WALL(params.tangentDampS2W);
    gpu_sys.set_Gamma_t_SPH2MESH(params.tangentDampS2M);

    gpu_sys.set_Cohesion_ratio(params.cohesion_ratio);
    gpu_sys.set_Adhesion_ratio_S2M(params.adhesion_ratio_s2m);
    gpu_sys.set_Adhesion_ratio_S2W(params.adhesion_ratio_s2w);
    gpu_sys.set_friction_mode(chrono::gpu::CHGPU_FRICTION_MODE::MULTI_STEP);

    gpu_sys.set_static_friction_coeff_SPH2SPH(params.static_friction_coeffS2S);
    gpu_sys.set_static_friction_coeff_SPH2WALL(params.static_friction_coeffS2W);
    gpu_sys.set_static_friction_coeff_SPH2MESH(params.static_friction_coeffS2M);

    gpu_sys.setOutputMode(params.write_mode);

    filesystem::create_directory(filesystem::path(params.output_dir));

    gpu_sys.set_timeIntegrator(CHGPU_TIME_INTEGRATOR::CENTERED_DIFFERENCE);
    gpu_sys.set_fixed_stepSize(params.step_size);
    gpu_sys.set_BD_Fixed(true);

    const float chamber_bottom = -Bz / 2.f;
    const float fill_bottom = chamber_bottom + chamber_height;

    float cyl_center[3] = {0, 0, 0};
    const float cyl_rad = Bx / 2.f;
    gpu_sys.Create_BC_Cyl_Z(cyl_center, cyl_rad, false, false);

    utils::HCPSampler<float> sampler(2.1 * params.sphere_radius);
    std::vector<ChVector<float>> body_points;

    const float fill_radius = Bx / 2.f - 2.f * params.sphere_radius;
    const float fill_top = fill_bottom + fill_height;

    unsigned int n_spheres = body_points.size();
    std::cout << "Created " << n_spheres << " spheres" << std::endl;
    std::cout << "Fill radius " << fill_radius << std::endl;
    std::cout << "Fill bottom " << fill_bottom << std::endl;
    std::cout << "Fill top " << fill_top << std::endl;

    ChVector<float> center(0, 0, fill_bottom);
    center.z() += 2 * params.sphere_radius;
    while (center.z() < fill_top - 2 * params.sphere_radius) {
        auto points = sampler.SampleCylinderZ(center, fill_radius, 0);
        body_points.insert(body_points.end(), points.begin(), points.end());
        center.z() += 2.1 * params.sphere_radius;
    }

    ChGpuSMC_API apiSMC;
    apiSMC.setSystem(&gpu_sys);
    apiSMC.setElemsPositions(body_points);

    float g[3];
    std::vector<string> mesh_filenames;
    std::string mesh_filename;
    mesh_filename = gpu::GetDataFile("demo_GPU_mixer/internal_mixer.obj");
    g[0] = 0;
    g[1] = 0;
    g[2] = -980;

    gpu_sys.set_gravitational_acceleration(g[0], g[1], g[2]);

    mesh_filenames.push_back(mesh_filename);

    std::vector<ChMatrix33<float>> mesh_rotscales;
    std::vector<float3> mesh_translations;

    float scale_xy = Bx / 2.f;
    float scale_z = chamber_height;  // TODO fix this / make switch on mixer_type
    float3 scaling = make_float3(scale_xy, scale_xy, scale_z);
    mesh_rotscales.push_back(ChMatrix33<float>(ChVector<float>(scaling.x, scaling.y, scaling.z)));
    mesh_translations.push_back(make_float3(0, 0, 0));

    std::vector<float> mesh_masses;
    float mixer_mass = 10;
    mesh_masses.push_back(mixer_mass);

    apiSMC_TriMesh.load_meshes(mesh_filenames, mesh_rotscales, mesh_translations, mesh_masses);

    unsigned int nSoupFamilies = gpu_sys.getNumTriangleFamilies();
    std::cout << nSoupFamilies << " soup families" << std::endl;
    double* mesh_pos_rot = new double[7 * nSoupFamilies];
    float* mesh_vel = new float[6 * nSoupFamilies]();

    float rev_per_sec = 1.f;
    float ang_vel_Z = rev_per_sec * 2 * CH_C_PI;
    mesh_vel[5] = ang_vel_Z;

    gpu_sys.enableMeshCollision();
    gpu_sys.initialize();

    unsigned int currframe = 0;
    double out_fps = 200;
    float frame_step = 1.f / out_fps;  // Duration of a frame
    unsigned int out_steps = frame_step / iteration_step;
    unsigned int total_frames = (unsigned int)((float)params.time_end * out_fps);
    std::cout << "out_steps " << out_steps << std::endl;

    unsigned int step = 0;

    for (float t = 0; t < params.time_end; t += iteration_step, step++) {
        mesh_pos_rot[0] = 0;
        mesh_pos_rot[1] = 0;
        mesh_pos_rot[2] = chamber_bottom + chamber_height / 2.0;

        auto q = Q_from_AngZ(t * ang_vel_Z);
        mesh_pos_rot[3] = q[0];
        mesh_pos_rot[4] = q[1];
        mesh_pos_rot[5] = q[2];
        mesh_pos_rot[6] = q[3];

        gpu_sys.meshSoup_applyRigidBodyMotion(mesh_pos_rot, mesh_vel);
        if (step % out_steps == 0) {
            std::cout << "Rendering frame " << (currframe+1) << " of " << total_frames << std::endl;
            char filename[100];
            sprintf(filename, "%s/step%06u", params.output_dir.c_str(), currframe++);
            gpu_sys.writeFile(std::string(filename));
            gpu_sys.write_meshes(std::string(filename));

            float forces[6];
            gpu_sys.collectGeneralizedForcesOnMeshSoup(forces);
            std::cout << "torque: " << forces[3] << ", " << forces[4] << ", " << forces[5] << std::endl;
        }

        gpu_sys.advance_simulation(iteration_step);
    }

    delete[] mesh_pos_rot;
    delete[] mesh_vel;

    return 0;
}
