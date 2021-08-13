// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#ifndef IMPALA_EXEC_HDFS_JSON_SCANNER_H
#define IMPALA_EXEC_HDFS_JSON_SCANNER_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <arrow/api.h>
#include <arrow/array.h>
#include <arrow/buffer.h>
#include <arrow/config.h>
#include <arrow/io/interfaces.h>
#include <arrow/io/type_fwd.h>
#include <arrow/json/api.h>
#include <arrow/json/chunked_builder.h>
#include <arrow/json/chunker.h>
#include <arrow/json/converter.h>
#include <arrow/json/options.h>
#include <arrow/json/parser.h>
#include <arrow/json/reader.h>
#include <arrow/memory_pool.h>
#include <arrow/result.h>
#include <arrow/type_fwd.h>
#include <arrow/util/macros.h>
#include <arrow/util/string_view.h>
#include <arrow/util/type_fwd.h>
#include "exec/hdfs-scan-node.h"
#include "exec/hdfs-scanner.h"
#include "runtime/exec-env.h"
#include "runtime/io/disk-io-mgr.h"
#include "runtime/mem-pool.h"
#include "runtime/runtime-state.h"
#include "runtime/string-buffer.h"
#include "util/runtime-profile-counters.h"

namespace impala {

struct HdfsFileDesc;

class HdfsJsonScanner : public HdfsScanner {
 public:
  class ResourceError : public std::runtime_error {
   public:
    explicit ResourceError(const Status& status)
      : runtime_error(status.msg().msg()), status_(status) {}
    virtual ~ResourceError() {}
    Status& GetStatus() { return status_; }
   private:
    Status status_;
  };

  /// A wrapper of arrow::memorypool to track memory used by arrow
  class ArrowMemPool : public arrow::MemoryPool {
   public:
    ArrowMemPool(HdfsJsonScanner* scanner, MemoryPool* pool);
    ~ArrowMemPool() override = default;

    arrow::Status Allocate(int64_t size, uint8_t** out) override;
    arrow::Status Reallocate(int64_t old_size, int64_t new_size, uint8_t** ptr) override;

    void Free(uint8_t* buffer, int64_t size) override;
    int64_t bytes_allocated() const override;
    std::string backend_name() const override;

   private:
    arrow::MemoryPool* pool_;
    HdfsJsonScanner* scanner_;
    MemTracker* mem_tracker_;
    boost::unordered_map<uint8_t*, uint64_t> chunk_sizes_;
  };

  /// A wrapper of DiskIoMgr to be used by the Arrow lib.
  class ScanRangeInputStream : public arrow::io::InputStream {
   public:
    ScanRangeInputStream(HdfsJsonScanner* scanner) {
      this->scanner_ = scanner;
      this->filename_ = scanner->filename();
      this->file_desc_ = scanner->scan_node_->GetFileDesc(
          scanner->context_->partition_descriptor()->id(), filename_);
    }
    arrow::Status Close() override {
      closed_ = true;
      return arrow::Status::OK();
    }

    arrow::Result<int64_t> Tell() const override { return pos_; }
    bool closed() const override { return closed_; }
    arrow::Result<int64_t> Read(int64_t nbytes, void* out) override;
    arrow::Result<std::shared_ptr<arrow::Buffer>> Read(int64_t nbytes) override;

   private:
    int64_t pos_ = 0;
    bool closed_ = false;
    HdfsJsonScanner* scanner_;
    const HdfsFileDesc* file_desc_;
    std::string filename_;
  };

  HdfsJsonScanner(HdfsScanNodeBase* scan_node, RuntimeState* state);
  virtual ~HdfsJsonScanner();

  /// Implementation of HdfsScanner interface.
  virtual Status Open(ScannerContext* context) override WARN_UNUSED_RESULT;
  virtual void Close(RowBatch* row_batch) override;
  THdfsFileFormat::type file_format() const override {
    return THdfsFileFormat::JSON;
  }
  Status InitNewRange() override WARN_UNUSED_RESULT { return Status::OK(); }
  Status GetNextInternal(RowBatch* row_batch) override WARN_UNUSED_RESULT;
  /// Issue io manager byte ranges for 'files'.
  static Status IssueInitialRanges(HdfsScanNodeBase* scan_node,
      const std::vector<HdfsFileDesc*>& files) WARN_UNUSED_RESULT;

 private:
  Status ReadTable(std::shared_ptr<arrow::io::InputStream> input_stream);
  inline Status AllocateTupleMem(RowBatch* row_batch) WARN_UNUSED_RESULT;
  const char* filename() const { return metadata_range_->file(); }
  int row_read_;
  int num_rows_;
  int chunk_pos_ = 0;
  int start_pos_ = 0;
  int chunked_boundary_ = 0;
  uint8_t* tuple_mem_end_ = nullptr;
  const io::ScanRange* metadata_range_ = nullptr;
  std::shared_ptr<arrow::json::TableReader> reader_ = nullptr;
  arrow::json::ParseOptions parseOptions_;
  arrow::json::ReadOptions readOptions_;
  std::shared_ptr<arrow::Table> table_;
  boost::scoped_ptr<MemPool> data_batch_pool_;
};
} // namespace impala

#endif
