/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/core/kernels/dataset_utils.h"

namespace tensorflow {

namespace dataset {

Status MakeIteratorFromInputElement(
    IteratorContext* ctx, const std::vector<Tensor>& input_element,
    int64 thread_index, CapturedFunction* captured_func, StringPiece prefix,
    std::unique_ptr<IteratorBase>* out_iterator) {
  FunctionLibraryRuntime::Options opts;
  opts.runner = ctx->runner();
  // Choose a step ID that is guaranteed not to clash with any
  // Session-generated step ID. DirectSession only generates
  // non-negative step IDs (contiguous, starting from 0), and
  // MasterSession generates 56-bit random step IDs whose MSB
  // is always 0, so a negative random step ID should suffice.
  opts.step_id = CapturedFunction::generate_step_id();
  ScopedStepContainer step_container(
      opts.step_id, [captured_func](const string& name) {
        captured_func->resource_manager()->Cleanup(name).IgnoreError();
      });
  opts.step_container = &step_container;
  std::vector<Tensor> return_values;
  TF_RETURN_IF_ERROR(
      captured_func->RunWithBorrowedArgs(opts, input_element, &return_values));

  if (!(return_values.size() == 1 && return_values[0].dtype() == DT_VARIANT &&
        TensorShapeUtils::IsScalar(return_values[0].shape()))) {
    return errors::InvalidArgument(
        "Function must return a single scalar of dtype DT_VARIANT.");
  }

  // Retrieve the dataset that was created in `f`.
  DatasetBase* returned_dataset;
  TF_RETURN_IF_ERROR(
      GetDatasetFromVariantTensor(return_values[0], &returned_dataset));

  // Create an iterator for the dataset that was returned by `f`.
  *out_iterator = returned_dataset->MakeIterator(
      strings::StrCat(prefix, "[", thread_index, "]"));
  return Status::OK();
}

}  // namespace dataset

}  // namespace tensorflow
