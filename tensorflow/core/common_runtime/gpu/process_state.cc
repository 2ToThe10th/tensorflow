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

#include "tensorflow/core/common_runtime/gpu/process_state.h"

#include "tensorflow/stream_executor/multi_platform_manager.h"
#include "tensorflow/core/common_runtime/gpu/gpu_bfc_allocator.h"
#include "tensorflow/core/common_runtime/gpu/gpu_debug_allocator.h"
#include "tensorflow/core/common_runtime/gpu/gpu_init.h"
#include "tensorflow/core/common_runtime/gpu/gpu_region_allocator.h"
#include "tensorflow/core/common_runtime/gpu/pool_allocator.h"
#include "tensorflow/core/framework/allocator.h"
#include "tensorflow/core/lib/strings/strcat.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/mutex.h"
#include "tensorflow/core/platform/port.h"

#if defined(PLATFORM_GOOGLE)
// TODO(vrv): Remove these flags and add them as options to config.proto
//#include "base/commandlineflags.h"
DEFINE_bool(record_mem_types, false,
            "If true, record attributes of memory allocations and "
            "dyanmically check for appropriate use of registered memory."
            "Should only be true for debugging or diagnosis of "
            "performance issues.");
DEFINE_bool(brain_mem_reg_cuda_dma, true,
            "If true, register CPU RAM used to copy to/from GPU RAM "
            "with the CUDA driver.");
DEFINE_bool(brain_gpu_use_bfc_allocator, true,
            "If true, uses the Best-Fit GPU allocator.");
DEFINE_bool(brain_gpu_region_allocator_debug, false,
            "If true, checks for memory overwrites by writing "
            "distinctive patterns on both ends of allocated memory.");
DEFINE_bool(brain_gpu_region_allocator_reset_to_nan, false,
            "If true, initializes all new Malloc buffers to NaN, "
            "and resets the buffer to NaN upon Free.");

#else
bool FLAGS_record_mem_types = false;
bool FLAGS_brain_mem_reg_cuda_dma = true;
bool FLAGS_brain_gpu_region_allocator_debug = false;
bool FLAGS_brain_gpu_region_allocator_reset_to_nan = false;
bool FLAGS_brain_gpu_use_bfc_allocator = true;
#endif

namespace gpu = ::perftools::gputools;

