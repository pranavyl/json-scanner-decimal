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

#include "exec/hdfs-json-scanner.h"
#include "common/names.h"
#include "runtime/collection-value-builder.h"
#include "runtime/datetime-simple-date-format-parser.h"
#include "runtime/io/request-context.h"
#include "runtime/mem-tracker.h"
#include "runtime/row-batch.h"
#include "runtime/runtime-filter.inline.h"
#include "runtime/timestamp-value.h"
#include "runtime/timestamp-value.inline.h"
#include "runtime/tuple-row.h"
#include "util/decompress.h"
#include "util/debug-util.h"
#include "exec/exec-node.h"
#include "exprs/scalar-expr-evaluator.h"


using namespace impala;
using namespace impala::io;

Status HdfsJsonScanner::IssueInitialRanges(
    HdfsScanNodeBase* scan_node, const vector<HdfsFileDesc*>& files) {
  DCHECK(!files.empty());
  for (HdfsFileDesc* file : files) {
    RETURN_IF_ERROR(scan_node->AddDiskIoRanges(file, EnqueueLocation::TAIL));
  }
  return Status::OK();
}

HdfsJsonScanner::ArrowMemPool::ArrowMemPool(HdfsJsonScanner* scanner, MemoryPool* pool)
  : pool_(pool), scanner_(scanner), mem_tracker_(scanner_->scan_node_->mem_tracker()) {}

arrow::Status HdfsJsonScanner::ArrowMemPool::Allocate(int64_t size, uint8_t** out) {
  if (!mem_tracker_->TryConsume(size)) {
    throw ResourceError(mem_tracker_->MemLimitExceeded(
        scanner_->state_, "Failed to allocate memory required by Arrow library", size));
  }
  arrow::Status s = pool_->Allocate(size, out);
  if (*out == nullptr) {
    mem_tracker_->Release(size);
    throw ResourceError(Status(TErrorCode::MEM_ALLOC_FAILED, size));
  }
  chunk_sizes_[*out] = size;
  return s;
}

arrow::Status HdfsJsonScanner::ArrowMemPool::Reallocate(
    int64_t old_size, int64_t new_size, uint8_t** ptr) {
  if (new_size < old_size) {
    mem_tracker_->Release(old_size - new_size);
  } else if (!mem_tracker_->TryConsume(new_size - old_size)) {
    throw ResourceError(mem_tracker_->MemLimitExceeded(scanner_->state_,
        "Failed to allocate memory required by Arrow library", new_size - old_size));
  }
  arrow::Status s = pool_->Reallocate(old_size, new_size, ptr);
  chunk_sizes_[*ptr] = new_size;
  return s;
}

void HdfsJsonScanner::ArrowMemPool::Free(uint8_t* buffer, int64_t size) {
  //DCHECK(chunk_sizes_.find(buffer) != chunk_sizes_.end()) << "invalid free!" << endl
  //                                                        << GetStackTrace();
  //VLOG_QUERY << "free size" << size;
  int64_t total_bytes_released = 0;
  for (auto it = chunk_sizes_.begin(); it != chunk_sizes_.end(); ++it) {
    //std::free(it->first);
    total_bytes_released += it->second;
  }
  VLOG_QUERY << "total_bytes_released:::" << total_bytes_released;
  VLOG_QUERY << "size:FREE:::" << size;
  //mem_tracker_->Release(total_bytes_released);
  pool_->Free(buffer, size);
  mem_tracker_->Release(size);
  //chunk_sizes_.erase(buffer);
  //chunk_sizes_.clear();
}

int64_t HdfsJsonScanner::ArrowMemPool::bytes_allocated() const {
  int64_t nb_bytes = pool_->bytes_allocated();
  return nb_bytes;
}

std::string HdfsJsonScanner::ArrowMemPool::backend_name() const {
  return pool_->backend_name();
}

