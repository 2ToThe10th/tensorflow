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

#ifndef TENSORFLOW_COMPILER_XLA_SERVICE_CPU_KERNEL_SUPPORT_LIBRARY_H_
#define TENSORFLOW_COMPILER_XLA_SERVICE_CPU_KERNEL_SUPPORT_LIBRARY_H_

#include <string>

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Value.h"
#include "tensorflow/compiler/xla/service/llvm_ir/llvm_loop.h"
#include "tensorflow/compiler/xla/service/llvm_ir/llvm_util.h"
#include "tensorflow/core/lib/core/stringpiece.h"

namespace xla {
// A thin wrapper around llvm_loop.h to make code generating structured control
// flow more readable.
class KernelSupportLibrary {
 public:
  // `b` is the llvm::IRBuilder instance used to generate LLVM IR.
  // `unroll_mode` specifies the desired LLVM unrolling behavior for every loop
  // generated by this instance of KernelSupportLibrary.
  explicit KernelSupportLibrary(
      llvm::IRBuilder<>* b,
      llvm_ir::UnrollMode unroll_mode = llvm_ir::UnrollMode::kNoUnroll,
      bool prevent_vectorization = true)
      : b_(b),
        unroll_mode_(unroll_mode),
        prevent_vectorization_(prevent_vectorization) {}

  // Generates the following control flow structure:
  //
  //   if (`start` < `end`) {
  //     `for_body_generator(/*ind_var=*/start, /*is_first_iteration=*/true)`;
  //     for (i64 i = `start` + `step`; i s< `end`; i += `step`)
  //       `for_body_generator(/*ind_var=*/,i, /*is_first_iteration=*/false)`;
  //   }
  Status For(
      tensorflow::StringPiece name, llvm::Value* start, llvm::Value* end,
      llvm::Value* step,
      const std::function<Status(llvm::Value* ind_var,
                                 bool is_first_iteration)>& for_body_generator);

  void ForReturnVoid(
      tensorflow::StringPiece name, llvm::Value* start, llvm::Value* end,
      llvm::Value* step,
      const std::function<void(llvm::Value* ind_var, bool is_first_iteration)>&
          for_body_generator) {
    CHECK_EQ(Status::OK(),
             For(name, start, end, step,
                 [&](llvm::Value* ind_var, bool is_first_iteration) -> Status {
                   for_body_generator(ind_var, is_first_iteration);
                   return Status::OK();
                 }));
  }

  Status For(tensorflow::StringPiece name, int64 start, int64 end, int64 step,
             const std::function<Status(llvm::Value* ind_var,
                                        bool is_first_iteration)>&
                 for_body_generator) {
    return For(name, /*start=*/b_->getInt64(start),
               /*end=*/b_->getInt64(end),
               /*step=*/b_->getInt64(step), for_body_generator);
  }

  void ForReturnVoid(
      tensorflow::StringPiece name, int64 start, int64 end, int64 step,
      const std::function<void(llvm::Value* ind_var, bool is_first_iteration)>&
          for_body_generator) {
    ForReturnVoid(name, /*start=*/b_->getInt64(start),
                  /*end=*/b_->getInt64(end),
                  /*step=*/b_->getInt64(step), for_body_generator);
  }

  // Generates the following control flow structure if `peel_first_iteration` is
  // true:
  //
  //   if (`start` < `end`) {
  //     `for_body_generator(/*ind_var=*/start, /*is_first_iteration=*/,true)`;
  //     for (i64 i = `start` + `step`; i s< `end`; i += `step`)
  //       `for_body_generator(/*ind_var=*/,i, /*is_first_iteration=*/,false)`;
  //   }
  //
  // and the following if `peel_first_iteration` is false:
  //
  //   for (i64 i = `start`; i s< `end`; i += `step`)
  //     `for_body_generator(/*ind_var=*/,i,
  //                         /*is_first_iteration=*/,(i != `start`))`;
  Status For(tensorflow::StringPiece name, llvm::Value* start, llvm::Value* end,
             llvm::Value* step, bool peel_first_iteration,
             const std::function<Status(llvm::Value* ind_var,
                                        llvm::Value* is_first_iteration)>&
                 for_body_generator);

