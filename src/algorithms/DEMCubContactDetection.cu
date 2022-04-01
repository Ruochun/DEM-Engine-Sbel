//  Copyright (c) 2021, SBEL GPU Development Team
//  Copyright (c) 2021, University of Wisconsin - Madison
//  All rights reserved.

#include <cub/cub.cuh>
#include <core/utils/JitHelper.h>

#include <algorithms/DEMCubHelperFunctions.h>

#include <core/utils/GpuError.h>

namespace sgps {

void cubPrefixScan(binsSphereTouches_t* d_in,
                   binsSphereTouchesScan_t* d_out,
                   size_t n,
                   cudaStream_t& this_stream,
                   DEMSolverStateData& scratchPad) {
    size_t cub_scratch_bytes = 0;
    cub::DeviceScan::ExclusiveSum(NULL, cub_scratch_bytes, d_in, d_out, n, this_stream, false);
    GPU_CALL(cudaStreamSynchronize(this_stream));
    void* d_scratch_space = (void*)scratchPad.allocateScratchSpace(cub_scratch_bytes);
    cub::DeviceScan::ExclusiveSum(d_scratch_space, cub_scratch_bytes, d_in, d_out, n, this_stream, false);
    GPU_CALL(cudaStreamSynchronize(this_stream));
}

void cubSortByKeys(binID_t* d_keys_in,
                   binID_t* d_keys_out,
                   bodyID_t* d_vals_in,
                   bodyID_t* d_vals_out,
                   size_t n,
                   cudaStream_t& this_stream,
                   DEMSolverStateData& scratchPad) {
    size_t cub_scratch_bytes = 0;
    cub::DeviceRadixSort::SortPairs(NULL, cub_scratch_bytes, d_keys_in, d_keys_out, d_vals_in, d_vals_out, n, 0,
                                    sizeof(sgps::binID_t) * BITS_PER_BYTE, this_stream, false);
    GPU_CALL(cudaStreamSynchronize(this_stream));
    void* d_scratch_space = (void*)scratchPad.allocateScratchSpace(cub_scratch_bytes);
    cub::DeviceRadixSort::SortPairs(d_scratch_space, cub_scratch_bytes, d_keys_in, d_keys_out, d_vals_in, d_vals_out, n,
                                    0, sizeof(sgps::binID_t) * BITS_PER_BYTE, this_stream, false);
    GPU_CALL(cudaStreamSynchronize(this_stream));
}

}  // namespace sgps