// Supporting method for reading input stream
// returns number of bytes read
arrow::Result<int64_t> HdfsJsonScanner::ScanRangeInputStream::Read(
    int64_t nbytes, void* out) {
  const ScanRange* metadata_range = scanner_->metadata_range_;
  //const ScanRange* split_range =
  //    reinterpret_cast<ScanRangeMetadata*>(metadata_range->meta_data())->original_split;
  int64_t partition_id = scanner_->context_->partition_descriptor()->id();
  int64_t file_length = file_desc_->file_length;
  if ((pos_ + nbytes) > file_length) {
    nbytes = file_length - pos_;
  }
  if (nbytes == 0) {
    return 0;
  }
  // Set expected_local to false to avoid cache on stale data (IMPALA-6830)
  //bool expected_local = false;
  int cache_options = metadata_range->cache_options() & ~BufferOpts::USE_HDFS_CACHE;
  /*
  ScanRange* range = scanner_->scan_node_->AllocateScanRange(
    ScanRange::FileInfo{scanner_->filename(), metadata_range->fs(),
        split_range->mtime(), split_range->is_erasure_coded()},
    nbytes, pos_, partition_id, split_range->disk_id(), expected_local,
    BufferOpts::ReadInto(reinterpret_cast<uint8_t*>(out), nbytes, cache_options));
  ScanRange* object_range = scan_node_->AllocateScanRange(
      metadata_range_->fs(), filename(), size,
      offset, partition_id,
      metadata_range_->disk_id(), metadata_range_->expected_local(),
      metadata_range_->mtime(),
      BufferOpts::ReadInto(buffer, size, cache_options));
 */
  ScanRange* range = scanner_->scan_node_->AllocateScanRange(
      scanner_->metadata_range_->GetFileInfo(), nbytes, pos_, partition_id,
      scanner_->metadata_range_->disk_id(), scanner_->metadata_range_->expected_local(),
      BufferOpts::ReadInto(reinterpret_cast<uint8_t*>(out), nbytes, cache_options));
  //ScanRange* range = scanner_->scan_node_->AllocateScanRange(metadata_range->fs(),
  //   scanner_->filename(), nbytes, pos_, partition_id, metadata_range->disk_id(),
  //    expected_local, metadata_range->mtime(),
  //    BufferOpts::ReadInto(reinterpret_cast<uint8_t*>(out), nbytes, cache_options));
  pos_ += nbytes;
  unique_ptr<BufferDescriptor> io_buffer;

  Status status;
  SCOPED_TIMER2(scanner_->state_->total_storage_wait_timer(),
      scanner_->scan_node_->scanner_io_wait_time());
  bool needs_buffers;
  status = scanner_->scan_node_->reader_context()->StartScanRange(range, &needs_buffers);
  DCHECK(!status.ok() || !needs_buffers) << "Already provided a buffer";
  if (status.ok()) status = range->GetNext(&io_buffer);

  if (io_buffer != nullptr) range->ReturnBuffer(move(io_buffer));
  if (!status.ok()) throw ResourceError(status);
  return nbytes;
}

// Main method for reading input stream
// returns the buffer of input stream read
arrow::Result<std::shared_ptr<arrow::Buffer>> HdfsJsonScanner::ScanRangeInputStream::Read(
    int64_t nbytes) {
  auto buffer = *arrow::AllocateResizableBuffer(nbytes);
  int64_t bytes_read;
  arrow::Result<int64_t> res;
  res = Read(nbytes, buffer->mutable_data());
  if (res.ok()) {
    bytes_read = res.ValueOrDie();
  } else {
    return res.status();
  }
  if (bytes_read < nbytes) {
    // Change size but do not reallocate internal capacity
    RETURN_NOT_OK(buffer->Resize(bytes_read, false));
    buffer->ZeroPadding();
  }
  return std::move(buffer);
}

HdfsJsonScanner::HdfsJsonScanner(HdfsScanNodeBase* scan_node, RuntimeState* state)
  : HdfsScanner(scan_node, state),
    data_batch_pool_(new MemPool(scan_node->mem_tracker())) {}

