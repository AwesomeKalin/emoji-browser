// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/offline_pages/core/model/clear_storage_task.h"

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/bind.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/metrics/histogram_macros.h"
#include "base/time/time.h"
#include "base/trace_event/trace_event.h"
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

namespace offline_pages {

using ClearStorageResult = ClearStorageTask::ClearStorageResult;

namespace {

// Maximum % of total available storage that will be occupied by offline pages
// before a storage clearup.
const double kOfflinePageStorageLimit = 0.3;
// The target % of storage usage we try to reach below when expiring pages.
const double kOfflinePageStorageClearThreshold = 0.1;

class PageClearCriteria {
 public:
  PageClearCriteria(const ClientPolicyController* policy_controller,
                    base::Time start_time,
                    const ArchiveManager::StorageStats& stats)
      : policy_controller_(policy_controller),
        start_time_(start_time),
        stats_(stats) {}

  // Returns whether a page should be deleted.
  bool should_delete_item(const OfflinePageItem& page) {
    const std::string& name_space = page.client_id.name_space;
    const OfflinePageClientPolicy& policy =
        policy_controller_->GetPolicy(name_space);
    const size_t page_limit = policy.lifetime_policy.page_limit;
    const base::TimeDelta expiration_period =
        policy.lifetime_policy.expiration_period;

    // If the cached pages exceed the storage limit, we need to clear more than
    // just expired pages to make the storage usage below the threshold.
    const bool quota_based_clearing =
        stats_.temporary_archives_size >=
        (stats_.temporary_archives_size + stats_.internal_free_disk_space) *
            kOfflinePageStorageLimit;
    const int64_t max_allowed_size =
        (stats_.temporary_archives_size + stats_.internal_free_disk_space) *
        kOfflinePageStorageClearThreshold;

    // If the page is expired, put it in the list to delete later.
    if (start_time_ - page.last_access_time >= expiration_period)
      return true;

    // If the namespace of the page already has more pages than limit, this page
    // needs to be deleted.
    if (page_limit != kUnlimitedPages &&
        namespace_page_count_[name_space] >= page_limit) {
      return true;
    }

    // Pages with no file can be removed.
    if (!base::PathExists(page.file_path))
      return true;

    // If there's no quota, remove the pages.
    if (quota_based_clearing &&
        remaining_size_ + page.file_size > max_allowed_size) {
      return true;
    }

    // Otherwise the page needs to be kept, in case the storage usage is still
    // higher than the threshold, and we need to clear more pages.
    remaining_size_ += page.file_size;
    ++namespace_page_count_[name_space];
    return false;
  }

 private:
  const ClientPolicyController* policy_controller_;
  base::Time start_time_;
  const ArchiveManager::StorageStats& stats_;

  int64_t remaining_size_ = 0;
  std::map<std::string, size_t> namespace_page_count_;
};

std::vector<OfflinePageItem> GetPagesToClear(
    const ClientPolicyController* policy_controller,
    const base::Time& start_time,
    const ArchiveManager::StorageStats& stats,
    sql::Database* db) {
  std::map<std::string, size_t> namespace_page_count;
  PageClearCriteria additional_criteria(policy_controller, start_time, stats);

  PageCriteria criteria;
  criteria.lifetime_type = LifetimeType::TEMPORARY;
  // Order is critical for correctness of PageClearCriteria::should_delete_item.
  criteria.result_order = PageCriteria::kDescendingAccessTime;
  criteria.additional_criteria =
      base::BindRepeating(&PageClearCriteria::should_delete_item,
                          base::Unretained(&additional_criteria));
  GetPagesTask::ReadResult result =
      GetPagesTask::ReadPagesWithCriteriaSync(policy_controller, criteria, db);

  return std::move(result.pages);
}

bool DeleteArchiveSync(const base::FilePath& file_path) {
  // Delete the file only, |false| for recursive.
  return base::DeleteFile(file_path, false);
}

std::pair<size_t, DeletePageResult> ClearPagesSync(
    ClientPolicyController* policy_controller,
    const base::Time& start_time,
    const ArchiveManager::StorageStats& stats,
    sql::Database* db) {
  std::vector<OfflinePageItem> pages_to_delete =
      GetPagesToClear(policy_controller, start_time, stats, db);

  size_t pages_cleared = 0;
  for (const OfflinePageItem& page : pages_to_delete) {
    if (!base::PathExists(page.file_path) ||
        DeleteArchiveSync(page.file_path)) {
      if (DeletePageTask::DeletePageFromDbSync(page.offline_id, db)) {
        pages_cleared++;
        // Reports the time since creation in minutes.
        base::TimeDelta time_since_creation = start_time - page.creation_time;
        UMA_HISTOGRAM_CUSTOM_COUNTS(
            "OfflinePages.ClearTemporaryPages.TimeSinceCreation",
            time_since_creation.InMinutes(), 1,
            base::TimeDelta::FromDays(30).InMinutes(), 50);
      }
    }
  }

  return std::make_pair(pages_cleared, pages_cleared == pages_to_delete.size()
                                           ? DeletePageResult::SUCCESS
                                           : DeletePageResult::STORE_FAILURE);
}

}  // namespace

ClearStorageTask::ClearStorageTask(OfflinePageMetadataStore* store,
                                   ArchiveManager* archive_manager,
                                   ClientPolicyController* policy_controller,
                                   const base::Time& clearup_time,
                                   ClearStorageCallback callback)
    : store_(store),
      archive_manager_(archive_manager),
      policy_controller_(policy_controller),
      callback_(std::move(callback)),
      clearup_time_(clearup_time),
      weak_ptr_factory_(this) {
  DCHECK(store_);
  DCHECK(archive_manager_);
  DCHECK(policy_controller_);
  DCHECK(!callback_.is_null());
}

ClearStorageTask::~ClearStorageTask() {}

void ClearStorageTask::Run() {
  TRACE_EVENT_ASYNC_BEGIN0("offline_pages", "ClearStorageTask running", this);
  archive_manager_->GetStorageStats(
      base::BindOnce(&ClearStorageTask::OnGetStorageStatsDone,
                     weak_ptr_factory_.GetWeakPtr()));
}

void ClearStorageTask::OnGetStorageStatsDone(
    const ArchiveManager::StorageStats& stats) {
  store_->Execute(
      base::BindOnce(&ClearPagesSync, policy_controller_, clearup_time_, stats),
      base::BindOnce(&ClearStorageTask::OnClearPagesDone,
                     weak_ptr_factory_.GetWeakPtr()),
      {0, DeletePageResult::STORE_FAILURE});
}

void ClearStorageTask::OnClearPagesDone(
    std::pair<size_t, DeletePageResult> result) {
  if (result.first == 0 && result.second == DeletePageResult::SUCCESS) {
    InformClearStorageDone(result.first, ClearStorageResult::UNNECESSARY);
    return;
  }

  ClearStorageResult clear_result = ClearStorageResult::SUCCESS;
  if (result.second != DeletePageResult::SUCCESS)
    clear_result = ClearStorageResult::DELETE_FAILURE;
  InformClearStorageDone(result.first, clear_result);
}

void ClearStorageTask::InformClearStorageDone(size_t pages_cleared,
                                              ClearStorageResult result) {
  std::move(callback_).Run(pages_cleared, result);
  TaskComplete();
  TRACE_EVENT_ASYNC_END2("offline_pages", "ClearStorageTask running", this,
                         "result", static_cast<int>(result), "pages_cleared",
                         pages_cleared);
}

}  // namespace offline_pages
