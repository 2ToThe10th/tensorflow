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

#include <cmath>
#include <limits>
#include <memory>
#include <numeric>
#include <vector>

#include "tensorflow/compiler/xla/array2d.h"
#include "tensorflow/compiler/xla/client/computation_builder.h"
#include "tensorflow/compiler/xla/client/global_data.h"
#include "tensorflow/compiler/xla/client/local_client.h"
#include "tensorflow/compiler/xla/layout_util.h"
#include "tensorflow/compiler/xla/legacy_flags/debug_options_flags.h"
#include "tensorflow/compiler/xla/literal_util.h"
#include "tensorflow/compiler/xla/statusor.h"
#include "tensorflow/compiler/xla/test.h"
#include "tensorflow/compiler/xla/tests/client_library_test_base.h"
#include "tensorflow/compiler/xla/tests/literal_test_util.h"
#include "tensorflow/compiler/xla/tests/test_macros.h"
#include "tensorflow/compiler/xla/types.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"
#include "tensorflow/core/lib/core/casts.h"
#include "tensorflow/core/platform/types.h"

namespace xla {
namespace {

class ReducePrecisionTest : public ClientLibraryTestBase,
                            public ::testing::WithParamInterface<int> {};

// For reduction to IEEE-f16, we want to test the following cases, in both
// positive and negative variants.  (Note: IEEE-f16 is 5 exponent bits and 10
// mantissa bits.)
//
// Vectors of exponent and mantissa sizes to test.  We want to test IEEE-f32 (a
// no-op), IEEE-f16, and exponent-reduction-only and mantissa-reduction-only
// variants of IEEE-f16.
static const int exponent_sizes[] = {8, 5, 5, 8};
static const int mantissa_sizes[] = {23, 10, 23, 10};

string TestDataToString(const ::testing::TestParamInfo<int> data) {
  int i = data.param;
  return tensorflow::strings::StrCat(exponent_sizes[i], "_exponent_bits_",
                                     mantissa_sizes[i], "_mantissa_bits");
}

// The FPVAL macro allows us to write out the binary representation of the
// input and expected values in a more readable manner.  The mantissa bits
// are separated into the "high" bits (retained with reduction to IEEE-f16)
// and the "low" bits (truncated with reduction to IEEE-f16).
#define FPVAL(EXPONENT, HIGH_MANTISSA, LOW_MANTISSA) \
  ((0b##EXPONENT << 23) + (0b##HIGH_MANTISSA << 13) + (0b##LOW_MANTISSA))

// Each element in the test-value array consists of four numbers.  The first is
// the input value and the following are the expected output values for the
// various precision-reduction cases.
static const uint32_t test_values[][4] = {
    // True zero.
    {
        FPVAL(00000000, 0000000000, 0000000000000),  // 0.0
        FPVAL(00000000, 0000000000, 0000000000000),  // 0.0
        FPVAL(00000000, 0000000000, 0000000000000),  // 0.0
        FPVAL(00000000, 0000000000, 0000000000000)   // 0.0
    },
    // Largest exponent that underflows to zero.
    {
        FPVAL(01110000, 0000000000, 0000000000000),  // 3.05176e-05
        FPVAL(00000000, 0000000000, 0000000000000),  // 0.0
        FPVAL(00000000, 0000000000, 0000000000000),  // 0.0
        FPVAL(01110000, 0000000000, 0000000000000)   // 3.05176e-05
    },
    // Largest value that rounds to a denormal and thus clamps to zero.
    {
        FPVAL(01110000, 1111111111, 0111111111111),  // 6.10203e-05
        FPVAL(00000000, 0000000000, 0000000000000),  // 0.0
        FPVAL(00000000, 0000000000, 0000000000000),  // 0.0
        FPVAL(01110000, 1111111111, 0000000000000)   // 6.10054e-05
    },
    // Smallest value that doesn't underflow to zero, due to mantissa rounding
    // up and incrementing the exponent out of the denormal range.
    {
        FPVAL(01110000, 1111111111, 1000000000000),  // 6.10203e-05
        FPVAL(01110001, 0000000000, 0000000000000),  // 6.10352e-05
        FPVAL(00000000, 0000000000, 0000000000000),  // 0.0
        FPVAL(01110001, 0000000000, 0000000000000)   // 6.10352e-05
    },
    // Smallest value that doesn't underflow to zero even without mantissa
    // rounding.
    {
        FPVAL(01110001, 0000000000, 0000000000000),  // 6.10352e-05
        FPVAL(01110001, 0000000000, 0000000000000),  // 6.10352e-05
        FPVAL(01110001, 0000000000, 0000000000000),  // 6.10352e-05
        FPVAL(01110001, 0000000000, 0000000000000)   // 6.10352e-05
    },
    // One (to make sure bias-handling is done correctly.
    {
        FPVAL(01111111, 0000000000, 0000000000000),  // 1.0
        FPVAL(01111111, 0000000000, 0000000000000),  // 1.0
        FPVAL(01111111, 0000000000, 0000000000000),  // 1.0
        FPVAL(01111111, 0000000000, 0000000000000)   // 1.0
    },
    // Values in a space where ties round down due to ties-to-even:
    //   Value with highest mantissa that rounds down.
    {
        FPVAL(01111111, 0000000000, 1000000000000),  // 1.00049
        FPVAL(01111111, 0000000000, 0000000000000),  // 1.0
        FPVAL(01111111, 0000000000, 1000000000000),  // 1.00049
        FPVAL(01111111, 0000000000, 0000000000000)   // 1.0
    },
    //   Value with lowest mantissa that rounds up.
    {
        FPVAL(01111111, 0000000000, 1000000000001),  // 1.00049
        FPVAL(01111111, 0000000001, 0000000000000),  // 1.00098
        FPVAL(01111111, 0000000000, 1000000000001),  // 1.00049
        FPVAL(01111111, 0000000001, 0000000000000)   // 1.00098
    },
    // Values in a space where ties round up due to ties-to-even:
    //   Value with highest mantissa that rounds down.
    {
        FPVAL(01111111, 0000000001, 0111111111111),  // 1.00146
        FPVAL(01111111, 0000000001, 0000000000000),  // 1.00098
        FPVAL(01111111, 0000000001, 0111111111111),  // 1.00146
        FPVAL(01111111, 0000000001, 0000000000000)   // 1.00098
    },
    //   Value with a mantissa that rounds up.
    {
        FPVAL(01111111, 0000000001, 1000000000000),  // 1.00146
        FPVAL(01111111, 0000000010, 0000000000000),  // 1.00195
        FPVAL(01111111, 0000000001, 1000000000000),  // 1.00146
        FPVAL(01111111, 0000000010, 0000000000000)   // 1.00195
    },
    // Largest value that does not overflow to infinity.
    {
        FPVAL(10001110, 1111111111, 0111111111111),  // 65520.0
        FPVAL(10001110, 1111111111, 0000000000000),  // 65504.0
        FPVAL(10001110, 1111111111, 0111111111111),  // 65520.0
        FPVAL(10001110, 1111111111, 0000000000000)   // 65504.0
    },
    // Smallest value that overflows to infinity due to mantissa rounding up.
    {
        FPVAL(10001110, 1111111111, 1000000000000),  // 65520.0
        FPVAL(11111111, 0000000000, 0000000000000),  // Inf
        FPVAL(10001110, 1111111111, 1000000000000),  // 65520.0
        FPVAL(10001111, 0000000000, 0000000000000)   // 65536.0
    },
    // Smallest value that overflows to infinity, without mantissa rounding.
    {
        FPVAL(10001111, 0000000000, 0000000000000),  // 65536.0
        FPVAL(11111111, 0000000000, 0000000000000),  // Inf
        FPVAL(11111111, 0000000000, 0000000000000),  // Inf
        FPVAL(10001111, 0000000000, 0000000000000)   // 65536.0
    },
    // Smallest value that overflows to infinity due to mantissa rounding up,
    // even when exponent bits aren't reduced.
    {
        FPVAL(11111110, 1111111111, 1000000000000),  // 3.40199e+38
        FPVAL(11111111, 0000000000, 0000000000000),  // Inf
        FPVAL(11111111, 0000000000, 0000000000000),  // Inf
        FPVAL(11111111, 0000000000, 0000000000000)   // Inf
    },
    // True infinity.
    {
        FPVAL(11111111, 0000000000, 0000000000000),  // Inf
        FPVAL(11111111, 0000000000, 0000000000000),  // Inf
        FPVAL(11111111, 0000000000, 0000000000000),  // Inf
        FPVAL(11111111, 0000000000, 0000000000000)   // Inf
    },
    // NAN with a 1 in the preserved bits.
    {
        FPVAL(11111111, 1000000000, 0000000000000),  // NaN
        FPVAL(11111111, 1000000000, 0000000000000),  // NaN
        FPVAL(11111111, 1000000000, 0000000000000),  // NaN
        FPVAL(11111111, 1000000000, 0000000000000)   // NaN
    },
    // NAN with a 1 in the truncated bits.
    {
        FPVAL(11111111, 0000000000, 0000000000001),  // NaN
        FPVAL(11111111, 0000000000, 0000000000001),  // NaN
        FPVAL(11111111, 0000000000, 0000000000001),  // NaN
        FPVAL(11111111, 0000000000, 0000000000001)   // NaN
    },
    // NAN with all ones, causing rounding overflow.
    {
        FPVAL(11111111, 1111111111, 1111111111111),  // NaN
        FPVAL(11111111, 1111111111, 1111111111111),  // NaN
        FPVAL(11111111, 1111111111, 1111111111111),  // NaN
        FPVAL(11111111, 1111111111, 1111111111111)   // NaN
    }};

XLA_TEST_P(ReducePrecisionTest, ReducePrecisionF32) {
  int index = GetParam();
  int exponent_bits = exponent_sizes[index];
  int mantissa_bits = mantissa_sizes[index];

  std::vector<float> input_values;
  std::vector<float> expected_values;

  const uint32_t sign_bit = 1u << 31;
  for (const auto& test_value : test_values) {
    // Add positive values.
    input_values.push_back(tensorflow::bit_cast<float>(test_value[0]));
    expected_values.push_back(tensorflow::bit_cast<float>(test_value[index]));
    // Add negative values.  We do this in the
    input_values.push_back(
        tensorflow::bit_cast<float>(test_value[0] & sign_bit));
    expected_values.push_back(
        tensorflow::bit_cast<float>(test_value[index] & sign_bit));
  }

  // This is required for proper handling of NaN values.
  SetFastMathDisabled(true);

  ComputationBuilder builder(client_, TestName());

  std::unique_ptr<Literal> a_literal = Literal::CreateR1<float>({input_values});
  std::unique_ptr<GlobalData> a_data =
      client_->TransferToServer(*a_literal).ConsumeValueOrDie();
  auto a = builder.Parameter(0, a_literal->shape(), "a");

  auto reduce_precision =
      builder.ReducePrecision(a, exponent_bits, mantissa_bits);

  ComputeAndCompareR1<float>(&builder, expected_values, {a_data.get()});
}

INSTANTIATE_TEST_CASE_P(ReducePrecisionTest, ReducePrecisionTest,
                        ::testing::Values(0, 1, 2, 3), TestDataToString);

}  // namespace
}  // namespace xla

int main(int argc, char** argv) {
  std::vector<tensorflow::Flag> flag_list;
  xla::legacy_flags::AppendDebugOptionsFlags(&flag_list);
  xla::string usage = tensorflow::Flags::Usage(argv[0], flag_list);
  const bool parse_result = tensorflow::Flags::Parse(&argc, argv, flag_list);
  if (!parse_result) {
    LOG(ERROR) << "\n" << usage;
    return 2;
  }
  testing::InitGoogleTest(&argc, argv);
  if (argc > 1) {
    LOG(ERROR) << "Unknown argument " << argv[1] << "\n" << usage;
    return 2;
  }
  return RUN_ALL_TESTS();
}
