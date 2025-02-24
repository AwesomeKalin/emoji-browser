// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/indexed_db/leveldb/leveldb_database.h"

#include <inttypes.h>
#include <stdint.h>

#include <algorithm>
#include <cerrno>
#include <memory>
#include <utility>

#include "base/files/file.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/memory/ptr_util.h"
#include "base/metrics/histogram_macros.h"
#include "base/strings/string16.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_piece.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/system/sys_info.h"
#include "base/time/default_clock.h"
#include "base/trace_event/memory_dump_manager.h"
#include "base/trace_event/process_memory_dump.h"
#include "build/build_config.h"
#include "content/browser/indexed_db/indexed_db_class_factory.h"
#include "content/browser/indexed_db/indexed_db_reporting.h"
#include "content/browser/indexed_db/indexed_db_tracing.h"
#include "content/browser/indexed_db/leveldb/leveldb_comparator.h"
#include "content/browser/indexed_db/leveldb/leveldb_env.h"
#include "content/browser/indexed_db/leveldb/leveldb_iterator_impl.h"
#include "content/browser/indexed_db/leveldb/leveldb_write_batch.h"
#include "third_party/leveldatabase/env_chromium.h"
#include "third_party/leveldatabase/leveldb_chrome.h"
#include "third_party/leveldatabase/src/include/leveldb/db.h"
#include "third_party/leveldatabase/src/include/leveldb/slice.h"

using base::StringPiece;
using leveldb_env::DBTracker;

