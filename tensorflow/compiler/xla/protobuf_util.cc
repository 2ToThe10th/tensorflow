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

#include "tensorflow/compiler/xla/protobuf_util.h"

#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/compiler/xla/types.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/core/lib/io/path.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/platform/protobuf.h"

namespace xla {
namespace protobuf_util {

bool ProtobufEquals(const tensorflow::protobuf::Message& m1,
                    const tensorflow::protobuf::Message& m2) {
  // This is a bit fast and loose, but avoids introducing a dependency on
  // the much more complex protobuf::util::MessageDifferencer class.  For
  // our purposes we just say that two protobufs are equal if their serialized
  // representations are equal.
  string serialized1, serialized2;
  m1.AppendToString(&serialized1);
  m2.AppendToString(&serialized2);
  return (serialized1 == serialized2);
}

StatusOr<string> ToJson(const tensorflow::protobuf::Message& message) {
  string json_output;
  tensorflow::protobuf::util::JsonPrintOptions json_options;
  json_options.add_whitespace = true;
  json_options.always_print_primitive_fields = true;
  auto status = tensorflow::protobuf::util::MessageToJsonString(
      message, &json_output, json_options);
  if (!status.ok()) {
    return InternalError("MessageToJsonString failed: %s",
                         status.error_message().data());
  }
  return json_output;
}

Status DumpJsonToDirectory(const tensorflow::protobuf::Message& message,
                           const string& directory, const string& file_name) {
  TF_ASSIGN_OR_RETURN(const string json_output, ToJson(message));

  tensorflow::Env* env = tensorflow::Env::Default();
  TF_RETURN_IF_ERROR(env->RecursivelyCreateDir(directory));
  string safe_file_name = file_name + ".json";
  for (char& c : safe_file_name) {
    if (c == '/' || c == '\\') {
      c = '_';
    }
  }
  const string path = tensorflow::io::JoinPath(directory, safe_file_name);
  return tensorflow::WriteStringToFile(env, path, json_output);
}

}  // namespace protobuf_util
}  // namespace xla