  void ForReturnVoid(tensorflow::StringPiece name, llvm::Value* start,
                     llvm::Value* end, llvm::Value* step,
                     bool peel_first_iteration,
                     const std::function<void(llvm::Value* ind_var,
                                              llvm::Value* is_first_iteration)>&
                         for_body_generator) {
    TF_CHECK_OK(For(
        name, start, end, step, peel_first_iteration,
        [&](llvm::Value* ind_var, llvm::Value* is_first_iteration) -> Status {
          for_body_generator(ind_var, is_first_iteration);
          return Status::OK();
        }));
  }

  Status For(tensorflow::StringPiece name, llvm::Value* start, llvm::Value* end,
             int64 step, bool peel_first_iteration,
             const std::function<Status(llvm::Value* ind_var,
                                        llvm::Value* is_first_iteration)>&
                 for_body_generator) {
    return For(name, /*start=*/start, /*end=*/end,
               /*step=*/llvm::ConstantInt::get(start->getType(), step),
               peel_first_iteration, for_body_generator);
  }

  void ForReturnVoid(tensorflow::StringPiece name, llvm::Value* start,
                     llvm::Value* end, int64 step, bool peel_first_iteration,
                     const std::function<void(llvm::Value* ind_var,
                                              llvm::Value* is_first_iteration)>&
                         for_body_generator) {
    ForReturnVoid(name, /*start=*/start, /*end=*/end,
                  /*step=*/llvm::ConstantInt::get(start->getType(), step),
                  peel_first_iteration, for_body_generator);
  }

  Status For(
      tensorflow::StringPiece name, llvm::Value* start, llvm::Value* end,
      llvm::Value* step,
      const std::function<Status(llvm::Value* ind_var)>& for_body_generator) {
    return For(name, start, end, step,
               /*peel_first_iteration=*/false,
               [&](llvm::Value* indvar, llvm::Value*) -> Status {
                 return for_body_generator(indvar);
               });
  }

  void ForReturnVoid(
      tensorflow::StringPiece name, llvm::Value* start, llvm::Value* end,
      llvm::Value* step,
      const std::function<void(llvm::Value* ind_var)>& for_body_generator) {
    ForReturnVoid(name, start, end, step,
                  /*peel_first_iteration=*/false,
                  [&](llvm::Value* indvar, llvm::Value*) {
                    return for_body_generator(indvar);
                  });
  }

  Status For(
      tensorflow::StringPiece name, llvm::Value* start, llvm::Value* end,
      int64 step,
      const std::function<Status(llvm::Value* ind_var)>& for_body_generator) {
    return For(name, start, end, llvm::ConstantInt::get(start->getType(), step),
               /*peel_first_iteration=*/false,
               [&](llvm::Value* indvar, llvm::Value*) -> Status {
                 return for_body_generator(indvar);
               });
  }

  void ForReturnVoid(
      tensorflow::StringPiece name, llvm::Value* start, llvm::Value* end,
      int64 step,
      const std::function<void(llvm::Value* ind_var)>& for_body_generator) {
    ForReturnVoid(name, start, end,
                  llvm::ConstantInt::get(start->getType(), step),
                  for_body_generator);
  }

  Status For(
      tensorflow::StringPiece name, int64 start, int64 end, int64 step,
      const std::function<Status(llvm::Value* ind_var)>& for_body_generator) {
    return For(name, /*start=*/b_->getInt64(start),
               /*end=*/b_->getInt64(end),
               /*step=*/b_->getInt64(step), for_body_generator);
  }

  void ForReturnVoid(
      tensorflow::StringPiece name, int64 start, int64 end, int64 step,
      const std::function<void(llvm::Value* ind_var)>& for_body_generator) {
    ForReturnVoid(name, /*start=*/b_->getInt64(start),
                  /*end=*/b_->getInt64(end),
                  /*step=*/b_->getInt64(step), for_body_generator);
  }

