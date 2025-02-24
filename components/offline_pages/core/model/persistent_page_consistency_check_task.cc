// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/offline_pages/core/model/persistent_page_consistency_check_task.h"

#include <string>
#include <utility>
#include <vector>

#include "base/bind.h"
#include "base/files/file_util.h"
#include "base/metrics/histogram_macros.h"
#include "base/numerics/safe_conversions.h"
#include "components/offline_pages/core/archive_manager.h"
#include "components/offline_pages/core/client_policy_controller.h"
#include "components/offline_pages/core/model/delete_page_task.h"
#include "components/offline_pages/core/model/get_pages_task.h"
#include "components/offline_pages/core/offline_page_client_policy.h"
#include "components/offline_pages/core/offline_page_metadata_store.h"
#include "components/offline_pages/core/offline_store_utils.h"
#include "components/offline_pages/core/page_criteria.h"
#include "sql/database.h"
#include "sql/statement.h"
#include "sql/transaction.h"

namespace base {
class Time;
}  // namespace base

namespace offline_pages {

namespace {

const base::TimeDelta kExpireThreshold = base::TimeDelta::FromDays(365);

std::vector<OfflinePageItem> GetPersistentPages(
    const ClientPolicyController* policy_controller,
    sql::Database* db) {
  PageCriteria criteria;
  criteria.lifetime_type = LifetimeType::PERSISTENT;
  return std::move(
      GetPagesTask::ReadPagesWithCriteriaSync(policy_controller, criteria, db)
          .pages);
}

bool SetItemsFileMissingTimeSync(const std::vector<int64_t>& item_ids,
                                 base::Time missing_time,
                                 sql::Database* db) {
  static const char kSql[] = "UPDATE OR IGNORE offlinepages_v1"
                             " SET file_missing_time=?"
                             " WHERE offline_id=?";

  for (auto offline_id : item_ids) {
    sql::Statement statement(db->GetCachedStatement(SQL_FROM_HERE, kSql));
    statement.BindInt64(0, store_utils::ToDatabaseTime(missing_time));
    statement.BindInt64(1, offline_id);
    if (!statement.Run())
      return false;
  }
  return true;
}

bool MarkPagesAsMissing(const std::vector<int64_t>& ids_of_missing_pages,
                        base::Time missing_time,
                        sql::Database* db) {
  return SetItemsFileMissingTimeSync(ids_of_missing_pages, missing_time, db);
}

bool MarkPagesAsReappeared(const std::vector<int64_t>& ids_of_reappeared_pages,
                           sql::Database* db) {
  return SetItemsFileMissingTimeSync(ids_of_reappeared_pages, base::Time(), db);
}

PersistentPageConsistencyCheckTask::CheckResult
PersistentPageConsistencyCheckSync(
    OfflinePageMetadataStore* store,
    const base::FilePath& private_dir,
    const base::FilePath& public_dir,
    const ClientPolicyController* policy_controller,
    base::Time check_time,
    sql::Database* db) {
  std::vector<int64_t> download_ids_of_deleted_pages;

  sql::Transaction transaction(db);
  if (!transaction.Begin())
    return {SyncOperationResult::TRANSACTION_BEGIN_ERROR,
            download_ids_of_deleted_pages};

  std::vector<OfflinePageItem> persistent_page_infos =
      GetPersistentPages(policy_controller, db);

  std::vector<int64_t> pages_found_missing;
  std::vector<int64_t> pages_reappeared;
  std::vector<int64_t> page_ids_to_delete;
  for (const OfflinePageItem& item : persistent_page_infos) {
    if (base::PathExists(item.file_path)) {
      if (item.file_missing_time != base::Time())
        pages_reappeared.push_back(item.offline_id);
    } else {
      if (item.file_missing_time == base::Time()) {
        pages_found_missing.push_back(item.offline_id);
      } else {
        if (check_time - item.file_missing_time > kExpireThreshold) {
          page_ids_to_delete.push_back(item.offline_id);
          download_ids_of_deleted_pages.push_back(item.system_download_id);
        }
      }
    }
  }

  if (!DeletePageTask::DeletePagesFromDbSync(page_ids_to_delete, db) ||
      !MarkPagesAsMissing(pages_found_missing, check_time, db) ||
      !MarkPagesAsReappeared(pages_reappeared, db)) {
    return {SyncOperationResult::DB_OPERATION_ERROR,
            download_ids_of_deleted_pages};
  }

  if (page_ids_to_delete.size() > 0) {
    UMA_HISTOGRAM_COUNTS_1M(
        "OfflinePages.ConsistencyCheck.Persistent.ExpiredEntryCount",
        base::saturated_cast<int32_t>(page_ids_to_delete.size()));
  }
  if (pages_found_missing.size() > 0) {
    UMA_HISTOGRAM_COUNTS_1M(
        "OfflinePages.ConsistencyCheck.Persistent.MissingFileCount",
        base::saturated_cast<int32_t>(pages_found_missing.size()));
  }
  if (pages_reappeared.size() > 0) {
    UMA_HISTOGRAM_COUNTS_1M(
        "OfflinePages.ConsistencyCheck.Persistent.ReappearedFileCount",
        base::saturated_cast<int32_t>(pages_reappeared.size()));
  }

  if (!transaction.Commit())
    return {SyncOperationResult::TRANSACTION_COMMIT_ERROR,
            download_ids_of_deleted_pages};

  return {SyncOperationResult::SUCCESS, download_ids_of_deleted_pages};
}

}  // namespace

PersistentPageConsistencyCheckTask::CheckResult::CheckResult() = default;

PersistentPageConsistencyCheckTask::CheckResult::CheckResult(
    SyncOperationResult result,
    const std::vector<int64_t>& system_download_ids)
    : result(result), download_ids_of_deleted_pages(system_download_ids) {}

PersistentPageConsistencyCheckTask::CheckResult::CheckResult(
    const CheckResult& other) = default;

PersistentPageConsistencyCheckTask::CheckResult&
PersistentPageConsistencyCheckTask::CheckResult::operator=(
    const CheckResult& other) = default;

PersistentPageConsistencyCheckTask::CheckResult::~CheckResult() {}

PersistentPageConsistencyCheckTask::PersistentPageConsistencyCheckTask(
    OfflinePageMetadataStore* store,
    ArchiveManager* archive_manager,
    ClientPolicyController* policy_controller,
    base::Time check_time,
    PersistentPageConsistencyCheckCallback callback)
    : store_(store),
      archive_manager_(archive_manager),
      policy_controller_(policy_controller),
      check_time_(check_time),
      callback_(std::move(callback)),
      weak_ptr_factory_(this) {
  DCHECK(store_);
  DCHECK(archive_manager_);
  DCHECK(policy_controller_);
}

PersistentPageConsistencyCheckTask::~PersistentPageConsistencyCheckTask() =
    default;

void PersistentPageConsistencyCheckTask::Run() {
  store_->Execute(base::BindOnce(&PersistentPageConsistencyCheckSync, store_,
                                 archive_manager_->GetPrivateArchivesDir(),
                                 archive_manager_->GetPublicArchivesDir(),
                                 policy_controller_, check_time_),
                  base::BindOnce(&PersistentPageConsistencyCheckTask::
                                     OnPersistentPageConsistencyCheckDone,
                                 weak_ptr_factory_.GetWeakPtr()),
                  CheckResult{SyncOperationResult::INVALID_DB_CONNECTION, {}});
}

void PersistentPageConsistencyCheckTask::OnPersistentPageConsistencyCheckDone(
    CheckResult check_result) {
  UMA_HISTOGRAM_ENUMERATION("OfflinePages.ConsistencyCheck.Persistent.Result",
                            check_result.result);
  // If sync operation failed, invoke the callback with an empty list of
  // download ids.
  if (check_result.result != SyncOperationResult::SUCCESS) {
    std::move(callback_).Run(false, {});
  } else {
    std::move(callback_).Run(true, check_result.download_ids_of_deleted_pages);
  }
  TaskComplete();
}

}  // namespace offline_pages
