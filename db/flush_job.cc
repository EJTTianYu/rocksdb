//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/flush_job.h"

#include <cinttypes>

#include <algorithm>
#include <vector>

#include "db/builder.h"
#include "db/db_iter.h"
#include "db/dbformat.h"
#include "db/event_helpers.h"
#include "db/log_reader.h"
#include "db/log_writer.h"
#include "db/memtable.h"
#include "db/memtable_list.h"
#include "db/merge_context.h"
#include "db/range_tombstone_fragmenter.h"
#include "db/version_set.h"
#include "file/file_util.h"
#include "file/filename.h"
#include "logging/event_logger.h"
#include "logging/log_buffer.h"
#include "logging/logging.h"
#include "monitoring/iostats_context_imp.h"
#include "monitoring/perf_context_imp.h"
#include "monitoring/thread_status_util.h"
#include "port/port.h"
#include "rocksdb/db.h"
#include "rocksdb/env.h"
#include "rocksdb/statistics.h"
#include "rocksdb/status.h"
#include "rocksdb/table.h"
#include "table/merging_iterator.h"
#include "table/table_builder.h"
#include "table/two_level_iterator.h"
#include "test_util/sync_point.h"
#include "util/coding.h"
#include "util/mutexlock.h"
#include "util/stop_watch.h"