namespace content {

namespace {

// Forcing flushes to disk at the end of a transaction guarantees that the
// data hit disk, but drastically impacts throughput when the filesystem is
// busy with background compactions. Not syncing trades off reliability for
// performance. Note that background compactions which move data from the
// log to SSTs are always done with reliable writes.
//
// Sync writes are necessary on Windows for quota calculations; POSIX
// calculates file sizes correctly even when not synced to disk.
#if defined(OS_WIN)
const bool kSyncWrites = true;
#else
// TODO(dgrogan): Either remove the #if block or change this back to false.
// See http://crbug.com/338385.
const bool kSyncWrites = true;
#endif
}  // namespace

LevelDBSnapshot::LevelDBSnapshot(LevelDBDatabase* db)
    : db_(db->db()), snapshot_(db_->GetSnapshot()) {}

LevelDBSnapshot::~LevelDBSnapshot() {
  db_->ReleaseSnapshot(snapshot_);
}

// static
constexpr const size_t LevelDBDatabase::kDefaultMaxOpenIteratorsPerDatabase;

LevelDBDatabase::LevelDBDatabase(
    scoped_refptr<LevelDBState> level_db_state,
    indexed_db::LevelDBFactory* class_factory,
    scoped_refptr<base::SequencedTaskRunner> task_runner,
    size_t max_open_iterators)
    : level_db_state_(std::move(level_db_state)),
      class_factory_(class_factory),
      clock_(new base::DefaultClock()),
      iterator_lru_(max_open_iterators) {
  if (task_runner) {
    base::trace_event::MemoryDumpManager::GetInstance()
        ->RegisterDumpProviderWithSequencedTaskRunner(
            this, "IndexedDBBackingStore", std::move(task_runner),
            base::trace_event::MemoryDumpProvider::Options());
  }
  DCHECK(max_open_iterators);
}

LevelDBDatabase::~LevelDBDatabase() {
  LOCAL_HISTOGRAM_COUNTS_10000("Storage.IndexedDB.LevelDB.MaxIterators",
                               max_iterators_);
  base::trace_event::MemoryDumpManager::GetInstance()->UnregisterDumpProvider(
      this);
}

leveldb::Status LevelDBDatabase::Put(const StringPiece& key,
                                     std::string* value) {
  base::TimeTicks begin_time = base::TimeTicks::Now();

  leveldb::WriteOptions write_options;
  write_options.sync = kSyncWrites;

  const leveldb::Status s =
      db()->Put(write_options, leveldb_env::MakeSlice(key),
                leveldb_env::MakeSlice(*value));
  if (!s.ok())
    LOG(ERROR) << "LevelDB put failed: " << s.ToString();
  else
    UMA_HISTOGRAM_TIMES("WebCore.IndexedDB.LevelDB.PutTime",
                        base::TimeTicks::Now() - begin_time);
  last_modified_ = clock_->Now();
  return s;
}

leveldb::Status LevelDBDatabase::Remove(const StringPiece& key) {
  leveldb::WriteOptions write_options;
  write_options.sync = kSyncWrites;

  const leveldb::Status s =
      db()->Delete(write_options, leveldb_env::MakeSlice(key));
  if (!s.ok() && !s.IsNotFound())
    LOG(ERROR) << "LevelDB remove failed: " << s.ToString();
  last_modified_ = clock_->Now();
  return s;
}

leveldb::Status LevelDBDatabase::Get(const StringPiece& key,
                                     std::string* value,
                                     bool* found,
                                     const LevelDBSnapshot* snapshot) {
  *found = false;
  leveldb::ReadOptions read_options;
  read_options.verify_checksums = true;  // TODO(jsbell): Disable this if the
                                         // performance impact is too great.
  read_options.snapshot = snapshot ? snapshot->snapshot_ : nullptr;

  const leveldb::Status s =
      db()->Get(read_options, leveldb_env::MakeSlice(key), value);
  if (s.ok()) {
    *found = true;
    return s;
  }
  if (s.IsNotFound())
    return leveldb::Status::OK();
  indexed_db::ReportLevelDBError("WebCore.IndexedDB.LevelDBReadErrors", s);
  LOG(ERROR) << "LevelDB get failed: " << s.ToString();
  return s;
}

leveldb::Status LevelDBDatabase::Write(const LevelDBWriteBatch& write_batch) {
  base::TimeTicks begin_time = base::TimeTicks::Now();
  leveldb::WriteOptions write_options;
  write_options.sync = kSyncWrites;

  const leveldb::Status s =
      db()->Write(write_options, write_batch.write_batch_.get());
  if (!s.ok()) {
    indexed_db::ReportLevelDBError("WebCore.IndexedDB.LevelDBWriteErrors", s);
    LOG(ERROR) << "LevelDB write failed: " << s.ToString();
  } else {
    UMA_HISTOGRAM_TIMES("WebCore.IndexedDB.LevelDB.WriteTime",
                        base::TimeTicks::Now() - begin_time);
  }
  last_modified_ = clock_->Now();
  return s;
}

std::unique_ptr<LevelDBIterator> LevelDBDatabase::CreateIterator(
    const leveldb::ReadOptions& options) {
  num_iterators_++;
  max_iterators_ = std::max(max_iterators_, num_iterators_);
  // Iterator isn't added to lru cache until it is used, as memory isn't loaded
  // for the iterator until it's first Seek call.
  std::unique_ptr<leveldb::Iterator> i(db()->NewIterator(options));
  return std::unique_ptr<LevelDBIterator>(
      class_factory_->CreateIteratorImpl(std::move(i), this, options.snapshot));
}

void LevelDBDatabase::Compact(const base::StringPiece& start,
                              const base::StringPiece& stop) {
  IDB_TRACE("LevelDBDatabase::Compact");
  const leveldb::Slice start_slice = leveldb_env::MakeSlice(start);
  const leveldb::Slice stop_slice = leveldb_env::MakeSlice(stop);
  // NULL batch means just wait for earlier writes to be done
  db()->Write(leveldb::WriteOptions(), nullptr);
  db()->CompactRange(&start_slice, &stop_slice);
}

void LevelDBDatabase::CompactAll() {
  db()->CompactRange(nullptr, nullptr);
}

leveldb::ReadOptions LevelDBDatabase::DefaultReadOptions() {
  return DefaultReadOptions(nullptr);
}

leveldb::ReadOptions LevelDBDatabase::DefaultReadOptions(
    const LevelDBSnapshot* snapshot) {
  leveldb::ReadOptions read_options;
  read_options.verify_checksums = true;  // TODO(jsbell): Disable this if the
                                         // performance impact is too great.
  read_options.snapshot = snapshot ? snapshot->snapshot_ : nullptr;
  return read_options;
}

bool LevelDBDatabase::OnMemoryDump(
    const base::trace_event::MemoryDumpArgs& args,
    base::trace_event::ProcessMemoryDump* pmd) {
  if (!level_db_state_)
    return false;
  // All leveldb databases are already dumped by leveldb_env::DBTracker. Add
  // an edge to the existing database.
  auto* db_tracker_dump =
      leveldb_env::DBTracker::GetOrCreateAllocatorDump(pmd, db());
  if (!db_tracker_dump)
    return true;

  auto* db_dump = pmd->CreateAllocatorDump(
      base::StringPrintf("site_storage/index_db/db_0x%" PRIXPTR,
                         reinterpret_cast<uintptr_t>(db())));
  db_dump->AddScalar(base::trace_event::MemoryAllocatorDump::kNameSize,
                     base::trace_event::MemoryAllocatorDump::kUnitsBytes,
                     db_tracker_dump->GetSizeInternal());
  pmd->AddOwnershipEdge(db_dump->guid(), db_tracker_dump->guid());

  if (env() && leveldb_chrome::IsMemEnv(env())) {
    // All leveldb env's are already dumped by leveldb_env::DBTracker. Add
    // an edge to the existing env.
    auto* env_tracker_dump = DBTracker::GetOrCreateAllocatorDump(pmd, env());
    auto* env_dump = pmd->CreateAllocatorDump(
        base::StringPrintf("site_storage/index_db/memenv_0x%" PRIXPTR,
                           reinterpret_cast<uintptr_t>(env())));
    env_dump->AddScalar(base::trace_event::MemoryAllocatorDump::kNameSize,
                        base::trace_event::MemoryAllocatorDump::kUnitsBytes,
                        env_tracker_dump->GetSizeInternal());
    pmd->AddOwnershipEdge(env_dump->guid(), env_tracker_dump->guid());
  }

  // Dumps in BACKGROUND mode can only have whitelisted strings (and there are
  // currently none) so return early.
  if (args.level_of_detail ==
      base::trace_event::MemoryDumpLevelOfDetail::BACKGROUND) {
    return true;
  }

  db_dump->AddString("file_name", "", file_name_for_tracing);

  return true;
}

void LevelDBDatabase::SetClockForTesting(std::unique_ptr<base::Clock> clock) {
  clock_ = std::move(clock);
}

std::unique_ptr<leveldb::Iterator> LevelDBDatabase::CreateLevelDBIterator(
    const leveldb::Snapshot* snapshot) {
  leveldb::ReadOptions read_options;
  read_options.verify_checksums = true;
  read_options.snapshot = snapshot;
  return base::WrapUnique(db()->NewIterator(read_options));
}

LevelDBDatabase::DetachIteratorOnDestruct::~DetachIteratorOnDestruct() {
  if (it_)
    it_->Detach();
}

void LevelDBDatabase::OnIteratorUsed(LevelDBIterator* iter) {
  // This line updates the LRU if the item exists.
  if (iterator_lru_.Get(iter) != iterator_lru_.end())
    return;
  DetachIteratorOnDestruct purger(iter);
  iterator_lru_.Put(iter, std::move(purger));
}

void LevelDBDatabase::OnIteratorDestroyed(LevelDBIterator* iter) {
  DCHECK_GT(num_iterators_, 0u);
  --num_iterators_;
  auto it = iterator_lru_.Peek(iter);
  if (it == iterator_lru_.end())
    return;
  iterator_lru_.Erase(it);
}

}  // namespace content