  // Generates the following control flow structure:
  //
  //   if (`condition`)
  //     `true_block_generator()`;
  //   else
  //      `false_block_generator()`;
  Status If(tensorflow::StringPiece name, llvm::Value* condition,
            const std::function<Status()>& true_block_generator,
            const std::function<Status()>& false_block_generator =
                []() -> Status { return Status::OK(); });

  Status If(llvm::Value* condition,
            const std::function<Status()>& true_block_generator,
            const std::function<Status()>& false_block_generator =
                []() -> Status { return Status::OK(); }) {
    return If("", condition, true_block_generator, false_block_generator);
  }

  void IfReturnVoid(llvm::Value* condition,
                    const std::function<void()>& true_block_generator,
                    const std::function<void()>& false_block_generator = []() {
                    }) {
    IfReturnVoid("", condition, true_block_generator, false_block_generator);
  }

  void IfReturnVoid(tensorflow::StringPiece name, llvm::Value* condition,
                    const std::function<void()>& true_block_generator,
                    const std::function<void()>& false_block_generator = []() {
                    }) {
    TF_CHECK_OK(If(name, condition,
                   [&]() {
                     true_block_generator();
                     return Status::OK();
                   },
                   [&]() {
                     false_block_generator();
                     return Status::OK();
                   }));
  }

  using ArgumentVector = tensorflow::gtl::ArraySlice<llvm::Value*>;

  // Generates the following control flow structure:
  //
  //  define @`kernel_name`(arg0, arg1, ... arg`arguments.size()`) {
  //    kernel_body_generator({arg0, arg1, ... arg`arguments.size()`});
  //  }
  //
  //  ...
  //  call @`kernel_name`(arguments[0], arguments[1] ...)
  //  ...
  //
  // If a function called `kernel_name` is already present in the module then
  // that function is re-used.  In that sense we're using the llvm::Module as a
  // cache of outlined kernels, keyed by function name.
  //
  // If any of the values in `arguments` is nullptr (i.e. a nullptr
  // llvm::Value*) then we ignore it when generating LLVM IR, and instead pass
  // in a nullptr llvm::Value* in its position to `kernel_body_generator`.
  // Currently we only support at most one nullptr value in `arguments`.
  static void EmitAndCallOutlinedKernel(
      bool enable_fast_math, bool optimize_for_size, llvm::IRBuilder<>* b,
      tensorflow::StringPiece kernel_name, ArgumentVector arguments,
      const std::function<void(ArgumentVector)>& kernel_body_generator);

  // Thin wrappers around the more general EmitAndCallOutlinedKernel above.
  static void EmitAndCallOutlinedKernel(
      bool enable_fast_math, bool optimize_for_size, llvm::IRBuilder<>* b,
      tensorflow::StringPiece kernel_name, llvm::Value* arg0, llvm::Value* arg1,
      llvm::Value* arg2,
      const std::function<void(llvm::Value*, llvm::Value*, llvm::Value*)>&
          kernel_body_generator) {
    EmitAndCallOutlinedKernel(
        enable_fast_math, optimize_for_size, b, kernel_name, {arg0, arg1, arg2},
        [&](ArgumentVector args) {
          kernel_body_generator(args[0], args[1], args[2]);
        });
  }

  static void EmitAndCallOutlinedKernel(
      bool enable_fast_math, bool optimize_for_size, llvm::IRBuilder<>* b,
      tensorflow::StringPiece kernel_name, llvm::Value* arg0, llvm::Value* arg1,
      llvm::Value* arg2, llvm::Value* arg3,
      const std::function<void(llvm::Value*, llvm::Value*, llvm::Value*,
                               llvm::Value*)>& kernel_body_generator) {
    EmitAndCallOutlinedKernel(
        enable_fast_math, optimize_for_size, b, kernel_name,
        {arg0, arg1, arg2, arg3}, [&](ArgumentVector args) {
          kernel_body_generator(args[0], args[1], args[2], args[3]);
        });
  }

 private:
  llvm::IRBuilder<>* b_;
  llvm_ir::UnrollMode unroll_mode_;
  bool prevent_vectorization_;
};
}  // namespace xla

#endif  // TENSORFLOW_COMPILER_XLA_SERVICE_CPU_KERNEL_SUPPORT_LIBRARY_H_
