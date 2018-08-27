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

// A simple CPU allocator that intercepts malloc/free calls from MKL library
// and redirects them to Tensorflow allocator

#ifndef TENSORFLOW_CORE_COMMON_RUNTIME_MKL_CPU_ALLOCATOR_H_
#define TENSORFLOW_CORE_COMMON_RUNTIME_MKL_CPU_ALLOCATOR_H_

#ifdef INTEL_MKL

#include <cstdlib>
#include "tensorflow/core/common_runtime/bfc_allocator.h"
#include "tensorflow/core/common_runtime/visitable_allocator.h"
#include "tensorflow/core/lib/strings/numbers.h"
#include "tensorflow/core/lib/strings/str_util.h"
#include "tensorflow/core/platform/mem.h"
#include "tensorflow/core/framework/allocator_registry.h"
#include "tensorflow/core/platform/mutex.h"

#ifndef INTEL_MKL_DNN_ONLY
#include "i_malloc.h"
#endif

#ifdef _WIN32
typedef unsigned int uint;
#endif

namespace tensorflow {

class MklSubAllocator : public SubAllocator {
 public:
  ~MklSubAllocator() override {}

  void* Alloc(size_t alignment, size_t num_bytes) override {
    return port::AlignedMalloc(num_bytes, alignment);
  }
  void Free(void* ptr, size_t num_bytes) override { port::AlignedFree(ptr); }
};

/// CPU allocator that handles small-size allocations by calling
/// suballocator directly. Mostly, it is just a wrapper around a suballocator
/// (that calls malloc and free directly) with support for bookkeeping.
class MklSmallSizeAllocator : public VisitableAllocator {
 public:
  MklSmallSizeAllocator(SubAllocator* sub_allocator, size_t total_memory,
                        const string& name) : sub_allocator_(sub_allocator),
                        name_(name) {
    stats_.bytes_limit = total_memory;
  }
  ~MklSmallSizeAllocator() override {}

  TF_DISALLOW_COPY_AND_ASSIGN(MklSmallSizeAllocator);

  inline string Name() override { return name_; }

  void* AllocateRaw(size_t alignment, size_t num_bytes) override {
    void* ptr = nullptr;
    if ((ptr = sub_allocator_->Alloc(alignment, num_bytes)) != nullptr) {
      std::pair<void*, size_t> map_val(ptr, num_bytes);
      mutex_lock l(mutex_);
      // Check that insertion in the hash map was successful.
      CHECK_EQ(map_.insert(map_val).second, true);
      // Increment statistics for small-size allocations.
      IncrementStats(num_bytes);
      // Call alloc visitors.
      for (const auto& visitor : alloc_visitors_) {
        visitor(ptr, num_bytes);
      }
    }
    return ptr;
  }

  void DeallocateRaw(void* ptr) override {
    if (ptr == nullptr) {
      LOG(ERROR) << "tried to deallocate nullptr";
      return;
    }

    mutex_lock l(mutex_);
    auto map_iter = map_.find(ptr);
    if (map_iter != map_.end()) {
      // Call free visitors.
      size_t dealloc_bytes = map_iter->second;
      for (const auto& visitor : free_visitors_) {
        visitor(ptr, dealloc_bytes);
      }
      sub_allocator_->Free(ptr, dealloc_bytes);
      DecrementStats(dealloc_bytes);
      map_.erase(map_iter);
    }
  }

  inline bool IsSmallSizeAllocation(const void* ptr) const {
    mutex_lock l(mutex_);
    return map_.find(ptr) != map_.end();
  }

  void GetStats(AllocatorStats* stats) override {
    mutex_lock l(mutex_);
    *stats = stats_;
  }

  void ClearStats() override {
    mutex_lock l(mutex_);
    stats_.Clear();
  }

  void AddAllocVisitor(Visitor visitor) override {
    mutex_lock l(mutex_);
    alloc_visitors_.push_back(visitor);
  }

  void AddFreeVisitor(Visitor visitor) override {
    mutex_lock l(mutex_);
    free_visitors_.push_back(visitor);
  }

