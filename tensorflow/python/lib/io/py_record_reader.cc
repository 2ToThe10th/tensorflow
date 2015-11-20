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

#include "tensorflow/python/lib/io/py_record_reader.h"

#include "tensorflow/core/lib/core/stringpiece.h"
#include "tensorflow/core/lib/io/record_reader.h"
#include "tensorflow/core/platform/port.h"
#include "tensorflow/core/public/env.h"

namespace tensorflow {

class RandomAccessFile;

namespace io {

PyRecordReader::PyRecordReader() {}

PyRecordReader* PyRecordReader::New(const string& filename,
                                    uint64 start_offset) {
  RandomAccessFile* file;
  Status s = Env::Default()->NewRandomAccessFile(filename, &file);
  if (!s.ok()) {
    return nullptr;
  }
  PyRecordReader* reader = new PyRecordReader;
  reader->offset_ = start_offset;
  reader->file_ = file;
  reader->reader_ = new RecordReader(reader->file_);
  return reader;
}

PyRecordReader::~PyRecordReader() {
  delete reader_;
  delete file_;
}

bool PyRecordReader::GetNext() {
  if (reader_ == nullptr) return false;
  Status s = reader_->ReadRecord(&offset_, &record_);
  return s.ok();
}

void PyRecordReader::Close() {
  delete reader_;
  delete file_;
  file_ = nullptr;
  reader_ = nullptr;
}

}  // namespace io
}  // namespace tensorflow