HdfsJsonScanner::~HdfsJsonScanner() {}

// input : impala column type as specified in schema
// output : arrow data type to be specified in read options.
static std::shared_ptr<arrow::DataType> ColumnType2ArrowType(const ColumnType ct) {
  //if(ct.type == TYPE_NULL) {
   // return arrow::null();
    if (ct.type == TYPE_TINYINT) {
    return arrow::int8();
  } else if (ct.type == TYPE_SMALLINT) {
    return arrow::int16();
  } else if (ct.type == TYPE_INT) {
    return arrow::int32();
  } else if (ct.type == TYPE_BIGINT) {
    return arrow::int64();
  } else if (ct.type == TYPE_FLOAT) {
    return arrow::float32();
  } else if (ct.type == TYPE_DOUBLE) {
    return arrow::float64();
  } else if (ct.type == TYPE_DECIMAL) {
    // VLOG_QUERY << "decimal128" << arrow::decimal128(ct.precision, ct.scale)->ToString();
    return arrow::decimal128(ct.precision,ct.scale);
  } else if (ct.IsIntegerType()) {
    return arrow::int32();
  } else if (ct.IsStringType()) {
    return arrow::utf8();
  } else if (ct.type == TYPE_TIMESTAMP) {
    return arrow::utf8();
  } else if (ct.type == TYPE_BOOLEAN) {
    return arrow::boolean();
  }
  return arrow::utf8();
}

// method to create an arrow reader from the input stream and read arrow table
Status HdfsJsonScanner::ReadTable(std::shared_ptr<arrow::io::InputStream> input_stream) {
  // TODO: handle the arrow exceptions thrown while creating reader or  reading table
  arrow::Status st;
  std::shared_ptr<arrow::json::TableReader> reader;
  arrow::Result<std::shared_ptr<arrow::json::TableReader>> res;
  res =
      arrow::json::TableReader::Make(new ArrowMemPool(this, arrow::default_memory_pool()),
          input_stream, readOptions_, parseOptions_);
  if (res.ok()) {
    reader = res.ValueOrDie();
  } else {
    return Status(res.status().message());
  }
  arrow::Result<std::shared_ptr<arrow::Table>> res2;
  std::shared_ptr<arrow::Table> table;
  res2 = reader->Read();
  if (res2.ok()) {
    table = res2.ValueOrDie();
  } else {
    return Status(res2.status().message());
  }
  table_ = table;
  VLOG_QUERY << "num_rows_ReadTable:::" << table->num_rows();
  return Status::OK();
}

Status HdfsJsonScanner::Open(ScannerContext* context) {
  RETURN_IF_ERROR(HdfsScanner::Open(context));
  VLOG_QUERY << " PrintTuple:::" << PrintTuple(template_tuple_, *scan_node_->tuple_desc());
  //VLOG_QUERY << "Filename():::" << stream_->filename() << " " << stream_->file_offset();
  VLOG_QUERY << "ScanRange:::" << stream_->scan_range()->DebugString();

  metadata_range_ = stream_->scan_range();
  VLOG_QUERY << "offset" << metadata_range_->offset();
  TupleDescriptor td = *scan_node_->tuple_desc();
  VLOG_QUERY << "tuple descriptor: " << td.DebugString() << std::endl << GetStackTrace();
  const TableDescriptor* tad = td.table_desc();
  vector<ColumnDescriptor> cv = tad->col_descs();
  vector<std::shared_ptr<arrow::Field>> fields_list = {};
  // convert impala tuple descriptor to arrow schema
  for (auto cvf : cv) {
    std::shared_ptr<arrow::Field> field_a =
        arrow::field(cvf.name(), ColumnType2ArrowType(cvf.type()));
    fields_list.push_back(field_a);
  }
  std::shared_ptr<arrow::Schema> schema;
  schema = arrow::schema(fields_list);
  parseOptions_ = arrow::json::ParseOptions::Defaults();
  readOptions_ = arrow::json::ReadOptions::Defaults();
  parseOptions_.explicit_schema = schema;
  readOptions_.use_threads = false;
  std::shared_ptr<arrow::io::InputStream> input_stream(new ScanRangeInputStream(this));
  // reading the arrow table
  Status status = ReadTable(input_stream);
  if (!status.ok()) {
    return status;
  }
  row_read_ = 0;
  num_rows_ = table_->num_rows();
  VLOG_QUERY<< "Filename:::"<< stream_->filename()<< " "<< "num_rows_openfunction():::" << num_rows_;
  return Status::OK();
}

