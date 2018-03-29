/*
 * Copyright 2018 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Implementation of fastq_reader.h
#include "nucleus/io/fastq_reader.h"

#include "nucleus/protos/fastq.pb.h"
#include "nucleus/util/utils.h"
#include "nucleus/vendor/zlib_compression_options.h"
#include "nucleus/vendor/zlib_inputstream.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/lib/io/buffered_inputstream.h"
#include "tensorflow/core/lib/io/compression.h"
#include "tensorflow/core/lib/io/random_inputstream.h"
#include "tensorflow/core/lib/strings/str_util.h"
#include "tensorflow/core/lib/strings/strcat.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/platform/file_system.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/types.h"

namespace nucleus {

namespace tf = tensorflow;

// 256 KB read buffer.
constexpr int READER_BUFFER_SIZE = 256 * 1024 - 1;

constexpr char HEADER_SYMBOL = '@';
constexpr char SEQUENCE_AND_QUALITY_SEPARATOR_SYMBOL = '+';

// -----------------------------------------------------------------------------
//
// Reader for FASTQ formats containing NGS reads.
//
// -----------------------------------------------------------------------------

namespace {
tf::Status ConvertToPb(const string& header, const string& sequence,
                       const string& pad, const string& quality,
                       nucleus::genomics::v1::FastqRecord* record) {
  CHECK(record != nullptr) << "FASTQ record cannot be null";
  record->Clear();

  // Validate the four lines as appropriate.
  if (!header.length() || header[0] != HEADER_SYMBOL ||
      sequence.length() != quality.length()) {
    return tf::errors::DataLoss("Failed to parse FASTQ record");
  }

  size_t spaceix = header.find(" ");
  if (spaceix == string::npos) {
    record->set_id(header.substr(1));
  } else {
    record->set_id(header.substr(1, spaceix - 1));
    record->set_description(header.substr(spaceix + 1));
  }

  record->set_sequence(sequence);
  record->set_quality(quality);

  return tf::Status::OK();
}
}  // namespace

// Iterable class for traversing all FASTQ records in the file.
class FastqFullFileIterable : public FastqIterable {
 public:
  // Advance to the next record.
  StatusOr<bool> Next(nucleus::genomics::v1::FastqRecord* out) override;

  // Constructor is invoked via FastqReader::Iterate.
  FastqFullFileIterable(const FastqReader* reader);
  ~FastqFullFileIterable() override;
};

StatusOr<std::unique_ptr<FastqReader>> FastqReader::FromFile(
    const string& fastq_path,
    const nucleus::genomics::v1::FastqReaderOptions& options) {
  std::unique_ptr<tensorflow::RandomAccessFile> fp;
  tf::Status status =
      tf::Env::Default()->NewRandomAccessFile(fastq_path.c_str(), &fp);
  if (!status.ok()) {
    return tf::errors::NotFound(
        tf::strings::StrCat("Could not open ", fastq_path));
  }
  return std::unique_ptr<FastqReader>(new FastqReader(fp.release(), options));
}

FastqReader::FastqReader(
    tensorflow::RandomAccessFile* fp,
    const nucleus::genomics::v1::FastqReaderOptions& options)
    : options_(options), src_(fp) {
  if (options.compression_type() ==
      nucleus::genomics::v1::FastqReaderOptions::GZIP) {
    file_stream_.reset(new tf::io::RandomAccessInputStream(src_));
    zlib_stream_.reset(new tf::io::ZlibInputStream(
        file_stream_.get(), READER_BUFFER_SIZE, READER_BUFFER_SIZE,
        tf::io::ZlibCompressionOptions::GZIP()));
    buffered_inputstream_.reset(new tf::io::BufferedInputStream(
        zlib_stream_.get(), READER_BUFFER_SIZE));
  } else {
    buffered_inputstream_.reset(
        new tf::io::BufferedInputStream(src_, READER_BUFFER_SIZE));
  }
}

FastqReader::~FastqReader() { TF_CHECK_OK(Close()); }

tf::Status FastqReader::Close() {
  buffered_inputstream_.reset();
  zlib_stream_.reset();
  file_stream_.reset();
  if (src_) {
    delete src_;
    src_ = nullptr;
  }
  return tf::Status::OK();
}

tf::Status FastqReader::Next(string* header, string* sequence, string* pad,
                             string* quality) const {
  header->clear();
  sequence->clear();
  pad->clear();
  quality->clear();

  // Read the four lines, returning early if we are at the end of the stream or
  // the record is truncated.
  TF_RETURN_IF_ERROR(buffered_inputstream_->ReadLine(header));
  TF_RETURN_IF_ERROR(buffered_inputstream_->ReadLine(sequence));
  TF_RETURN_IF_ERROR(buffered_inputstream_->ReadLine(pad));
  TF_RETURN_IF_ERROR(buffered_inputstream_->ReadLine(quality));

  return tf::Status::OK();
}

StatusOr<std::shared_ptr<FastqIterable>> FastqReader::Iterate() const {
  if (src_ == nullptr)
    return tf::errors::FailedPrecondition(
        "Cannot Iterate a closed FastqReader.");
  return StatusOr<std::shared_ptr<FastqIterable>>(
      MakeIterable<FastqFullFileIterable>(this));
}

// Iterable class definitions.
StatusOr<bool> FastqFullFileIterable::Next(
    nucleus::genomics::v1::FastqRecord* out) {
  TF_RETURN_IF_ERROR(CheckIsAlive());
  const FastqReader* fastq_reader = static_cast<const FastqReader*>(reader_);
  string header;
  string sequence;
  string pad;
  string quality;
  tf::Status status = fastq_reader->Next(&header, &sequence, &pad, &quality);
  if (tf::errors::IsOutOfRange(status)) {
    return false;
  } else if (!status.ok()) {
    return status;
  }
  TF_RETURN_IF_ERROR(ConvertToPb(header, sequence, pad, quality, out));
  return true;
}

FastqFullFileIterable::~FastqFullFileIterable() {}

FastqFullFileIterable::FastqFullFileIterable(const FastqReader* reader)
    : Iterable(reader) {}

}  // namespace nucleus
