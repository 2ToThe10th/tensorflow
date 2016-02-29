/* Copyright 2015 Google Inc. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#if GOOGLE_CUDA

#define EIGEN_USE_GPU

#include <stdio.h>

#include "tensorflow/core/kernels/resize_nearest_neighbor_op_gpu.h"

#include "tensorflow/core/framework/register_types.h"
#include "tensorflow/core/framework/tensor_types.h"
#include "tensorflow/core/util/cuda_kernel_helper.h"

namespace tensorflow {
namespace {

template <typename T>
__global__ void ResizeNearestNeighborNHWC(const int nthreads, const T* bottom_data,
                                          const int in_height, const int in_width,
                                          const int channels, const float height_scale,
                                          const float width_scale, const int out_height,
                                          const int out_width, T* top_data) {
  CUDA_1D_KERNEL_LOOP(index, nthreads) {
    int n = index;
    int c = n % channels;
    n /= channels;
    int out_x = n % out_width;
    n /= out_width;
    int out_y = n % out_height;
    n /= out_height;

    const T* bottom_data_n = bottom_data + n * channels * in_height * in_width;
    const int in_x = min(static_cast<int>(floorf(out_x * width_scale)), in_width - 1);
    const int in_y = min(static_cast<int>(floorf(out_y * height_scale)), in_height - 1);
    const int idx = (in_y * in_height + in_x) * channels + c;
    top_data[index] = bottom_data_n[idx];
  }
}

}  // namespace

template <typename T>
bool ResizeNearestNeighbor(const T* bottom_data, const int batch,
                           const int in_height, const int in_width,
                           const int channels, const int out_height,
                           const int out_width,  const float height_scale,
                           const float width_scale, T* top_data,
                           const Eigen::GpuDevice& d) {
  const int output_size = batch * channels * out_height * out_width;
  CudaLaunchConfig config = GetCudaLaunchConfig(output_size, d);

  ResizeNearestNeighborNHWC<T>
      <<<config.block_count, config.thread_per_block, 0, d.stream()>>>(
      output_size, bottom_data, in_height, in_width, channels, out_height,
      out_width, height_scale, width_scale, top_data);
  return d.ok();
}

#define DECLARE_GPU_SPEC(T)                                                        \
  template bool ResizeNearestNeighbor(const T* bottom_data, const int batch,       \
                               const int in_height, const int in_width,            \
                               const int channels, const int out_height,           \
                               const int out_width,  const float height_scale,     \
                               const float width_scale, T* top_data,               \
                               const Eigen::GpuDevice& d);

DECLARE_GPU_SPEC(float);

#undef DECLARE_GPU_SPEC
}  // end namespace tensorflow

#endif  // GOOGLE_CUDA