 private:
  /// Increment statistics for the allocator handling small allocations.
  inline void IncrementStats(size_t alloc_size) {
    ++stats_.num_allocs;
    stats_.bytes_in_use += alloc_size;
    stats_.max_bytes_in_use = std::max(stats_.max_bytes_in_use,
                                       stats_.bytes_in_use);
    stats_.max_alloc_size = std::max(alloc_size,
                                    static_cast<size_t>(stats_.max_alloc_size));
  }

  /// Decrement statistics for the allocator handling small allocations.
  inline void DecrementStats(size_t dealloc_size) {
    stats_.bytes_in_use -= dealloc_size;
  }

  SubAllocator* sub_allocator_;  // Not owned by this class.

  /// Mutex for protecting updates to map of allocations.
  mutable mutex mutex_;

  /// Allocator name
  string name_;

  /// Hash map to keep track of "small" allocations
  /// We do not use BFC allocator for small allocations.
  std::unordered_map<const void*, size_t> map_ GUARDED_BY(mutex_);

  /// Allocator stats for small allocs
  AllocatorStats stats_ GUARDED_BY(mutex_);

  /// Visitors
  std::vector<Visitor> alloc_visitors_ GUARDED_BY(mutex_);
  std::vector<Visitor> free_visitors_ GUARDED_BY(mutex_);
};

/// CPU allocator for MKL that wraps BFC allocator and intercepts
/// and redirects memory allocation calls from MKL.
class MklCPUAllocator : public VisitableAllocator {
 public:
  // Constructor and other standard functions

  /// Environment variable that user can set to upper bound on memory allocation
  static constexpr const char* kMaxLimitStr = "TF_MKL_ALLOC_MAX_BYTES";

  /// Default upper limit on allocator size - 64GB
  static constexpr size_t kDefaultMaxLimit = 64LL << 30;

  MklCPUAllocator() { TF_CHECK_OK(Initialize()); }

  ~MklCPUAllocator() override {
    delete small_size_allocator_;
    delete large_size_allocator_;
  }

  Status Initialize() {
    VLOG(2) << "MklCPUAllocator: In MklCPUAllocator";

    // Set upper bound on memory allocation to physical RAM available on the
    // CPU unless explicitly specified by user
    uint64 max_mem_bytes = kDefaultMaxLimit;
#if defined(_SC_PHYS_PAGES) && defined(_SC_PAGESIZE)
    max_mem_bytes =
        (uint64)sysconf(_SC_PHYS_PAGES) * (uint64)sysconf(_SC_PAGESIZE);
#endif
    char* user_mem_bytes = getenv(kMaxLimitStr);

    if (user_mem_bytes != NULL) {
      uint64 user_val = 0;
      if (!strings::safe_strtou64(user_mem_bytes, &user_val)) {
        return errors::InvalidArgument("Invalid memory limit (", user_mem_bytes,
                                       ") specified for MKL allocator through ",
                                       kMaxLimitStr);
      }
#if defined(_SC_PHYS_PAGES) && defined(_SC_PAGESIZE)
      if (user_val > max_mem_bytes) {
        LOG(WARNING) << "The user specified a memory limit " << kMaxLimitStr
                     << "=" << user_val
                     << " greater than available physical memory: "
                     << max_mem_bytes
                     << ". This could significantly reduce performance!";
      }
#endif
      max_mem_bytes = user_val;
    }

    VLOG(1) << "MklCPUAllocator: Setting max_mem_bytes: " << max_mem_bytes;

    sub_allocator_ = new MklSubAllocator();
    small_size_allocator_ = new MklSmallSizeAllocator(sub_allocator_,
                                                      max_mem_bytes, kName);
    large_size_allocator_ = new BFCAllocator(sub_allocator_, max_mem_bytes,
                                  kAllowGrowth, kName);
#ifndef INTEL_MKL_DNN_ONLY
    // For redirecting all allocations from MKL to this allocator
    // From: http://software.intel.com/en-us/node/528565
    i_malloc = MallocHook;
    i_calloc = CallocHook;
    i_realloc = ReallocHook;
    i_free = FreeHook;
#endif
    return Status::OK();
  }

