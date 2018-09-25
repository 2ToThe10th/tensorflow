/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

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
#include "tensorflow/core/kernels/string_util.h"

#include "tensorflow/core/lib/core/errors.h"

namespace {
inline bool IsTrailByte(char x) { return static_cast<signed char>(x) < -0x40; }
}  // namespace

namespace tensorflow {

// Sets unit value based on str.
Status ParseUnicodeEncoding(const string& str, UnicodeEncoding* encoding) {
  if (str == "UTF8") {
    *encoding = UnicodeEncoding::UTF8;
  } else {
    return errors::InvalidArgument(strings::StrCat(
        "Invalid encoding \"", str, "\": Should be one of: BYTE"));
  }
  return Status::OK();
}

// Sets unit value based on str.
Status ParseCharUnit(const string& str, CharUnit* unit) {
  if (str == "BYTE") {
    *unit = CharUnit::BYTE;
  } else if (str == "UTF8_CHAR") {
    *unit = CharUnit::UTF8_CHAR;
  } else {
    return errors::InvalidArgument(strings::StrCat(
        "Invalid unit \"", str, "\": Should be one of: BYTE, UTF8_CHAR"));
  }
  return Status::OK();
}

// Return the number of Unicode characters in a UTF-8 string.
// Result may be incorrect if the input string is not valid UTF-8.
int32 UTF8StrLen(const string& string) {
  const int32 byte_size = string.size();
  const char* const end = string.data() + byte_size;
  const char* ptr = string.data();
  int32 skipped_count = 0;
  while (ptr < end) {
    skipped_count += IsTrailByte(*ptr++) ? 1 : 0;
  }
  const int32 result = byte_size - skipped_count;
  return result;
}

}  // namespace tensorflow
