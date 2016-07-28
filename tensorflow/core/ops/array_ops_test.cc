/* Copyright 2016 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");

You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/core/framework/graph.pb.h"
#include "tensorflow/core/framework/node_def_builder.h"
#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/shape_inference_testutil.h"
#include "tensorflow/core/framework/tensor_testutil.h"
#include "tensorflow/core/platform/test.h"

namespace tensorflow {

TEST(ArrayOpsTest, Pack_ShapeFn) {
  ShapeInferenceTestOp op("Pack");
  auto set_axis = [&op](int axis) {
    int n = 3;
    std::vector<NodeDefBuilder::NodeOut> src_list;
    for (int i = 0; i < n; ++i) src_list.emplace_back("a", 0, DT_FLOAT);
    TF_CHECK_OK(NodeDefBuilder("test", "Pack")
                    .Input(src_list)
                    .Attr("N", n)
                    .Attr("axis", axis)
                    .Finalize(&op.node_def));
  };

  set_axis(0);
  INFER_OK(op, "?;?;?", "?");

  for (int axis : {0, -3}) {
    set_axis(axis);
    INFER_OK(op, "?;?;?", "?");
    INFER_OK(op, "[1,3];[1,3];?", "[3,d0_0|d1_0,d0_1|d1_1]");
    INFER_OK(op, "[?,3];[1,3];?", "[3,d1_0,d0_1|d1_1]");
    INFER_OK(op, "[?,?];[1,3];?", "[3,d1_0,d1_1]");
  }
  for (int axis : {1, -2}) {
    set_axis(axis);
    INFER_OK(op, "?;?;?", "?");
    INFER_OK(op, "[1,3];[1,3];?", "[d0_0|d1_0,3,d0_1|d1_1]");
    INFER_OK(op, "[?,3];[1,3];?", "[d1_0,3,d0_1|d1_1]");
    INFER_OK(op, "[?,?];[1,3];?", "[d1_0,3,d1_1]");
  }
  for (int axis : {2, -1}) {
    set_axis(axis);
    INFER_OK(op, "?;?;?", "?");
    INFER_OK(op, "[1,3];[1,3];?", "[d0_0|d1_0,d0_1|d1_1,3]");
    INFER_OK(op, "[?,3];[1,3];?", "[d1_0,d0_1|d1_1,3]");
    INFER_OK(op, "[?,?];[1,3];?", "[d1_0,d1_1,3]");
  }

  set_axis(-4);
  INFER_ERROR("Invalid axis: -4; must be in [-3,3)", op, "[1,3];[1,3];?");
  set_axis(3);
  INFER_ERROR("Invalid axis: 3; must be in [-3,3)", op, "[1,3];[1,3];?");

  set_axis(0);
  INFER_ERROR(("Shapes must be equal rank, but are 3 and 2"
               "\n\tFrom merging shape 0 with other shapes."),
              op, "[1,2,3];?;[1,4]");
}

TEST(ArrayOpsTest, UnPack_ShapeFn) {
  ShapeInferenceTestOp op("Unpack");
  auto set_axis_and_num = [&op](int axis, int num) {
    TF_CHECK_OK(NodeDefBuilder("test", "Unpack")
                    .Input("a", 0, DT_FLOAT)
                    .Attr("axis", axis)
                    .Attr("num", num)
                    .Finalize(&op.node_def));
  };

  set_axis_and_num(0, 1);
  INFER_OK(op, "?", "?");

  for (int axis : {0, -3}) {
    set_axis_and_num(axis, 1);
    INFER_OK(op, "?", "?");
    INFER_OK(op, "[1,2,3]", "[d0_1,d0_2]");
    INFER_OK(op, "[?,?,?]", "[d0_1,d0_2]");
  }
  for (int axis : {1, -2}) {
    set_axis_and_num(axis, 2);
    INFER_OK(op, "[1,2,3]", "[d0_0,d0_2];[d0_0,d0_2]");
    INFER_OK(op, "[?,?,?]", "[d0_0,d0_2];[d0_0,d0_2]");
  }
  for (int axis : {2, -1}) {
    set_axis_and_num(axis, 3);
    INFER_OK(op, "[1,2,3]", "[d0_0,d0_1];[d0_0,d0_1];[d0_0,d0_1]");
    INFER_OK(op, "[?,?,?]", "[d0_0,d0_1];[d0_0,d0_1];[d0_0,d0_1]");
  }

  set_axis_and_num(2, 2);
  INFER_ERROR("Dimension must be 2 but is 3", op, "[1,2,3]");

  set_axis_and_num(-4, 3);
  INFER_ERROR("Invalid axis: -4; must be in [-3,3)", op, "[1,2,3]");
  set_axis_and_num(3, 3);
  INFER_ERROR("Invalid axis: 3; must be in [-3,3)", op, "[1,2,3]");
}

TEST(ArrayOpsTest, Const_ShapeFn) {
  ShapeInferenceTestOp op("Const");
  TensorProto tensor_proto;
  auto* shape_proto = tensor_proto.mutable_tensor_shape();
  auto rebuild_node_def = [&op, &tensor_proto]() {
    TF_CHECK_OK(NodeDefBuilder("test", "Const")
                    .Attr("value", tensor_proto)
                    .Finalize(&op.node_def));
  };

  TensorShape{}.AsProto(shape_proto);
  rebuild_node_def();
  INFER_OK(op, "", "[]");
  TensorShape{1, 2, 3, 4}.AsProto(shape_proto);
  rebuild_node_def();
  INFER_OK(op, "", "[1,2,3,4]");

  shape_proto->add_dim()->set_size(-1);
  rebuild_node_def();
  INFER_ERROR("Shape [1,2,3,4,-1] has negative dimensions", op, "");
}

TEST(ArrayOpsTest, UnchangedShapes_ShapeFn) {
  for (const char* op_name : {
           "CheckNumerics", "Identity", "QuantizeAndDequantize", "RefIdentity",
           "StopGradient", "ZerosLike",
       }) {
    ShapeInferenceTestOp op(op_name);
    INFER_OK(op, "?", "in0");
    INFER_OK(op, "[]", "in0");
    INFER_OK(op, "[1,2,?,4,5]", "in0");
  }

  // inputs 1 and 2 are ignored; input 0 is transferred to output 0.
  ShapeInferenceTestOp op("BatchMatrixBandPart");
  INFER_OK(op, "?;?;?", "in0");
  INFER_OK(op, "[];?;?", "in0");
  INFER_OK(op, "[1,2,?,4,5];?;?", "in0");
}

TEST(ArrayOpsTest, Diag_ShapeFn) {
  ShapeInferenceTestOp op("Diag");
  INFER_OK(op, "?", "?");
  INFER_OK(op, "[]", "[]");
  INFER_OK(op, "[1,?,3]", "[d0_0,d0_1,d0_2,d0_0,d0_1,d0_2]");
  INFER_ERROR("Shape must be at most rank 3 but is rank 4", op, "[?,1,2,3]");
}

TEST(ArrayOpsTest, DiagPart_ShapeFn) {
  ShapeInferenceTestOp op("DiagPart");
  INFER_OK(op, "?", "?");
  INFER_OK(op, "[]", "[]");
  INFER_OK(op, "[1,?,?,4]", "[d0_0,d0_3]");
  INFER_OK(op, "[1,?,3,?,4,3]", "[d0_0,d0_4,d0_2|d0_5]");
  INFER_ERROR("Input must have even rank <= 6, input rank is 1", op, "[?]");
  INFER_ERROR("Input must have even rank <= 6, input rank is 3", op, "[1,2,3]");
  INFER_ERROR("Input must have even rank <= 6, input rank is 8", op,
              "[1,2,3,?,?,?,?,?]");
  INFER_ERROR("Dimensions must be equal, but are 2 and 10", op, "[1,2,?,10]");
}

TEST(ArrayOpsTest, BatchMatrixDiag_ShapeFn) {
  ShapeInferenceTestOp op("BatchMatrixDiag");
  INFER_OK(op, "?", "?");
  INFER_ERROR("Shape must be at least rank 1 but is rank 0", op, "[]");
  INFER_OK(op, "[?]", "[d0_0,d0_0]");
  INFER_OK(op, "[1,?,?,4]", "[d0_0,d0_1,d0_2,d0_3,d0_3]");
}

TEST(ArrayOpsTest, BatchMatrixDiagPart_ShapeFn) {
  ShapeInferenceTestOp op("BatchMatrixDiagPart");
  INFER_OK(op, "?", "?");
  INFER_ERROR("Shape must be at least rank 2 but is rank 1", op, "[?]");
  INFER_OK(op, "[?,1,2,2]", "[d0_0,d0_1,d0_2|d0_3]");
  INFER_ERROR("Dimensions must be equal, but are 3 and 2", op, "[1,2,3]");
}

TEST(ArrayOpsTest, Reverse_ShapeFn) {
  ShapeInferenceTestOp op("Reverse");
  INFER_OK(op, "?;?", "in0");
  INFER_ERROR("Shape must be rank 1 but is rank 0", op, "?;[]");
  INFER_ERROR("Shape must be rank 1 but is rank 2", op, "?;[?,2]");
  INFER_ERROR("Shape must be rank 4 but is rank 3", op, "[1,2,3];[4]");
  INFER_ERROR("reverse does not work on tensors with more than 8 dimensions",
              op, "[1,2,3,4,5,6,7,8,9];[9]");
  INFER_OK(op, "[1,2,3,?];[4]", "in0");
  INFER_OK(op, "[1,2,3,?,5,6,7,8];[8]", "in0");
}

TEST(ArrayOpsTest, Fill_ShapeFn) {
  ShapeInferenceTestOp op("Fill");
  op.input_tensors.resize(2);
  INFER_OK(op, "?;?", "?");
  INFER_OK(op, "[?];?", "?");
  INFER_OK(op, "[4];?", "[?,?,?,?]");

  Tensor in_t = test::AsTensor<int32>({1, 2, 3, 4});
  op.input_tensors[0] = &in_t;
  INFER_OK(op, "[4];?", "[1,2,3,4]");
}

TEST(ArrayOpsTest, Gather_ShapeFn) {
  ShapeInferenceTestOp op("Gather");
  INFER_OK(op, "?;?", "?");
  INFER_OK(op, "[1,?,2];[3]", "[d1_0,d0_1,d0_2]");
  INFER_ERROR("Shape must be at least rank 1 but is rank 0", op, "[];[1,2,3]");
}

TEST(ArrayOpsTest, GatherNd_ShapeFn) {
  ShapeInferenceTestOp op("GatherNd");

  // Inputs are (params, indices).
  INFER_OK(op, "?;?", "?");
  INFER_OK(op, "[1,?,3,?];[?,0]", "[d1_0,d0_0,d0_1,d0_2,d0_3]");
  INFER_OK(op, "[1,?,3,?];[?,4]", "[d1_0]");

  // params.rank >= indices.dim(-1).
  INFER_ERROR("indices.shape[-1] must be <= params.rank", op, "[1,2,3];[4]");
}

TEST(ArrayOpsTest, Shape_ShapeFn) {
  ShapeInferenceTestOp op("Shape");
  INFER_OK(op, "?", "[?]");
  INFER_OK(op, "[?]", "[1]");
  INFER_OK(op, "[?,2,3,4,5]", "[5]");
}

TEST(ArrayOpsTest, Unique_ShapeFn) {
  ShapeInferenceTestOp op("Unique");
  INFER_OK(op, "?", "[?];in0");
  INFER_OK(op, "[1,2,3,?,5]", "[?];in0");
}

TEST(ArrayOpsTest, UniqueWithCounts_ShapeFn) {
  ShapeInferenceTestOp op("UniqueWithCounts");
  INFER_OK(op, "?", "[?];in0;[?]");
  INFER_OK(op, "[1,2,3,?,5]", "[?];in0;[?]");
}

TEST(ArrayOpsTest, InvertPermutation_ShapeFn) {
  ShapeInferenceTestOp op("InvertPermutation");
  INFER_OK(op, "?", "[?]");
  INFER_OK(op, "[1]", "in0");
  INFER_ERROR("Shape must be rank 1 but is rank 0", op, "[]");
}

TEST(ArrayOpsTest, PadD_ShapeFn) {
  for (const char* op_name : {"Pad", "MirrorPad"}) {
    ShapeInferenceTestOp op(op_name);
    op.input_tensors.resize(2);

    // Inputs are input and paddings.

    INFER_OK(op, "?;?", "?");

    // Check shape of paddings.
    INFER_ERROR("Shape must be rank 2 but is rank 3", op, "?;[1,2,3]");
    INFER_ERROR("Dimension must be 2 but is 4", op, "?;[1,4]");

    // input.rank and paddings.dim(0) are equal. This is the number of dims in
    // output.
    INFER_ERROR("Shape must be rank 4 but is rank 3", op, "[1,2,3];[4,2]");
    INFER_OK(op, "[1,2,3];?", "[?,?,?]");
    INFER_OK(op, "?;[3,2]", "[?,?,?]");

    // Make the paddings tensor known and verify padding values get added.
    // E.g., if padding is ((1,10),(2,20),(3,30)) then values 11,22,23 are added
    // to input dims to get output.
    Tensor paddings_t(DT_INT32, TensorShape{3, 2});
    test::FillValues<int32>(&paddings_t, {1, 10, 2, 20, 3, 30});
    op.input_tensors[1] = &paddings_t;
    INFER_OK(op, "[100,200,300];[3,2]", "[111,222,333]");
    INFER_OK(op, "[100,?,300];[3,2]", "[111,?,333]");
    INFER_OK(op, "?;[3,2]", "[?,?,?]");
  }
}

TEST(ArrayOpsTest, BroadcastGradientArgs_ShapeFn) {
  ShapeInferenceTestOp op("BroadcastGradientArgs");
  // Output is always two unknown vectors.
  INFER_OK(op, "?;?", "[?];[?]");
  INFER_OK(op, "[123];[456]", "[?];[?]");

  // Rank checks
  INFER_ERROR("Shape must be rank 1 but is rank 0", op, "[];?");
  INFER_ERROR("Shape must be rank 1 but is rank 0", op, "?;[]");
}

TEST(ArrayOpsTest, ListDiff_ShapeFn) {
  ShapeInferenceTestOp op("BroadcastGradientArgs");
  // Output is always two matching unknown vectors.
  INFER_OK(op, "?;?", "[?];[?]");
  INFER_OK(op, "[123];[456]", "[?];[?]");

  // Rank checks
  INFER_ERROR("Shape must be rank 1 but is rank 0", op, "[];?");
  INFER_ERROR("Shape must be rank 1 but is rank 0", op, "?;[]");
}

TEST(ArrayOpsTest, BatchMatrixSetDiag_ShapeFn) {
  ShapeInferenceTestOp op("BatchMatrixSetDiag");

  // Inputs are input and diagonal.

  // Rank checks.
  INFER_ERROR("Shape must be at least rank 2 but is rank 1", op, "[1];?");
  INFER_ERROR("Shape must be at least rank 1 but is rank 0", op, "?;[]");

  // Output matches input, and also matches diagonal + diagonal.dim(-1).
  INFER_OK(op, "?;?", "?");
  INFER_OK(op, "?;[1,2]", "[d1_0,d1_1,d1_1]");
  INFER_OK(op, "[1,2,2];?", "in0");
  INFER_OK(op, "[1,?,2];[?,?]", "in0");
  INFER_OK(op, "[1,?,?];[?,2]", "[d0_0,d1_1,d1_1]");

  // Last 2 dims of input must match.
  INFER_ERROR("Dimensions must be equal, but are 2 and 3", op, "[1,2,3];?");

  // Dims matches prefix of input.
  INFER_ERROR("Dimensions must be equal, but are 1 and 2", op, "[1,?];[2]");
}

TEST(ArrayOpsTest, ExpandDims_ShapeFn) {
  ShapeInferenceTestOp op("ExpandDims");
  op.input_tensors.resize(2);

  // With unknown dim tensor value, output is unknown.
  INFER_OK(op, "?;?", "?");
  INFER_ERROR("Shape must be rank 0 but is rank 1", op, "?;[1]");
  Tensor dim_t;
  op.input_tensors[1] = &dim_t;

  // Expand at front of tensor.
  dim_t = test::AsScalar<int32>(0);
  INFER_OK(op, "?;?", "?");
  INFER_OK(op, "[5,?,7];?", "[1,d0_0,d0_1,d0_2]");

  // Expand at middle of tensor.
  for (int32 idx : {1, -3}) {
    dim_t = test::AsScalar<int32>(idx);
    INFER_OK(op, "?;?", "?");
    INFER_OK(op, "[5,?,7];?", "[d0_0,1,d0_1,d0_2]");
  }
  for (int32 idx : {2, -2}) {
    dim_t = test::AsScalar<int32>(idx);
    INFER_OK(op, "?;?", "?");
    INFER_OK(op, "[5,?,7];?", "[d0_0,d0_1,1,d0_2]");
  }

  for (int32 idx : {3, -1}) {
    // Expand at the end.
    dim_t = test::AsScalar<int32>(idx);
    INFER_OK(op, "?;?", "?");
    INFER_OK(op, "[5,?,7];?", "[d0_0,d0_1,d0_2,1]");
  }
  // Examples from ExpandDims doc.
  dim_t = test::AsScalar<int32>(0);
  INFER_OK(op, "[2];[]", "[1,d0_0]");
  dim_t = test::AsScalar<int32>(1);
  INFER_OK(op, "[2];[]", "[d0_0,1]");
  dim_t = test::AsScalar<int32>(-1);
  INFER_OK(op, "[2];[]", "[d0_0,1]");
}

TEST(ArrayOpsTest, ImmutableConst_ShapeFn) {
  ShapeInferenceTestOp op("ImmutableConst");

  TF_CHECK_OK(NodeDefBuilder("test", "ImmutableConst")
                  .Attr("dtype", DT_FLOAT)
                  .Attr("shape", TensorShape({1, 2, 3}))
                  .Attr("memory_region_name", "test_region")
                  .Finalize(&op.node_def));
  INFER_OK(op, "", "[1,2,3]");

  TF_CHECK_OK(NodeDefBuilder("test", "ImmutableConst")
                  .Attr("dtype", DT_FLOAT)
                  .Attr("shape", TensorShape({}))
                  .Attr("memory_region_name", "test_region")
                  .Finalize(&op.node_def));
  INFER_OK(op, "", "[]");

  TF_CHECK_OK(NodeDefBuilder("test", "ImmutableConst")
                  .Attr("dtype", DT_FLOAT)
                  .Attr("shape", "invalid")
                  .Attr("memory_region_name", "test_region")
                  .Finalize(&op.node_def));
  INFER_ERROR("AttrValue had value with type 'string' when 'shape' expected",
              op, "");
}

TEST(ArrayOpsTest, Concat_ShapeFn) {
  ShapeInferenceTestOp op("Concat");
  auto set_n = [&op](int n) {
    std::vector<NodeDefBuilder::NodeOut> src_list;
    for (int i = 0; i < n; ++i) src_list.emplace_back("a", 0, DT_FLOAT);
    TF_CHECK_OK(NodeDefBuilder("test", "Concat")
                    .Input({"concat_dim", 0, DT_INT32})
                    .Input(src_list)
                    .Attr("n", n)
                    .Finalize(&op.node_def));
  };

  // Confirm dimension[0] of the input (the concat_dim) is a scalar.
  set_n(2);
  INFER_ERROR("Shape must be rank 0 but is rank 1", op, "[1];?;?");

  // Test with the input concat_dim tensor not known. This takes the known rank
  // of the inputs and makes a tensor of that many unknown dims.
  set_n(7);
  INFER_OK(op, "?;?;?;?;[1,2,3];?;[3,2,1];?", "[?,?,?]");
  set_n(4);
  INFER_OK(op, "?;?;?;[1,2,3,4];[4,3,2,1]", "[?,?,?,?]");
  INFER_OK(op, "?;?;?;?;?", "?");  // output rank unknown
  INFER_ERROR("Can't concatenate scalars (use tf.pack instead)", op,
              "?;?;?;[];[]");
  INFER_ERROR("Shape must be rank 2 but is rank 3", op, "?;?;?;[1,2];[1,2,3]");

  // Test when the concat_dim tensor is known. The concatenated dimension is
  // summed across all input tensors, and other dimensions are merged.
  Tensor concat_dim_t;
  op.input_tensors.push_back(&concat_dim_t);
  set_n(2);

  // Invalid concat dim value.
  concat_dim_t = test::AsScalar(-1);
  INFER_ERROR("Expected concat_dim >= 0, but got -1", op, "?;?;?");

  // Sum dim 0, merge the other two dims.
  concat_dim_t = test::AsScalar(0);
  INFER_OK(op, "[];[100,2,?];[10,?,3]", "[110,d1_1,d2_2]");
  INFER_ERROR("Dimension 1 in both shapes must be equal, but are 5 and 3", op,
              "[];[100,2,5];[10,?,3]");
  // concat_dim can't be summed, as one value is unknown.
  INFER_OK(op, "[];[100,2,?];[?,?,3]", "[?,d1_1,d2_2]");
  INFER_OK(op, "[];[?,2,?];[10,?,3]", "[?,d1_1,d2_2]");

  // Test with a higher concat_dim.
  concat_dim_t = test::AsScalar(1);
  INFER_OK(op, "[];[1,100,?];[?,10,3]", "[d1_0,110,d2_2]");
  INFER_OK(op, "[];[1,100];[?,10]", "[d1_0,110]");
  INFER_OK(op, "[];[?,100];[1,10]", "[d2_0,110]");
  // concat_dim is too high.
  INFER_ERROR("Shape must be at least rank 2 but is rank 1", op,
              "[];[100];[10,?]");
  INFER_ERROR("Shape must be at least rank 2 but is rank 1", op,
              "[];[100,5];[10]");

  // Repeat successful case with several unknown inputs.
  set_n(5);
  INFER_OK(op, "[];?;[1,100,?];[?,?,?];[?,10,3];?", "[d2_0,?,d4_2]");
}

TEST(ArrayOpsTest, ConcatOffset_ShapeFn) {
  ShapeInferenceTestOp op("ConcatOffset");

  const int n = 4;
  std::vector<NodeDefBuilder::NodeOut> src_list;
  for (int i = 0; i < n; ++i) src_list.emplace_back("a", 0, DT_INT32);
  TF_CHECK_OK(NodeDefBuilder("test", "ConcatOffset")
                  .Input({"concat_dim", 0, DT_INT32})
                  .Input(src_list)
                  .Attr("n", n)
                  .Finalize(&op.node_def));
  INFER_OK(op, "?;?;?;?;?", "in1;in2;in3;in4");
}

TEST(ArrayOpsTest, Reshape_ShapeFn) {
  ShapeInferenceTestOp op("Reshape");
  op.input_tensors.resize(2);

  // No valid shape provided.
  INFER_OK(op, "?;?", "?");
  INFER_OK(op, "[?];?", "?");
  INFER_OK(op, "[?];[?]", "?");
  INFER_OK(op, "[4];[?]", "?");

  // All dimensions provided.
  Tensor new_shape = test::AsTensor<int32>({1, 2, 3});
  op.input_tensors[1] = &new_shape;
  INFER_OK(op, "[?];[3]", "[1,2,3]");
  INFER_OK(op, "[6];[3]", "[1,2,3]");
  // The number of elements should match for the reshape to succeed.
  INFER_ERROR(
      "Cannot reshape a tensor with 12 elements to shape [1,2,3] (6 elements)",
      op, "[3,4];[3]");

  // Unknown dimensions.
  // Flatten:
  new_shape = test::AsTensor<int32>({-1});
  INFER_OK(op, "[?];[1]", "[?]");
  INFER_OK(op, "[2,2];[1]", "[4]");
  // The first dimension is inferred:
  new_shape = test::AsTensor<int32>({2, -1});
  INFER_OK(op, "[3,4];[2]", "[2,6]");
  // The total number of elements must be divisible by the known dimensions.
  INFER_ERROR("Dimension size must be divisible by 2 but is 7", op, "[7];[2]");
  // Multiple missing dimensions cannot be inferred.
  new_shape = test::AsTensor<int32>({-1, -1, 2});
  INFER_ERROR("Cannot infer multiple unknown dimensions in shape [?,?,2]", op,
              "[8];[3]");

  // Reshaping to a scalar.
  new_shape = test::AsTensor<int32>({});
  INFER_OK(op, "[1];[0]", "[]");
  INFER_ERROR(
      "Cannot reshape a tensor with 2 elements to shape [] (1 elements)", op,
      "[1,2];[0]");
}

TEST(ArrayOpsTest, Placeholder_ShapeFn) {
  {
    // 2D shape
    ShapeInferenceTestOp op("Placeholder");
    TensorShape shape({1, 2});
    TF_CHECK_OK(NodeDefBuilder("test", "Placeholder")
                    .Attr("shape", shape)
                    .Attr("dtype", DT_FLOAT)
                    .Finalize(&op.node_def));
    INFER_OK(op, "", "[1,2]");
  }

  {
    // Scalar shapes are unknown shapes due to legacy.
    ShapeInferenceTestOp op("Placeholder");
    TensorShape shape({});
    TF_CHECK_OK(NodeDefBuilder("test", "Placeholder")
                    .Attr("shape", shape)
                    .Attr("dtype", DT_FLOAT)
                    .Finalize(&op.node_def));
    INFER_OK(op, "", "?");
  }

  {
    // Partial shape
    ShapeInferenceTestOp op("Placeholder");
    const int64 dims[2] = {1, -1};
    PartialTensorShape shape;
    TF_CHECK_OK(PartialTensorShape::MakePartialShape(dims, 2, &shape));
    TF_CHECK_OK(NodeDefBuilder("test", "Placeholder")
                    .Attr("shape", shape)
                    .Attr("dtype", DT_FLOAT)
                    .Finalize(&op.node_def));
    INFER_OK(op, "", "[1,?]");
  }

  {
    ShapeInferenceTestOp op("PlaceholderWithDefault");
    const int64 dims[2] = {1, -1};
    PartialTensorShape shape;
    TF_CHECK_OK(PartialTensorShape::MakePartialShape(dims, 2, &shape));
    TF_CHECK_OK(NodeDefBuilder("test", "PlaceholderWithDefault")
                    .Input("input", 0, DT_FLOAT)
                    .Attr("shape", shape)
                    .Attr("dtype", DT_FLOAT)
                    .Finalize(&op.node_def));
    INFER_OK(op, "[1,2]", "[1,?]");

    // input shape is not compatible with output shape.
    INFER_ERROR("Dimension 0 in both shapes must be equal, but are 2 and 1", op,
                "[2,3]");
    // Wrong rank
    INFER_ERROR("Shapes must be equal rank, but are 3 and 2", op, "[1,3,10]");
  }
}

TEST(ArrayOpsTest, Transpose_ShapeFn) {
  ShapeInferenceTestOp op("Transpose");
  op.input_tensors.resize(2);

  // Missing shape information.
  INFER_OK(op, "?;?", "?");
  INFER_OK(op, "?;[?]", "?");
  INFER_OK(op, "?;[2]", "[?,?]");
  INFER_OK(op, "[?];?", "[?]");
  INFER_OK(op, "[?,?];[2]", "[?,?]");
  INFER_ERROR("Dimension must be 3 but is 2", op, "[1,2,3];[2]");
  Tensor perm = test::AsTensor<int32>({0});
  op.input_tensors[1] = &perm;
  INFER_OK(op, "[?];[?]", "[d0_0]");
  perm = test::AsTensor<int32>({1, 0});
  INFER_OK(op, "?;[2]", "[?,?]");
  INFER_OK(op, "[?,?];[2]", "[d0_1,d0_0]");
  INFER_OK(op, "[1,?];[2]", "[d0_1,d0_0]");

  // Invalid arguments.
  perm = test::AsTensor<int32>({1, 2});
  INFER_ERROR("perm dim 2 is out of range of input rank 2", op, "[1,2];[2]");
  perm = test::AsTensor<int32>({0});
  INFER_ERROR("Dimension must be 2 but is 1", op, "[1,2];[1]");

  // Larger valid cases.
  perm = test::AsTensor<int32>({1, 0, 3, 4, 2});
  INFER_OK(op, "[0,1,2,3,4];[5]", "[d0_1,d0_0,d0_3,d0_4,d0_2]");
  INFER_OK(op, "[0,?,2,3,4];[5]", "[d0_1,d0_0,d0_3,d0_4,d0_2]");
}

}  // end namespace tensorflow