  inline string Name() override { return kName; }

  inline void* AllocateRaw(size_t alignment, size_t num_bytes) override {
    // If the allocation size is less than threshold, call small allocator,
    // otherwise call large-size allocator (BFC). We found that BFC allocator
    // does not deliver good performance for small allocations when
    // inter_op_parallelism_threads is high.
    return (num_bytes < kSmallAllocationsThreshold) ?
          small_size_allocator_->AllocateRaw(alignment, num_bytes) :
          large_size_allocator_->AllocateRaw(alignment, num_bytes);
  }

  inline void DeallocateRaw(void* ptr) override {
    // Check if ptr is for "small" allocation. If it is, then call Free
    // directly. Otherwise, call BFC to handle free.
    if (small_size_allocator_->IsSmallSizeAllocation(ptr)) {
      small_size_allocator_->DeallocateRaw(ptr);
    } else {
      large_size_allocator_->DeallocateRaw(ptr);
    }
  }

  void GetStats(AllocatorStats* stats) override {
    AllocatorStats l_stats, s_stats;
    small_size_allocator_->GetStats(&s_stats);
    large_size_allocator_->GetStats(&l_stats);

    // Combine statistics from small-size and large-size allocator.
    stats->num_allocs = l_stats.num_allocs + s_stats.num_allocs;
    stats->bytes_in_use = l_stats.bytes_in_use + s_stats.bytes_in_use;
    stats->max_bytes_in_use = l_stats.max_bytes_in_use +
                              s_stats.max_bytes_in_use;
    stats->max_alloc_size = std::max(l_stats.max_alloc_size,
                                     s_stats.max_alloc_size);
  }

  void ClearStats() override {
    small_size_allocator_->ClearStats();
    large_size_allocator_->ClearStats();
  }

  void AddAllocVisitor(Visitor visitor) override {
    small_size_allocator_->AddAllocVisitor(visitor);
    large_size_allocator_->AddAllocVisitor(visitor);
  }

  void AddFreeVisitor(Visitor visitor) override {
    small_size_allocator_->AddFreeVisitor(visitor);
    large_size_allocator_->AddFreeVisitor(visitor);
  }

 private:
  // Hooks provided by this allocator for memory allocation routines from MKL

  static inline void* MallocHook(size_t size) {
    VLOG(3) << "MklCPUAllocator: In MallocHook";
    return cpu_allocator()->AllocateRaw(kAlignment, size);
  }

  static inline void FreeHook(void* ptr) {
    VLOG(3) << "MklCPUAllocator: In FreeHook";
    cpu_allocator()->DeallocateRaw(ptr);
  }

  static inline void* CallocHook(size_t num, size_t size) {
    Status s = Status(error::Code::UNIMPLEMENTED,
                      "Unimplemented case for hooking MKL function.");
    TF_CHECK_OK(s);  // way to assert with an error message
  }

  static inline void* ReallocHook(void* ptr, size_t size) {
    Status s = Status(error::Code::UNIMPLEMENTED,
                      "Unimplemented case for hooking MKL function.");
    TF_CHECK_OK(s);  // way to assert with an error message
  }

  /// Do we allow growth in BFC Allocator
  static const bool kAllowGrowth = true;

  /// Name
  static constexpr const char* kName = "mklcpu";

  /// The alignment that we need for the allocations
  static constexpr const size_t kAlignment = 64;

  VisitableAllocator* large_size_allocator_;  // owned by this class
  MklSmallSizeAllocator* small_size_allocator_;  // owned by this class.

  SubAllocator* sub_allocator_;  // not owned by this class

  /// Size in bytes that defines the upper-bound for "small" allocations.
  /// Any allocation below this threshold is "small" allocation.
  static constexpr const size_t kSmallAllocationsThreshold = 4096;
};

}  // namespace tensorflow

#endif  // INTEL_MKL

#endif  // TENSORFLOW_CORE_COMMON_RUNTIME_MKL_CPU_ALLOCATOR_H_