namespace tensorflow {

ProcessState* ProcessState::instance_ = nullptr;

/*static*/ ProcessState* ProcessState::singleton() {
  if (instance_ == nullptr) {
    instance_ = new ProcessState;
  }

  return instance_;
}

ProcessState::ProcessState() : gpu_count_(0) {
  CHECK(instance_ == nullptr);
  instance_ = this;
}

ProcessState::~ProcessState() {
  for (auto p : gpu_allocators_) {
    delete p;
  }
  instance_ = nullptr;
}

string ProcessState::MemDesc::DebugString() {
  return strings::StrCat((loc == CPU ? "CPU " : "GPU "), dev_index, ", dma: ",
                         gpu_registered, ", nic: ", nic_registered);
}

ProcessState::MemDesc ProcessState::PtrType(const void* ptr) {
  if (FLAGS_record_mem_types) {
    auto iter = mem_desc_map_.find(ptr);
    if (iter != mem_desc_map_.end()) {
      return iter->second;
    }
  }
  return MemDesc();
}

void ProcessState::SetGPUCount(int c) {
  CHECK(gpu_count_ == 0 || gpu_count_ == c)
      << "Cannot call SetGPUCount with a non-zero value "
      << "not equal to prior set value.";
  gpu_count_ = c;
}

int ProcessState::GPUCount() const { return gpu_count_; }

Allocator* ProcessState::GetGPUAllocator(int gpu_id, size_t total_bytes,
                                         const string& allocator_type) {
#if GOOGLE_CUDA
  mutex_lock lock(mu_);
  gpu::Platform* gpu_platform = GPUMachineManager();

  // Verify that gpu_id is legitimate.
  CHECK_LT(gpu_id, gpu_platform->VisibleDeviceCount())
      << "gpu_id is outside discovered device range";

  if (gpu_id >= static_cast<int64>(gpu_allocators_.size())) {
    gpu_allocators_.resize(gpu_id + 1);
    if (FLAGS_record_mem_types) gpu_al_.resize(gpu_id + 1);
  }

  if (gpu_allocators_[gpu_id] == nullptr) {
    VisitableAllocator* gpu_allocator;

    // Validate allocator types.
    if (!allocator_type.empty() && allocator_type != "BFC") {
      LOG(ERROR) << "Invalid allocator type: " << allocator_type;
      return nullptr;
    }

    if (FLAGS_brain_gpu_use_bfc_allocator || allocator_type == "BFC") {
      gpu_allocator = new GPUBFCAllocator(gpu_id, total_bytes);
    } else {
      gpu_allocator = new GPURegionAllocator(gpu_id, total_bytes);
    }

    if (FLAGS_brain_gpu_region_allocator_debug) {
      gpu_allocator = new GPUDebugAllocator(gpu_allocator, gpu_id);
    }
    if (FLAGS_brain_gpu_region_allocator_reset_to_nan) {
      gpu_allocator = new GPUNanResetAllocator(gpu_allocator, gpu_id);
    }

    gpu_allocators_[gpu_id] = gpu_allocator;

    // If there are any pending AllocVisitors for this bus, add
    // them now.
    gpu::StreamExecutor* se =
        gpu_platform->ExecutorForDevice(gpu_id).ValueOrDie();
    int bus_id = se->GetDeviceDescription().numa_node();
    if (bus_id < static_cast<int64>(gpu_visitors_.size())) {
      for (auto v : gpu_visitors_[bus_id]) {
        gpu_allocators_[gpu_id]->AddAllocVisitor(v);
      }
    }
    if (FLAGS_record_mem_types) {
      MemDesc md;
      md.loc = MemDesc::GPU;
      md.dev_index = gpu_id;
      md.gpu_registered = false;
      md.nic_registered = true;
      if (static_cast<int64>(gpu_al_.size()) <= gpu_id)
        gpu_al_.resize(gpu_id + 1);
      gpu_al_[gpu_id] = new internal::RecordingAllocator(
          &mem_desc_map_, gpu_allocators_[gpu_id], md, &mu_);
    }
  }
  if (FLAGS_record_mem_types) return gpu_al_[gpu_id];
  return gpu_allocators_[gpu_id];
#else
  LOG(FATAL) << "GPUAllocator unavailable. Not compiled with --config=cuda.";
  return nullptr;
#endif  // GOOGLE_CUDA
}

Allocator* ProcessState::GetCPUAllocator(int numa_node) {
  // Although we're temporarily ignoring numa_node, check for legality.
  CHECK_GE(numa_node, 0);
  // TODO(tucker): actually maintain separate CPUAllocators for
  // different numa_nodes.  For now, just one.
  numa_node = 0;
  mutex_lock lock(mu_);
  while (cpu_allocators_.size() <= static_cast<size_t>(numa_node)) {
    cpu_allocators_.push_back(new PoolAllocator(
        100 /*pool_size_limit*/, true /*auto_resize*/, new BasicCPUAllocator(),
        new NoopRounder, "cpu_pool"));
  }
  return cpu_allocators_[0];
}

Allocator* ProcessState::GetCUDAHostAllocator(int numa_node) {
  if (gpu_count_ == 0 || !FLAGS_brain_mem_reg_cuda_dma) {
    return GetCPUAllocator(numa_node);
  }
  // Although we're temporarily ignoring numa_node, check for legality.
  CHECK_GE(numa_node, 0);
  // TODO(tucker): actually maintain separate CPUAllocators for
  // different numa_nodes.  For now, just one.
  numa_node = 0;
  mutex_lock lock(mu_);
  while (static_cast<int>(cuda_host_allocators_.size()) <= numa_node) {
    // CUDAHost alloc the same across all gpus, so just get the
    // executor for the first device.
    gpu::Platform* gpu_platform = GPUMachineManager();
    gpu::StreamExecutor* se = gpu_platform->ExecutorForDevice(0).ValueOrDie();
    CHECK(se);
    cuda_host_allocators_.push_back(new PoolAllocator(
        100 /*pool_size_limit*/, true /*auto_resize*/,
        new CUDAHostAllocator(se), new Pow2Rounder, "cuda_host"));
    if (FLAGS_record_mem_types) {
      MemDesc md;
      md.loc = MemDesc::CPU;
      md.dev_index = 0;
      md.gpu_registered = true;
      md.nic_registered = false;
      cuda_al_.push_back(new internal::RecordingAllocator(
          &mem_desc_map_, cuda_host_allocators_.back(), md, &mu_));
    }
  }
  if (FLAGS_record_mem_types) return cuda_al_[0];
  return cuda_host_allocators_[0];
}

void ProcessState::AddGPUAllocVisitor(int bus_id, AllocVisitor visitor) {
#if GOOGLE_CUDA
  mutex_lock lock(mu_);
  gpu::Platform* gpu_platform = GPUMachineManager();
  for (int gpu_id = 0; gpu_id < static_cast<int64>(gpu_allocators_.size());
       ++gpu_id) {
    gpu::StreamExecutor* se =
        gpu_platform->ExecutorForDevice(gpu_id).ValueOrDie();
    if (gpu_allocators_[gpu_id] &&
        se->GetDeviceDescription().numa_node() == bus_id) {
      gpu_allocators_[gpu_id]->AddAllocVisitor(visitor);
    }
  }
  while (bus_id >= static_cast<int64>(gpu_visitors_.size())) {
    gpu_visitors_.push_back(std::vector<AllocVisitor>());
  }
  gpu_visitors_[bus_id].push_back(visitor);
#endif  // GOOGLE_CUDA
}

}  // namespace tensorflow