Status HdfsJsonScanner::AllocateTupleMem(RowBatch* row_batch) {
  int64_t tuple_buffer_size;
  RETURN_IF_ERROR(
      row_batch->ResizeAndAllocateTupleBuffer(state_, &tuple_buffer_size, &tuple_mem_));
  tuple_mem_end_ = tuple_mem_ + tuple_buffer_size;
  tuple_ = reinterpret_cast<Tuple*>(tuple_mem_);
  DCHECK_GT(row_batch->capacity(), 0);
  return Status::OK();
}

Status HdfsJsonScanner::GetNextInternal(RowBatch* row_batch) {
  VLOG_QUERY << "row read an total num: " << row_read_ << " " << num_rows_ << std::endl;
  if (row_read_ >= num_rows_) {
    eos_ = true;
    return Status::OK();
  }
  tuple_= nullptr;
  tuple_mem_=nullptr;

  const TupleDescriptor* tuple_desc = scan_node_->tuple_desc();
  //const RowDescriptor* row_desc = scan_node_->row_desc();
  Tuple* template_tuple = template_tuple_map_[tuple_desc];
  if (template_tuple != nullptr) {
        VLOG_QUERY << " PrintTemplateTuple:::" << PrintTuple(template_tuple, *tuple_desc) << std::endl;
  }
  VLOG_QUERY << " PrintTemplateTuple:::" << PrintTuple(template_tuple, *tuple_desc) << std::endl;

  const vector<ScalarExprEvaluator*>& evals = conjunct_evals_map_[tuple_desc->id()];

  int row_id = row_batch->num_rows();
  VLOG_QUERY << "num_rows:::" << row_id;
  if (tuple_ == nullptr) RETURN_IF_ERROR(AllocateTupleMem(row_batch));

  TupleRow* row = row_batch->GetRow(row_id);
  Tuple* tuple = tuple_;
  VLOG_QUERY << "PrintRowBatch:::" << PrintBatch(row_batch) << std::endl;

  int i; // reading_cursor
  // reading rows from the previous point we left, tilll either end of table/capacity of
  // rowbatch large file do not work currently with this fix
  VLOG_QUERY << "start pos:::" << start_pos_;
  int num_to_commit = 0;
  for (i = row_read_; i < num_rows_ && (i - row_read_) < row_batch->capacity(); i++) {
    InitTuple(tuple_desc, template_tuple, tuple);
    //VLOG_QUERY << "PrintRow:::"<< PrintRow(row, *row_desc);
    //VLOG_QUERY << " PrintTuple:::" << PrintTuple(tuple, *tuple_desc);
    //if (!EvalRuntimeFilters(reinterpret_cast<TupleRow*>(row)) || !ExecNode::EvalConjuncts(evals.data(), evals.size(), row)) {
      //VLOG_QUERY << "Inside filter ::::" << std::endl;
      //tuple = next_tuple(tuple_desc->byte_size(), tuple);
      //row = next_row(row);
      //continue;
    //}
    int k = 0;
    for (const SlotDescriptor* slot_desc : tuple_desc->slots()) {
      if (slot_desc->col_pos() < scan_node_->num_partition_keys() &&
          !slot_desc->IsVirtual()) {
                continue;
      }
      const auto& column = table_->column(slot_desc->col_pos());
      chunked_boundary_ = start_pos_ + column->chunk(chunk_pos_)->length();
      if (i == chunked_boundary_) {
        start_pos_ = chunked_boundary_;
        chunk_pos_++;
      }
      if(column->chunk(chunk_pos_)->IsNull(i - start_pos_)) {
        VLOG_QUERY << "NULL condition :::"<< std::endl;
        tuple->SetNull(slot_desc->null_indicator_offset());
        continue;
      }
      void* slot_val_ptr = tuple->GetSlot(slot_desc->tuple_offset());
      auto ard = column->chunk(chunk_pos_)->data();

       if (ard->type->ToString() == "int32") {
        arrow::Int32Array int_arr(ard);
        *(reinterpret_cast<int32_t*>(slot_val_ptr)) = int_arr.Value(i - start_pos_);
      } else if (ard->type->ToString() == "int8") {
        arrow::Int8Array int_arr(ard);
        *(reinterpret_cast<int8_t*>(slot_val_ptr)) = int_arr.Value(i - start_pos_);
      } else if (ard->type->ToString() == "int16") {
        arrow::Int16Array int_arr(ard);
        *(reinterpret_cast<int16_t*>(slot_val_ptr)) = int_arr.Value(i - start_pos_);
      } else if (ard->type->ToString() == "int64") {
        arrow::Int64Array int_arr(ard);
        *(reinterpret_cast<int64_t*>(slot_val_ptr)) = int_arr.Value(i - start_pos_);
      } else if (ard->type->ToString().find("decimal")!= string::npos) {
        arrow::Decimal128Array decimal_arr(ard);
        const arrow::Decimal128 value(decimal_arr.GetValue(i));
        if(slot_desc->type().precision == 0 || slot_desc->type().precision > 18) {
          int128_t val1 = value.high_bits();
          val1 <<= 64;
          val1 |= value.low_bits();
          // Use memcpy to avoid gcc generating unaligned instructions like movaps
          // for int128_t. They will raise SegmentFault when addresses are not
          // aligned to 16 bytes.
          memcpy(slot_val_ptr, &val1, sizeof(int128_t));
        } else {
          switch (slot_desc->slot_size()) {
            case 4:
              (*reinterpret_cast<Decimal4Value*>(slot_val_ptr)) = value.low_bits();
              break;
            case 8:
              (*reinterpret_cast<Decimal8Value*>(slot_val_ptr)) = value.low_bits();
              break;
            case 12:
              DCHECK(false) << "Planner should not generate this.";
              break;
            case 16:
              (*reinterpret_cast<Decimal16Value*>(slot_val_ptr)) = value.low_bits();
              break;
            default:
              DCHECK(false) << "Decimal slots can't be this size.";
          }
        }
      } else if (ard->type->ToString() == "bool") {
        arrow::BooleanArray bool_arr(ard);
        *(reinterpret_cast<bool*>(slot_val_ptr)) = bool_arr.Value(i - start_pos_);
      } else if (ard->type->ToString() == "string") {
        arrow::StringArray s_arr(ard);
        DCHECK(s_arr.IsValid(i - start_pos_))
            << "length: " << s_arr.length() << ", offset: " << s_arr.offset();
        int src_len = s_arr.value_length(i - start_pos_);
        int dst_len = slot_desc->type().len;
        char* src_ptr;
        int32_t leng;
        MemPool* pool = data_batch_pool_.get();

        char* blob_;
        //DCHECK(src_len > 0) << "i for size error" << i << "len" << src_len
        //                    << "length: " << s_arr.length()
        //                    << ", offset: " << s_arr.offset();
        blob_ = reinterpret_cast<char*>(pool->TryAllocateUnaligned(src_len));
        auto val = s_arr.GetValue(i - start_pos_, &leng);
        auto val_char = reinterpret_cast<const char*>(val);
        memcpy(blob_, val_char, src_len);
        src_ptr = blob_;
          if (slot_desc->type().type == TYPE_CHAR) {
          int unpadded_len = min(dst_len, src_len);
          char* dst = reinterpret_cast<char*>(slot_val_ptr);
          memcpy(dst, src_ptr, unpadded_len);
          StringValue::PadWithSpaces(dst, dst_len, unpadded_len);
        } else if (slot_desc->type().type == TYPE_TIMESTAMP) {
          TimestampValue* tv = reinterpret_cast<TimestampValue*>(slot_val_ptr);
          *tv = TimestampValue::ParseSimpleDateFormat(src_ptr, src_len);
        } else {
          StringValue* dst = reinterpret_cast<StringValue*>(slot_val_ptr);
          dst->len = src_len;
          if (slot_desc->type().type == TYPE_VARCHAR && (src_len > dst_len)) {
            dst->len = dst_len;
          }
          dst->ptr = src_ptr;
        }
      } else if (ard->type->ToString() == "double") {
        arrow::DoubleArray d_arr(ard);
        *(reinterpret_cast<double*>(slot_val_ptr)) = d_arr.Value(i - start_pos_);
      } else if (ard->type->ToString() == "float") {
        arrow::FloatArray f_arr(ard);
        // VLOG_QUERY << (f_arr.Value(i));
        *(reinterpret_cast<float*>(slot_val_ptr)) = f_arr.Value(i - start_pos_);
      }
      k++;

    }
    row->SetTuple(0, tuple);
    VLOG_QUERY << " PrintTuple:::" << PrintTuple(tuple, *tuple_desc);
    if (!EvalRuntimeFilters(reinterpret_cast<TupleRow*>(row))) {
      VLOG_QUERY << "Inside runtime filter ::::" << std::endl;
      continue;
    }
    if (!ExecNode::EvalConjuncts(evals.data(), evals.size(), row)) {
      VLOG_QUERY << "Inside eval conjuncts ::::" << std::endl;
      continue;
    }
    //if (EvalRuntimeFilters(reinterpret_cast<TupleRow*>(row)) && ExecNode::EvalConjuncts(evals.data(), evals.size(), row)) {
    //  VLOG_QUERY << "Inside filter ::::" << std::endl;
    //  tuple = next_tuple(tuple_desc->byte_size(), tuple);
    //}

    //if (ExecNode::EvalConjuncts(evals.data(), evals.size(), row)) {
     // VLOG_QUERY << "Inside filter ::::" << std::endl;
     // tuple = next_tuple(tuple_desc->byte_size(), tuple);
     // row = next_row(row);
    //}
    num_to_commit += 1;
    tuple = next_tuple(tuple_desc->byte_size(), tuple);
    row = next_row(row);
  }
  VLOG_QUERY << "i:::" << i << "row read:::" << row_read_ << "row batch:::" << row_batch;

  //Status s = CommitRows(i - row_read_, row_batch);
  VLOG_QUERY << "num_to_commit = " << num_to_commit;
  Status s = CommitRows(num_to_commit, row_batch);
  row_read_ = i;
  row_batch->VLogRows("HdfsJsonScanner::GetNextInternal()");
  return s;
}

void HdfsJsonScanner::Close(RowBatch* row_batch) {
  if (row_batch != nullptr) {
    context_->ReleaseCompletedResources(true);
    row_batch->tuple_data_pool()->AcquireData(template_tuple_pool_.get(), false);
    row_batch->tuple_data_pool()->AcquireData(data_batch_pool_.get(), false);
    if (scan_node_->HasRowBatchQueue()) {
      static_cast<HdfsScanNode*>(scan_node_)
          ->AddMaterializedRowBatch(unique_ptr<RowBatch>(row_batch));
    }
  }
  scan_node_->RangeComplete(THdfsFileFormat::JSON, THdfsCompression::NONE);

  for (int i = 0; i < filter_ctxs_.size(); ++i) {
    const FilterStats* stats = filter_ctxs_[i]->stats;
    const LocalFilterStats& local = filter_stats_[i];
    stats->IncrCounters(
        FilterStats::ROWS_KEY, local.total_possible, local.considered, local.rejected);
  }
  CloseInternal();
}
