#include <cub/cub.cuh>
#include <cub/device/device_reduce.cuh>
#include <cub/util_allocator.cuh>
#include <cub/util_debug.cuh>
#include <iostream>
#include <core/utils/GpuError.h>
#include "example.h"

void cubIntRunLength(int in[], int in_size, int unique_out[], int count_out[], int run_out[]){

    int* tmp = NULL;
    GPU_CALL(cudaMallocManaged(&tmp, 1));

    cub::CachingDeviceAllocator g_allocator(true); 

    // Set up device arrays
    int *d_in;
    g_allocator.DeviceAllocate((void **)&d_in, sizeof(int) * in_size);
    cudaMemcpy(d_in, in, sizeof(int) * in_size, cudaMemcpyHostToDevice);

    // Set up device arrays
    int *d_unique;
    g_allocator.DeviceAllocate((void **)&d_unique, sizeof(int) * in_size);

    // Set up device keys
    int *d_count;
    g_allocator.DeviceAllocate((void **)&d_count, sizeof(int) * in_size);

    // set up res key arrays
    int *d_run_out;
    g_allocator.DeviceAllocate((void **)&d_run_out, sizeof(int) * in_size);

    void *d_temp_storage_1 = NULL;
    size_t temp_storage_bytes_1 = 0;

    cub::DeviceRunLengthEncode::Encode(d_temp_storage_1, temp_storage_bytes_1,
                                     d_in, d_unique, d_count, d_run_out, in_size);

    g_allocator.DeviceAllocate(&d_temp_storage_1, temp_storage_bytes_1);

    cub::DeviceRunLengthEncode::Encode(d_temp_storage_1, temp_storage_bytes_1,
                                     d_in, d_unique, d_count, d_run_out, in_size);

    // std::cout << "d_unique: " << std::endl;
    // for (int i = 0; i < 7; i++) {
    //     std::cout << d_unique[i] << " ";
    // }
    // std::cout << std::endl;

    // std::cout << "d_count: " << std::endl;
    // for (int i = 0; i < 7; i++) {
    //     std::cout << d_count[i] << " ";
    // }
    // std::cout << std::endl;

    // std::cout << "d_run_out: " << std::endl;
    // for (int i = 0; i < 7; i++) {
    //     std::cout << d_run_out[i] << " ";
    // }
    // std::cout << std::endl;


    cudaDeviceSynchronize();


    cudaMemcpy(unique_out, d_unique, sizeof(int) * in_size, cudaMemcpyDeviceToHost);
    cudaMemcpy(count_out, d_count, sizeof(int) * in_size, cudaMemcpyDeviceToHost);
    cudaMemcpy(run_out, d_run_out, sizeof(int) * in_size, cudaMemcpyDeviceToHost);

    // Cleanup
    if (d_in)
        CubDebugExit(g_allocator.DeviceFree(d_in));
    if (d_unique)
        CubDebugExit(g_allocator.DeviceFree(d_unique));
    if (d_count)
        CubDebugExit(g_allocator.DeviceFree(d_count));
    if (d_run_out)
        CubDebugExit(g_allocator.DeviceFree(d_run_out));
    if (d_temp_storage_1)
        CubDebugExit(g_allocator.DeviceFree(d_temp_storage_1));



}