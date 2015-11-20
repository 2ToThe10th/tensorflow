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

#include "tensorflow/core/framework/numeric_types.h"
#include "tensorflow/core/kernels/reduction_ops.h"

namespace tensorflow {
namespace functor {

typedef Eigen::GpuDevice GPUDevice;

// Derive Index type. int (32-bit) or long (64-bit) depending on the
// compile-time configuration. "float" here is not relevant.
// TODO(zhifengc): Moves the definition to TTypes.
typedef TTypes<float>::Tensor::Index Index;

template <>
struct ReduceFunctor<GPUDevice> {
  template <typename OUT_T, typename IN_T, typename ReductionAxes,
            typename Reducer>
  static void Reduce(const GPUDevice& d, OUT_T out, IN_T in,
                     const ReductionAxes& reduction_axes,
                     const Reducer& reducer) {
    ReduceEigenImpl(d, To32Bit(out), To32Bit(in), reduction_axes, reducer);
  }
};

// T: the data type
// REDUCER: the reducer functor
// NUM_AXES: the number of axes to reduce
// IN_DIMS: the number of dimensions of the input tensor
#define DEFINE(T, REDUCER, IN_DIMS, NUM_AXES)                        \
  template void ReduceFunctor<GPUDevice>::Reduce(                    \
      const GPUDevice& d, TTypes<T, IN_DIMS - NUM_AXES>::Tensor out, \
      TTypes<T, IN_DIMS>::ConstTensor in,                            \
      const Eigen::array<Index, NUM_AXES>& reduction_axes,           \
      const REDUCER& reducer);

#define DEFINE_FOR_TYPE_AND_R(T, R) \
  DEFINE(T, R, 1, 1);               \
  DEFINE(T, R, 2, 1);               \
  DEFINE(T, R, 3, 1);               \
  DEFINE(T, R, 3, 2);

#define DEFINE_FOR_ALL_REDUCERS(T)                          \
  DEFINE_FOR_TYPE_AND_R(T, Eigen::internal::SumReducer<T>); \
  DEFINE_FOR_TYPE_AND_R(T, Eigen::internal::MinReducer<T>); \
  DEFINE_FOR_TYPE_AND_R(T, Eigen::internal::MaxReducer<T>); \
  DEFINE_FOR_TYPE_AND_R(T, Eigen::internal::ProdReducer<T>)

DEFINE_FOR_ALL_REDUCERS(float);
#undef DEFINE_FOR_ALL_REDUCERS

DEFINE_FOR_TYPE_AND_R(complex64, Eigen::internal::SumReducer<complex64>);
DEFINE_FOR_TYPE_AND_R(bool, AllReducer);
DEFINE_FOR_TYPE_AND_R(bool, AnyReducer);
#undef DEFINE_FOR_TYPE_AND_R

#undef DEFINE

}  // end namespace functor
}  // end namespace tensorflow

#endif  // GOOGLE_CUDA