namespace ROCKSDB_NAMESPACE {

const char* GetFlushReasonString (FlushReason flush_reason) {
  switch (flush_reason) {
    case FlushReason::kOthers:
      return "Other Reasons";
    case FlushReason::kGetLiveFiles:
      return "Get Live Files";
    case FlushReason::kShutDown:
      return "Shut down";
    case FlushReason::kExternalFileIngestion:
      return "External File Ingestion";
    case FlushReason::kManualCompaction:
      return "Manual Compaction";
    case FlushReason::kWriteBufferManager:
      return "Write Buffer Manager";
    case FlushReason::kWriteBufferFull:
      return "Write Buffer Full";
    case FlushReason::kTest:
      return "Test";
    case FlushReason::kDeleteFiles:
      return "Delete Files";
    case FlushReason::kAutoCompaction:
      return "Auto Compaction";
    case FlushReason::kManualFlush:
      return "Manual Flush";
    case FlushReason::kErrorRecovery:
      return "Error Recovery";
    case FlushReason::kWalFull:
      return "WAL Full";
    default:
      return "Invalid";
  }
}

FlushJob::FlushJob(
    const std::string& dbname, ColumnFamilyData* cfd,
    const ImmutableDBOptions& db_options,
    const MutableCFOptions& mutable_cf_options, uint64_t max_memtable_id,
    const FileOptions& file_options, VersionSet* versions,
    InstrumentedMutex* db_mutex, std::atomic<bool>* shutting_down,
    std::vector<SequenceNumber> existing_snapshots,
    SequenceNumber earliest_write_conflict_snapshot,
    SnapshotChecker* snapshot_checker, JobContext* job_context,
    LogBuffer* log_buffer, FSDirectory* db_directory,
    FSDirectory* output_file_directory, CompressionType output_compression,
    Statistics* stats, EventLogger* event_logger, bool measure_io_stats,
    const bool sync_output_directory, const bool write_manifest,
    Env::Priority thread_pri, const std::shared_ptr<IOTracer>& io_tracer,
    const std::string& db_id, const std::string& db_session_id,
    std::string full_history_ts_low, BlobFileCompletionCallback* blob_callback)
    : dbname_(dbname),
      db_id_(db_id),
      db_session_id_(db_session_id),
      cfd_(cfd),
      db_options_(db_options),
      mutable_cf_options_(mutable_cf_options),
      max_memtable_id_(max_memtable_id),
      file_options_(file_options),
      versions_(versions),
      db_mutex_(db_mutex),
      shutting_down_(shutting_down),
      existing_snapshots_(std::move(existing_snapshots)),
      earliest_write_conflict_snapshot_(earliest_write_conflict_snapshot),
      snapshot_checker_(snapshot_checker),
      job_context_(job_context),
      log_buffer_(log_buffer),
      db_directory_(db_directory),
      output_file_directory_(output_file_directory),
      output_compression_(output_compression),
      stats_(stats),
      event_logger_(event_logger),
      measure_io_stats_(measure_io_stats),
      sync_output_directory_(sync_output_directory),
      write_manifest_(write_manifest),
      edit_(nullptr),
      base_(nullptr),
      pick_memtable_called(false),
      thread_pri_(thread_pri),
      io_tracer_(io_tracer),
      clock_(db_options_.clock),
      full_history_ts_low_(std::move(full_history_ts_low)),
      blob_callback_(blob_callback) {
  // Update the thread status to indicate flush.
  ReportStartedFlush();
  TEST_SYNC_POINT("FlushJob::FlushJob()");
}

FlushJob::~FlushJob() {
  io_status_.PermitUncheckedError();
  ThreadStatusUtil::ResetThreadStatus();
}

void FlushJob::ReportStartedFlush() {
  ThreadStatusUtil::SetColumnFamily(cfd_, cfd_->ioptions()->env,
                                    db_options_.enable_thread_tracking);
  ThreadStatusUtil::SetThreadOperation(ThreadStatus::OP_FLUSH);
  ThreadStatusUtil::SetThreadOperationProperty(
      ThreadStatus::COMPACTION_JOB_ID,
      job_context_->job_id);
  IOSTATS_RESET(bytes_written);
}

void FlushJob::ReportFlushInputSize(const autovector<MemTable*>& mems) {
  uint64_t input_size = 0;
  for (auto* mem : mems) {
    input_size += mem->ApproximateMemoryUsage();
  }
  ThreadStatusUtil::IncreaseThreadOperationProperty(
      ThreadStatus::FLUSH_BYTES_MEMTABLES,
      input_size);
}

void FlushJob::RecordFlushIOStats() {
  RecordTick(stats_, FLUSH_WRITE_BYTES, IOSTATS(bytes_written));
  ThreadStatusUtil::IncreaseThreadOperationProperty(
      ThreadStatus::FLUSH_BYTES_WRITTEN, IOSTATS(bytes_written));
  IOSTATS_RESET(bytes_written);
}
void FlushJob::PickMemTable() {
  db_mutex_->AssertHeld();
  assert(!pick_memtable_called);
  pick_memtable_called = true;
  // Save the contents of the earliest memtable as a new Table
  cfd_->imm()->PickMemtablesToFlush(max_memtable_id_, &mems_);
  if (mems_.empty()) {
    return;
  }

  ReportFlushInputSize(mems_);

  // entries mems are (implicitly) sorted in ascending order by their created
  // time. We will use the first memtable's `edit` to keep the meta info for
  // this flush.
  MemTable* m = mems_[0];
  edit_ = m->GetEdits();
  edit_->SetPrevLogNumber(0);
  // SetLogNumber(log_num) indicates logs with number smaller than log_num
  // will no longer be picked up for recovery.
  edit_->SetLogNumber(mems_.back()->GetNextLogNumber());
  edit_->SetColumnFamily(cfd_->GetID());

  // path 0 for level 0 file.
  meta_.fd = FileDescriptor(versions_->NewFileNumber(), 0, 0);

  // If mempurge feature is activated, keep track of any potential
  // memtables coming from a previous mempurge operation.
  // Used for mempurge policy.
  if (db_options_.experimental_allow_mempurge) {
    contains_mempurge_outcome_ = false;
    for (MemTable* mt : mems_) {
      if (cfd_->imm()->IsMemPurgeOutput(mt->GetID())) {
        contains_mempurge_outcome_ = true;
        break;
      }
    }
  }

  base_ = cfd_->current();
  base_->Ref();  // it is likely that we do not need this reference
}

Status FlushJob::Run(LogsWithPrepTracker* prep_tracker,
                     FileMetaData* file_meta) {
  TEST_SYNC_POINT("FlushJob::Start");
  db_mutex_->AssertHeld();
  assert(pick_memtable_called);
  AutoThreadOperationStageUpdater stage_run(
      ThreadStatus::STAGE_FLUSH_RUN);
  if (mems_.empty()) {
    ROCKS_LOG_BUFFER(log_buffer_, "[%s] Nothing in memtable to flush",
                     cfd_->GetName().c_str());
    return Status::OK();
  }

  // I/O measurement variables
  PerfLevel prev_perf_level = PerfLevel::kEnableTime;
  uint64_t prev_write_nanos = 0;
  uint64_t prev_fsync_nanos = 0;
  uint64_t prev_range_sync_nanos = 0;
  uint64_t prev_prepare_write_nanos = 0;
  uint64_t prev_cpu_write_nanos = 0;
  uint64_t prev_cpu_read_nanos = 0;
  if (measure_io_stats_) {
    prev_perf_level = GetPerfLevel();
    SetPerfLevel(PerfLevel::kEnableTime);
    prev_write_nanos = IOSTATS(write_nanos);
    prev_fsync_nanos = IOSTATS(fsync_nanos);
    prev_range_sync_nanos = IOSTATS(range_sync_nanos);
    prev_prepare_write_nanos = IOSTATS(prepare_write_nanos);
    prev_cpu_write_nanos = IOSTATS(cpu_write_nanos);
    prev_cpu_read_nanos = IOSTATS(cpu_read_nanos);
  }
  Status mempurge_s = Status::NotFound("No MemPurge.");
  if (db_options_.experimental_allow_mempurge &&
      (cfd_->GetFlushReason() == FlushReason::kWriteBufferFull) &&
      (!mems_.empty()) && MemPurgeDecider()) {
    mempurge_s = MemPurge();
    if (!mempurge_s.ok()) {
      // Mempurge is typically aborted when the new_mem output memtable
      // is filled at more than XX % capacity (currently: 60%).
      if (mempurge_s.IsAborted()) {
        ROCKS_LOG_INFO(db_options_.info_log, "Mempurge process aborted: %s\n",
                       mempurge_s.ToString().c_str());
      } else {
        // However the mempurge process can also fail for
        // other reasons (eg: new_mem->Add() fails).
        ROCKS_LOG_WARN(db_options_.info_log, "Mempurge process failed: %s\n",
                       mempurge_s.ToString().c_str());
      }
    }
  }
  Status s;
  if (mempurge_s.ok()) {
    base_->Unref();
    s = Status::OK();
  } else {
    // This will release and re-acquire the mutex.
    s = WriteLevel0Table();
  }

  if (s.ok() && cfd_->IsDropped()) {
    s = Status::ColumnFamilyDropped("Column family dropped during compaction");
  }
  if ((s.ok() || s.IsColumnFamilyDropped()) &&
      shutting_down_->load(std::memory_order_acquire)) {
    s = Status::ShutdownInProgress("Database shutdown");
  }

  if (!s.ok()) {
    cfd_->imm()->RollbackMemtableFlush(mems_, meta_.fd.GetNumber());
  } else if (write_manifest_) {
    TEST_SYNC_POINT("FlushJob::InstallResults");
    // Replace immutable memtable with the generated Table
    IOStatus tmp_io_s;
    s = cfd_->imm()->TryInstallMemtableFlushResults(
        cfd_, mutable_cf_options_, mems_, prep_tracker, versions_, db_mutex_,
        meta_.fd.GetNumber(), &job_context_->memtables_to_free, db_directory_,
        log_buffer_, &committed_flush_jobs_info_, &tmp_io_s,
        !(mempurge_s.ok()) /* write_edit : true if no mempurge happened (or if aborted),
                              but 'false' if mempurge successful: no new min log number
                              or new level 0 file path to write to manifest. */);
    if (!tmp_io_s.ok()) {
      io_status_ = tmp_io_s;
    }
  }

  if (s.ok() && file_meta != nullptr) {
    *file_meta = meta_;
  }
  RecordFlushIOStats();

  // When measure_io_stats_ is true, the default 512 bytes is not enough.
  auto stream = event_logger_->LogToBuffer(log_buffer_, 1024);
  stream << "job" << job_context_->job_id << "event"
         << "flush_finished";
  stream << "output_compression"
         << CompressionTypeToString(output_compression_);
  stream << "lsm_state";
  stream.StartArray();
  auto vstorage = cfd_->current()->storage_info();
  for (int level = 0; level < vstorage->num_levels(); ++level) {
    stream << vstorage->NumLevelFiles(level);
  }
  stream.EndArray();

  const auto& blob_files = vstorage->GetBlobFiles();
  if (!blob_files.empty()) {
    stream << "blob_file_head" << blob_files.begin()->first;
    stream << "blob_file_tail" << blob_files.rbegin()->first;
  }

  stream << "immutable_memtables" << cfd_->imm()->NumNotFlushed();

  if (measure_io_stats_) {
    if (prev_perf_level != PerfLevel::kEnableTime) {
      SetPerfLevel(prev_perf_level);
    }
    stream << "file_write_nanos" << (IOSTATS(write_nanos) - prev_write_nanos);
    stream << "file_range_sync_nanos"
           << (IOSTATS(range_sync_nanos) - prev_range_sync_nanos);
    stream << "file_fsync_nanos" << (IOSTATS(fsync_nanos) - prev_fsync_nanos);
    stream << "file_prepare_write_nanos"
           << (IOSTATS(prepare_write_nanos) - prev_prepare_write_nanos);
    stream << "file_cpu_write_nanos"
           << (IOSTATS(cpu_write_nanos) - prev_cpu_write_nanos);
    stream << "file_cpu_read_nanos"
           << (IOSTATS(cpu_read_nanos) - prev_cpu_read_nanos);
  }

  return s;
}

void FlushJob::Cancel() {
  db_mutex_->AssertHeld();
  assert(base_ != nullptr);
  base_->Unref();
}

Status FlushJob::MemPurge() {
  Status s;
  db_mutex_->AssertHeld();
  db_mutex_->Unlock();
  assert(!mems_.empty());

  // Measure purging time.
  const uint64_t start_micros = clock_->NowMicros();
  const uint64_t start_cpu_micros = clock_->CPUNanos() / 1000;

  MemTable* new_mem = nullptr;
  // For performance/log investigation purposes:
  // look at how much useful payload we harvest in the new_mem.
  // This value is then printed to the DB log.
  double new_mem_capacity = 0.0;

  // Create two iterators, one for the memtable data (contains
  // info from puts + deletes), and one for the memtable
  // Range Tombstones (from DeleteRanges).
  ReadOptions ro;
  ro.total_order_seek = true;
  Arena arena;
  std::vector<InternalIterator*> memtables;
  std::vector<std::unique_ptr<FragmentedRangeTombstoneIterator>>
      range_del_iters;
  for (MemTable* m : mems_) {
    memtables.push_back(m->NewIterator(ro, &arena));
    auto* range_del_iter = m->NewRangeTombstoneIterator(ro, kMaxSequenceNumber);
    if (range_del_iter != nullptr) {
      range_del_iters.emplace_back(range_del_iter);
    }
  }

  assert(!memtables.empty());
  SequenceNumber first_seqno = kMaxSequenceNumber;
  SequenceNumber earliest_seqno = kMaxSequenceNumber;
  // Pick first and earliest seqno as min of all first_seqno
  // and earliest_seqno of the mempurged memtables.
  for (const auto& mem : mems_) {
    first_seqno = mem->GetFirstSequenceNumber() < first_seqno
                      ? mem->GetFirstSequenceNumber()
                      : first_seqno;
    earliest_seqno = mem->GetEarliestSequenceNumber() < earliest_seqno
                         ? mem->GetEarliestSequenceNumber()
                         : earliest_seqno;
  }

  ScopedArenaIterator iter(
      NewMergingIterator(&(cfd_->internal_comparator()), memtables.data(),
                         static_cast<int>(memtables.size()), &arena));

  auto* ioptions = cfd_->ioptions();

  // Place iterator at the First (meaning most recent) key node.
  iter->SeekToFirst();

  std::unique_ptr<CompactionRangeDelAggregator> range_del_agg(
      new CompactionRangeDelAggregator(&(cfd_->internal_comparator()),
                                       existing_snapshots_));
  for (auto& rd_iter : range_del_iters) {
    range_del_agg->AddTombstones(std::move(rd_iter));
  }

  // If there is valid data in the memtable,
  // or at least range tombstones, copy over the info
  // to the new memtable.
  if (iter->Valid() || !range_del_agg->IsEmpty()) {
    // MaxSize is the size of a memtable.
    size_t maxSize = mutable_cf_options_.write_buffer_size;
    std::unique_ptr<CompactionFilter> compaction_filter;
    if (ioptions->compaction_filter_factory != nullptr &&
        ioptions->compaction_filter_factory->ShouldFilterTableFileCreation(
            TableFileCreationReason::kFlush)) {
      CompactionFilter::Context ctx;
      ctx.is_full_compaction = false;
      ctx.is_manual_compaction = false;
      ctx.column_family_id = cfd_->GetID();
      ctx.reason = TableFileCreationReason::kFlush;
      compaction_filter =
          ioptions->compaction_filter_factory->CreateCompactionFilter(ctx);
      if (compaction_filter != nullptr &&
          !compaction_filter->IgnoreSnapshots()) {
        s = Status::NotSupported(
            "CompactionFilter::IgnoreSnapshots() = false is not supported "
            "anymore.");
        return s;
      }
    }

    new_mem = new MemTable((cfd_->internal_comparator()), *(cfd_->ioptions()),
                           mutable_cf_options_, cfd_->write_buffer_mgr(),
                           earliest_seqno, cfd_->GetID());
    assert(new_mem != nullptr);

    Env* env = db_options_.env;
    assert(env);
    MergeHelper merge(
        env, (cfd_->internal_comparator()).user_comparator(),
        (ioptions->merge_operator).get(), compaction_filter.get(),
        ioptions->logger, true /* internal key corruption is not ok */,
        existing_snapshots_.empty() ? 0 : existing_snapshots_.back(),
        snapshot_checker_);
    CompactionIterator c_iter(
        iter.get(), (cfd_->internal_comparator()).user_comparator(), &merge,
        kMaxSequenceNumber, &existing_snapshots_,
        earliest_write_conflict_snapshot_, snapshot_checker_, env,
        ShouldReportDetailedTime(env, ioptions->stats),
        true /* internal key corruption is not ok */, range_del_agg.get(),
        nullptr, ioptions->allow_data_in_errors,
        /*compaction=*/nullptr, compaction_filter.get(),
        /*shutting_down=*/nullptr,
        /*preserve_deletes_seqnum=*/0, /*manual_compaction_paused=*/nullptr,
        /*manual_compaction_canceled=*/nullptr, ioptions->info_log,
        &(cfd_->GetFullHistoryTsLow()));

    // Set earliest sequence number in the new memtable
    // to be equal to the earliest sequence number of the
    // memtable being flushed (See later if there is a need
    // to update this number!).
    new_mem->SetEarliestSequenceNumber(earliest_seqno);
    // Likewise for first seq number.
    new_mem->SetFirstSequenceNumber(first_seqno);
    SequenceNumber new_first_seqno = kMaxSequenceNumber;

    c_iter.SeekToFirst();

    // Key transfer
    for (; c_iter.Valid(); c_iter.Next()) {
      const ParsedInternalKey ikey = c_iter.ikey();
      const Slice value = c_iter.value();
      new_first_seqno =
          ikey.sequence < new_first_seqno ? ikey.sequence : new_first_seqno;

      // Should we update "OldestKeyTime" ???? -> timestamp appear
      // to still be an "experimental" feature.
      s = new_mem->Add(
          ikey.sequence, ikey.type, ikey.user_key, value,
          nullptr,   // KV protection info set as nullptr since it
                     // should only be useful for the first add to
                     // the original memtable.
          false,     // : allow concurrent_memtable_writes_
                     // Not seen as necessary for now.
          nullptr,   // get_post_process_info(m) must be nullptr
                     // when concurrent_memtable_writes is switched off.
          nullptr);  // hint, only used when concurrent_memtable_writes_
                     // is switched on.
      if (!s.ok()) {
        break;
      }

      // If new_mem has size greater than maxSize,
      // then rollback to regular flush operation,
      // and destroy new_mem.
      if (new_mem->ApproximateMemoryUsage() > maxSize) {
        s = Status::Aborted("Mempurge filled more than one memtable.");
        new_mem_capacity = 1.0;
        break;
      }
    }

    // Check status and propagate
    // potential error status from c_iter
    if (!s.ok()) {
      c_iter.status().PermitUncheckedError();
    } else if (!c_iter.status().ok()) {
      s = c_iter.status();
    }

    // Range tombstone transfer.
    if (s.ok()) {
      auto range_del_it = range_del_agg->NewIterator();
      for (range_del_it->SeekToFirst(); range_del_it->Valid();
           range_del_it->Next()) {
        auto tombstone = range_del_it->Tombstone();
        new_first_seqno =
            tombstone.seq_ < new_first_seqno ? tombstone.seq_ : new_first_seqno;
        s = new_mem->Add(
            tombstone.seq_,        // Sequence number
            kTypeRangeDeletion,    // KV type
            tombstone.start_key_,  // Key is start key.
            tombstone.end_key_,    // Value is end key.
            nullptr,               // KV protection info set as nullptr since it
                                   // should only be useful for the first add to
                                   // the original memtable.
            false,                 // : allow concurrent_memtable_writes_
                                   // Not seen as necessary for now.
            nullptr,               // get_post_process_info(m) must be nullptr
                      // when concurrent_memtable_writes is switched off.
            nullptr);  // hint, only used when concurrent_memtable_writes_
                       // is switched on.

        if (!s.ok()) {
          break;
        }

        // If new_mem has size greater than maxSize,
        // then rollback to regular flush operation,
        // and destroy new_mem.
        if (new_mem->ApproximateMemoryUsage() > maxSize) {
          s = Status::Aborted(Slice("Mempurge filled more than one memtable."));
          new_mem_capacity = 1.0;
          break;
        }
      }
    }

    // If everything happened smoothly and new_mem contains valid data,
    // decide if it is flushed to storage or kept in the imm()
    // memtable list (memory).
    if (s.ok() && (new_first_seqno != kMaxSequenceNumber)) {
      // Rectify the first sequence number, which (unlike the earliest seq
      // number) needs to be present in the new memtable.
      new_mem->SetFirstSequenceNumber(new_first_seqno);

      // The new_mem is added to the list of immutable memtables
      // only if it filled at less than 100% capacity and isn't flagged
      // as in need of being flushed.
      if (new_mem->ApproximateMemoryUsage() < maxSize &&
          !(new_mem->ShouldFlushNow())) {
        db_mutex_->Lock();
        uint64_t new_mem_id = mems_[0]->GetID();
        // Copy lowest memtable ID
        // House keeping work.
        for (MemTable* mt : mems_) {
          new_mem_id = mt->GetID() < new_mem_id ? mt->GetID() : new_mem_id;
          // Note: if m is not a previous mempurge output memtable,
          // nothing happens.
          cfd_->imm()->RemoveMemPurgeOutputID(mt->GetID());
        }
        new_mem->SetID(new_mem_id);
        cfd_->imm()->AddMemPurgeOutputID(new_mem_id);
        // This addition will not trigger another flush, because
        // we do not call SchedulePendingFlush().
        cfd_->imm()->Add(new_mem, &job_context_->memtables_to_free);
        new_mem_capacity = (new_mem->ApproximateMemoryUsage()) * 1.0 /
                           mutable_cf_options_.write_buffer_size;
        new_mem->Ref();
        db_mutex_->Unlock();
      } else {
        s = Status::Aborted(Slice("Mempurge filled more than one memtable."));
        new_mem_capacity = 1.0;
        if (new_mem) {
          job_context_->memtables_to_free.push_back(new_mem);
        }
      }
    } else {
      // In this case, the newly allocated new_mem is empty.
      assert(new_mem != nullptr);
      job_context_->memtables_to_free.push_back(new_mem);
    }
  }

  // Reacquire the mutex for WriteLevel0 function.
  db_mutex_->Lock();

  // If mempurge successful, don't write input tables to level0,
  // but write any full output table to level0.
  if (s.ok()) {
    TEST_SYNC_POINT("DBImpl::FlushJob:MemPurgeSuccessful");
  } else {
    TEST_SYNC_POINT("DBImpl::FlushJob:MemPurgeUnsuccessful");
  }
  const uint64_t micros = clock_->NowMicros() - start_micros;
  const uint64_t cpu_micros = clock_->CPUNanos() / 1000 - start_cpu_micros;
  ROCKS_LOG_INFO(db_options_.info_log,
                 "[%s] [JOB %d] Mempurge lasted %" PRIu64
                 " microseconds, and %" PRIu64
                 " cpu "
                 "microseconds. Status is %s ok. Perc capacity: %f\n",
                 cfd_->GetName().c_str(), job_context_->job_id, micros,
                 cpu_micros, s.ok() ? "" : "not", new_mem_capacity);

  return s;
}

bool FlushJob::MemPurgeDecider() {
  MemPurgePolicy policy = db_options_.experimental_mempurge_policy;
  if (policy == MemPurgePolicy::kAlways) {
    return true;
  } else if (policy == MemPurgePolicy::kAlternate) {
    // Note: if at least one of the flushed memtables is
    // an output of a previous mempurge process, then flush
    // to storage.
    return !(contains_mempurge_outcome_);
  }
  return false;
}

Status FlushJob::WriteLevel0Table() {
  AutoThreadOperationStageUpdater stage_updater(
      ThreadStatus::STAGE_FLUSH_WRITE_L0);
  db_mutex_->AssertHeld();
  const uint64_t start_micros = clock_->NowMicros();
  const uint64_t start_cpu_micros = clock_->CPUNanos() / 1000;
  Status s;

  std::vector<BlobFileAddition> blob_file_additions;

  {
    auto write_hint = cfd_->CalculateSSTWriteHint(0);
    db_mutex_->Unlock();
    if (log_buffer_) {
      log_buffer_->FlushBufferToLog();
    }
    // memtables and range_del_iters store internal iterators over each data
    // memtable and its associated range deletion memtable, respectively, at
    // corresponding indexes.
    std::vector<InternalIterator*> memtables;
    std::vector<std::unique_ptr<FragmentedRangeTombstoneIterator>>
        range_del_iters;
    ReadOptions ro;
    ro.total_order_seek = true;
    Arena arena;
    uint64_t total_num_entries = 0, total_num_deletes = 0;
    uint64_t total_data_size = 0;
    size_t total_memory_usage = 0;
    for (MemTable* m : mems_) {
      ROCKS_LOG_INFO(
          db_options_.info_log,
          "[%s] [JOB %d] Flushing memtable with next log file: %" PRIu64 "\n",
          cfd_->GetName().c_str(), job_context_->job_id, m->GetNextLogNumber());
      memtables.push_back(m->NewIterator(ro, &arena));
      auto* range_del_iter =
          m->NewRangeTombstoneIterator(ro, kMaxSequenceNumber);
      if (range_del_iter != nullptr) {
        range_del_iters.emplace_back(range_del_iter);
      }
      total_num_entries += m->num_entries();
      total_num_deletes += m->num_deletes();
      total_data_size += m->get_data_size();
      total_memory_usage += m->ApproximateMemoryUsage();
    }

    event_logger_->Log() << "job" << job_context_->job_id << "event"
                         << "flush_started"
                         << "num_memtables" << mems_.size() << "num_entries"
                         << total_num_entries << "num_deletes"
                         << total_num_deletes << "total_data_size"
                         << total_data_size << "memory_usage"
                         << total_memory_usage << "flush_reason"
                         << GetFlushReasonString(cfd_->GetFlushReason());

    {
      ScopedArenaIterator iter(
          NewMergingIterator(&cfd_->internal_comparator(), memtables.data(),
                             static_cast<int>(memtables.size()), &arena));
      ROCKS_LOG_INFO(db_options_.info_log,
                     "[%s] [JOB %d] Level-0 flush table #%" PRIu64 ": started",
                     cfd_->GetName().c_str(), job_context_->job_id,
                     meta_.fd.GetNumber());

      TEST_SYNC_POINT_CALLBACK("FlushJob::WriteLevel0Table:output_compression",
                               &output_compression_);
      int64_t _current_time = 0;
      auto status = clock_->GetCurrentTime(&_current_time);
      // Safe to proceed even if GetCurrentTime fails. So, log and proceed.
      if (!status.ok()) {
        ROCKS_LOG_WARN(
            db_options_.info_log,
            "Failed to get current time to populate creation_time property. "
            "Status: %s",
            status.ToString().c_str());
      }
      const uint64_t current_time = static_cast<uint64_t>(_current_time);

      uint64_t oldest_key_time =
          mems_.front()->ApproximateOldestKeyTime();

      // It's not clear whether oldest_key_time is always available. In case
      // it is not available, use current_time.
      uint64_t oldest_ancester_time = std::min(current_time, oldest_key_time);

      TEST_SYNC_POINT_CALLBACK(
          "FlushJob::WriteLevel0Table:oldest_ancester_time",
          &oldest_ancester_time);
      meta_.oldest_ancester_time = oldest_ancester_time;

      meta_.file_creation_time = current_time;

      uint64_t creation_time = (cfd_->ioptions()->compaction_style ==
                                CompactionStyle::kCompactionStyleFIFO)
                                   ? current_time
                                   : meta_.oldest_ancester_time;

      uint64_t num_input_entries = 0;
      uint64_t memtable_payload_bytes = 0;
      uint64_t memtable_garbage_bytes = 0;
      IOStatus io_s;
      const std::string* const full_history_ts_low =
          (full_history_ts_low_.empty()) ? nullptr : &full_history_ts_low_;
      TableBuilderOptions tboptions(
          *cfd_->ioptions(), mutable_cf_options_, cfd_->internal_comparator(),
          cfd_->int_tbl_prop_collector_factories(), output_compression_,
          mutable_cf_options_.compression_opts, cfd_->GetID(), cfd_->GetName(),
          0 /* level */, false /* is_bottommost */,
          TableFileCreationReason::kFlush, creation_time, oldest_key_time,
          current_time, db_id_, db_session_id_, 0 /* target_file_size */,
          meta_.fd.GetNumber());
      s = BuildTable(
          dbname_, versions_, db_options_, tboptions, file_options_,
          cfd_->table_cache(), iter.get(), std::move(range_del_iters), &meta_,
          &blob_file_additions, existing_snapshots_,
          earliest_write_conflict_snapshot_, snapshot_checker_,
          mutable_cf_options_.paranoid_file_checks, cfd_->internal_stats(),
          &io_s, io_tracer_, event_logger_, job_context_->job_id, Env::IO_HIGH,
          &table_properties_, write_hint, full_history_ts_low, blob_callback_,
          &num_input_entries, &memtable_payload_bytes, &memtable_garbage_bytes);
      if (!io_s.ok()) {
        io_status_ = io_s;
      }
      if (num_input_entries != total_num_entries && s.ok()) {
        std::string msg = "Expected " + ToString(total_num_entries) +
                          " entries in memtables, but read " +
                          ToString(num_input_entries);
        ROCKS_LOG_WARN(db_options_.info_log, "[%s] [JOB %d] Level-0 flush %s",
                       cfd_->GetName().c_str(), job_context_->job_id,
                       msg.c_str());
        if (db_options_.flush_verify_memtable_count) {
          s = Status::Corruption(msg);
        }
      }
      if (tboptions.reason == TableFileCreationReason::kFlush) {
        TEST_SYNC_POINT("DBImpl::FlushJob:Flush");
        RecordTick(stats_, MEMTABLE_PAYLOAD_BYTES_AT_FLUSH,
                   memtable_payload_bytes);
        RecordTick(stats_, MEMTABLE_GARBAGE_BYTES_AT_FLUSH,
                   memtable_garbage_bytes);
      }
      LogFlush(db_options_.info_log);
    }
    ROCKS_LOG_INFO(db_options_.info_log,
                   "[%s] [JOB %d] Level-0 flush table #%" PRIu64 ": %" PRIu64
                   " bytes %s"
                   "%s",
                   cfd_->GetName().c_str(), job_context_->job_id,
                   meta_.fd.GetNumber(), meta_.fd.GetFileSize(),
                   s.ToString().c_str(),
                   meta_.marked_for_compaction ? " (needs compaction)" : "");

    if (s.ok() && output_file_directory_ != nullptr && sync_output_directory_) {
      s = output_file_directory_->Fsync(IOOptions(), nullptr);
    }
    TEST_SYNC_POINT_CALLBACK("FlushJob::WriteLevel0Table", &mems_);
    db_mutex_->Lock();
  }
  base_->Unref();

  // Note that if file_size is zero, the file has been deleted and
  // should not be added to the manifest.
  const bool has_output = meta_.fd.GetFileSize() > 0;

  if (s.ok() && has_output) {
    TEST_SYNC_POINT("DBImpl::FlushJob:SSTFileCreated");
    // if we have more than 1 background thread, then we cannot
    // insert files directly into higher levels because some other
    // threads could be concurrently producing compacted files for
    // that key range.
    // Add file to L0
    edit_->AddFile(0 /* level */, meta_.fd.GetNumber(), meta_.fd.GetPathId(),
                   meta_.fd.GetFileSize(), meta_.smallest, meta_.largest,
                   meta_.fd.smallest_seqno, meta_.fd.largest_seqno,
                   meta_.marked_for_compaction, meta_.oldest_blob_file_number,
                   meta_.oldest_ancester_time, meta_.file_creation_time,
                   meta_.file_checksum, meta_.file_checksum_func_name);

    edit_->SetBlobFileAdditions(std::move(blob_file_additions));
  }
#ifndef ROCKSDB_LITE
  // Piggyback FlushJobInfo on the first first flushed memtable.
  mems_[0]->SetFlushJobInfo(GetFlushJobInfo());
#endif  // !ROCKSDB_LITE

  // Note that here we treat flush as level 0 compaction in internal stats
  InternalStats::CompactionStats stats(CompactionReason::kFlush, 1);
  const uint64_t micros = clock_->NowMicros() - start_micros;
  const uint64_t cpu_micros = clock_->CPUNanos() / 1000 - start_cpu_micros;
  stats.micros = micros;
  stats.cpu_micros = cpu_micros;

  ROCKS_LOG_INFO(db_options_.info_log,
                 "[%s] [JOB %d] Flush lasted %" PRIu64
                 " microseconds, and %" PRIu64 " cpu microseconds.\n",
                 cfd_->GetName().c_str(), job_context_->job_id, micros,
                 cpu_micros);

  if (has_output) {
    stats.bytes_written = meta_.fd.GetFileSize();
    stats.num_output_files = 1;
  }

  const auto& blobs = edit_->GetBlobFileAdditions();
  for (const auto& blob : blobs) {
    stats.bytes_written_blob += blob.GetTotalBlobBytes();
  }

  stats.num_output_files_blob = static_cast<int>(blobs.size());

  if (db_options_.experimental_allow_mempurge && s.ok()) {
    // The db_mutex is held at this point.
    for (MemTable* mt : mems_) {
      // Note: if m is not a previous mempurge output memtable,
      // nothing happens here.
      cfd_->imm()->RemoveMemPurgeOutputID(mt->GetID());
    }
  }

  RecordTimeToHistogram(stats_, FLUSH_TIME, stats.micros);
  cfd_->internal_stats()->AddCompactionStats(0 /* level */, thread_pri_, stats);
  cfd_->internal_stats()->AddCFStats(
      InternalStats::BYTES_FLUSHED,
      stats.bytes_written + stats.bytes_written_blob);
  RecordFlushIOStats();

  return s;
}

#ifndef ROCKSDB_LITE
std::unique_ptr<FlushJobInfo> FlushJob::GetFlushJobInfo() const {
  db_mutex_->AssertHeld();
  std::unique_ptr<FlushJobInfo> info(new FlushJobInfo{});
  info->cf_id = cfd_->GetID();
  info->cf_name = cfd_->GetName();

  const uint64_t file_number = meta_.fd.GetNumber();
  info->file_path =
      MakeTableFileName(cfd_->ioptions()->cf_paths[0].path, file_number);
  info->file_number = file_number;
  info->oldest_blob_file_number = meta_.oldest_blob_file_number;
  info->thread_id = db_options_.env->GetThreadID();
  info->job_id = job_context_->job_id;
  info->smallest_seqno = meta_.fd.smallest_seqno;
  info->largest_seqno = meta_.fd.largest_seqno;
  info->table_properties = table_properties_;
  info->flush_reason = cfd_->GetFlushReason();
  return info;
}
#endif  // !ROCKSDB_LITE

}  // namespace ROCKSDB_NAMESPACE
