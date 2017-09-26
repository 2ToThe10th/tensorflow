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

#include "tensorflow/compiler/xla/service/tuple_simplifier.h"

#include <memory>
#include <utility>

#include "tensorflow/compiler/xla/literal_util.h"
#include "tensorflow/compiler/xla/service/hlo_computation.h"
#include "tensorflow/compiler/xla/service/hlo_instruction.h"
#include "tensorflow/compiler/xla/service/hlo_matchers.h"
#include "tensorflow/compiler/xla/service/hlo_opcode.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/test.h"
#include "tensorflow/compiler/xla/tests/hlo_test_base.h"
#include "tensorflow/compiler/xla/types.h"
#include "tensorflow/core/lib/core/status_test_util.h"

namespace op = xla::testing::opcode_matchers;

namespace xla {
namespace {

class TupleSimplifierTest : public HloTestBase {
 protected:
  void Run(HloModule* module, bool change_expected) {
    TupleSimplifier simplifier;
    auto changed_status = simplifier.Run(module);
    TF_ASSERT_OK(changed_status.status());
    EXPECT_EQ(change_expected, changed_status.ValueOrDie());
  }

  const Shape scalar_shape_ = ShapeUtil::MakeShape(F32, {});
  const Shape tuple_shape_ = ShapeUtil::MakeTupleShape(
      {ShapeUtil::MakeShape(F32, {}), ShapeUtil::MakeShape(F32, {}),
       ShapeUtil::MakeShape(F32, {})});
};

TEST_F(TupleSimplifierTest, TupleOfParameters) {
  // A Tuple constructed of a bunch of parameters should not be changed.
  HloComputation::Builder builder(TestName());
  HloInstruction* param0 = builder.AddInstruction(
      HloInstruction::CreateParameter(0, scalar_shape_, "param0"));
  HloInstruction* param1 = builder.AddInstruction(
      HloInstruction::CreateParameter(1, scalar_shape_, "param1"));
  HloInstruction* param2 = builder.AddInstruction(
      HloInstruction::CreateParameter(2, scalar_shape_, "param2"));
  builder.AddInstruction(HloInstruction::CreateTuple({param0, param1, param2}));
  auto module = CreateNewModule();
  module->AddEntryComputation(builder.Build());

  Run(module.get(), /*change_expected=*/false);
}

TEST_F(TupleSimplifierTest, GteOfTupleOfParameter) {
  // A GTE of a tuple parameter should not be changed.
  HloComputation::Builder builder(TestName());
  HloInstruction* param = builder.AddInstruction(
      HloInstruction::CreateParameter(0, tuple_shape_, "param"));
  builder.AddInstruction(
      HloInstruction::CreateGetTupleElement(scalar_shape_, param, 1));
  auto module = CreateNewModule();
  module->AddEntryComputation(builder.Build());

  Run(module.get(), /*change_expected=*/false);
}

TEST_F(TupleSimplifierTest, GteOfTuple) {
  // A GTE of a Tuple should be short-circuited.
  HloComputation::Builder builder(TestName());
  HloInstruction* param0 = builder.AddInstruction(
      HloInstruction::CreateParameter(0, scalar_shape_, "param0"));
  HloInstruction* param1 = builder.AddInstruction(
      HloInstruction::CreateParameter(1, scalar_shape_, "param1"));
  HloInstruction* param2 = builder.AddInstruction(
      HloInstruction::CreateParameter(2, scalar_shape_, "param2"));
  HloInstruction* tuple = builder.AddInstruction(
      HloInstruction::CreateTuple({param0, param1, param2}));
  HloInstruction* gte = builder.AddInstruction(
      HloInstruction::CreateGetTupleElement(scalar_shape_, tuple, 1));

  auto module = CreateNewModule();
  auto computation = module->AddEntryComputation(builder.Build());

  EXPECT_THAT(computation->root_instruction(), gte);

  Run(module.get(), /*change_expected=*/true);

  EXPECT_THAT(computation->root_instruction(), param1);
}

TEST_F(TupleSimplifierTest, GteOfTupleChain) {
  // Verify a chain of GTE/Tuple instructions is collapsed.
  HloComputation::Builder builder(TestName());
  HloInstruction* param = builder.AddInstruction(
      HloInstruction::CreateParameter(0, scalar_shape_, "param"));

  const int kChainLength = 10;
  HloInstruction* element = param;
  for (int i = 0; i < kChainLength; ++i) {
    HloInstruction* tuple = builder.AddInstruction(
        HloInstruction::CreateTuple({element, element, element}));
    element = builder.AddInstruction(
        HloInstruction::CreateGetTupleElement(scalar_shape_, tuple, 1));
  }
  builder.AddInstruction(
      HloInstruction::CreateUnary(scalar_shape_, HloOpcode::kNegate, element));

  auto module = CreateNewModule();
  auto computation = module->AddEntryComputation(builder.Build());

  EXPECT_THAT(computation->root_instruction(),
              op::Negate(op::GetTupleElement(op::Tuple())));

  Run(module.get(), /*change_expected=*/true);

  EXPECT_THAT(computation->root_instruction(), op::Negate(op::Parameter()));
}

TEST_F(TupleSimplifierTest, NestedGteOfTuples) {
  // Verify a nesting of GTE/Tuple instructions is collapsed. Tuples are nested
  // to some depth with a chain of Tuple instructions, then extracted with a
  // chain of GTE instructions.
  HloComputation::Builder builder(TestName());
  HloInstruction* param = builder.AddInstruction(
      HloInstruction::CreateParameter(0, scalar_shape_, "param"));

  const int kNestingDepth = 5;
  HloInstruction* nested_tuple = param;
  for (int i = 0; i < kNestingDepth; ++i) {
    nested_tuple = builder.AddInstruction(
        HloInstruction::CreateTuple({nested_tuple, nested_tuple}));
  }

  HloInstruction* element = nested_tuple;
  for (int i = 0; i < kNestingDepth; ++i) {
    element = builder.AddInstruction(HloInstruction::CreateGetTupleElement(
        ShapeUtil::GetTupleElementShape(element->shape(), 0), element, 0));
  }

  auto module = CreateNewModule();
  auto computation = module->AddEntryComputation(builder.Build());

  EXPECT_THAT(computation->root_instruction(), element);

  Run(module.get(), /*change_expected=*/true);

  EXPECT_THAT(computation->root_instruction(), param);
}

TEST_F(TupleSimplifierTest, TupleOfGteInstructions) {
  // Verify that a tuple constructed of GTE instructions operating on the same
  // tuple are collapsed.
  HloComputation::Builder builder(TestName());
  HloInstruction* tuple_param = builder.AddInstruction(
      HloInstruction::CreateParameter(0, tuple_shape_, "param"));
  HloInstruction* gte0 = builder.AddInstruction(
      HloInstruction::CreateGetTupleElement(scalar_shape_, tuple_param, 0));
  HloInstruction* gte1 = builder.AddInstruction(
      HloInstruction::CreateGetTupleElement(scalar_shape_, tuple_param, 1));
  HloInstruction* gte2 = builder.AddInstruction(
      HloInstruction::CreateGetTupleElement(scalar_shape_, tuple_param, 2));
  HloInstruction* tuple =
      builder.AddInstruction(HloInstruction::CreateTuple({gte0, gte1, gte2}));

  auto module = CreateNewModule();
  auto computation = module->AddEntryComputation(builder.Build());

  EXPECT_THAT(computation->root_instruction(), tuple);

  Run(module.get(), /*change_expected=*/true);

  EXPECT_THAT(computation->root_instruction(), tuple_param);
}

}  // namespace
}  // namespace xla
