// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/send_tab_to_self/send_tab_to_self_bridge.h"

#include <algorithm>

#include "base/bind.h"
#include "base/guid.h"
#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "base/metrics/histogram_macros.h"
#include "base/optional.h"
#include "base/time/clock.h"
#include "base/time/time.h"
#include "components/history/core/browser/history_service.h"
#include "components/send_tab_to_self/features.h"
#include "components/send_tab_to_self/proto/send_tab_to_self.pb.h"
#include "components/send_tab_to_self/target_device_info.h"
#include "components/sync/model/entity_change.h"
#include "components/sync/model/metadata_batch.h"
#include "components/sync/model/metadata_change_list.h"
#include "components/sync/model/model_type_change_processor.h"
#include "components/sync/model/mutable_data_batch.h"
#include "components/sync/protocol/model_type_state.pb.h"
#include "components/sync_device_info/device_info_tracker.h"
#include "components/sync_device_info/local_device_info_util.h"

namespace send_tab_to_self {

namespace {

// Status of the result of AddEntry.
// These values are persisted to logs. Entries should not be renumbered and
// numeric values should never be reused.
enum UMAAddEntryStatus {
  // The add entry call was successful.
  SUCCESS = 0,
  // The add entry call failed.
  FAILURE = 1,
  // The add entry call was a duplication.
  DUPLICATE = 2,
  // Update kMaxValue when new enums are added.
  kMaxValue = DUPLICATE,
};

using syncer::ModelTypeStore;

const base::TimeDelta kDedupeTime = base::TimeDelta::FromSeconds(5);

const base::TimeDelta kDeviceExpiration = base::TimeDelta::FromDays(10);

const char kAddEntryStatus[] = "SendTabToSelf.Sync.AddEntryStatus";

// Converts a time field from sync protobufs to a time object.
base::Time ProtoTimeToTime(int64_t proto_t) {
  return base::Time::FromDeltaSinceWindowsEpoch(
      base::TimeDelta::FromMicroseconds(proto_t));
}

// Allocate a EntityData and copies |specifics| into it.
std::unique_ptr<syncer::EntityData> CopyToEntityData(
    const sync_pb::SendTabToSelfSpecifics& specifics) {
  auto entity_data = std::make_unique<syncer::EntityData>();
  *entity_data->specifics.mutable_send_tab_to_self() = specifics;
  entity_data->name = specifics.url();
  entity_data->creation_time = ProtoTimeToTime(specifics.shared_time_usec());
  return entity_data;
}

// Parses the content of |record_list| into |*initial_data|. The output
// parameter is first for binding purposes.
base::Optional<syncer::ModelError> ParseLocalEntriesOnBackendSequence(
    base::Time now,
    std::map<std::string, std::unique_ptr<SendTabToSelfEntry>>* entries,
    std::string* local_session_name,
    std::unique_ptr<ModelTypeStore::RecordList> record_list) {
  DCHECK(entries);
  DCHECK(entries->empty());
  DCHECK(local_session_name);
  DCHECK(record_list);

  *local_session_name = syncer::GetSessionNameBlocking();

  for (const syncer::ModelTypeStore::Record& r : *record_list) {
    auto specifics = std::make_unique<SendTabToSelfLocal>();
    if (specifics->ParseFromString(r.value)) {
      (*entries)[specifics->specifics().guid()] =
          SendTabToSelfEntry::FromLocalProto(*specifics, now);
    } else {
      return syncer::ModelError(FROM_HERE, "Failed to deserialize specifics.");
    }
  }

  return base::nullopt;
}

}  // namespace

SendTabToSelfBridge::SendTabToSelfBridge(
    std::unique_ptr<syncer::ModelTypeChangeProcessor> change_processor,
    base::Clock* clock,
    syncer::OnceModelTypeStoreFactory create_store_callback,
    history::HistoryService* history_service,
    syncer::DeviceInfoTracker* device_info_tracker)
    : ModelTypeSyncBridge(std::move(change_processor)),
      clock_(clock),
      history_service_(history_service),
      device_info_tracker_(device_info_tracker),
      mru_entry_(nullptr),
      weak_ptr_factory_(this) {
  DCHECK(clock_);
  DCHECK(device_info_tracker_);
  if (history_service) {
    history_service->AddObserver(this);
  }

  std::move(create_store_callback)
      .Run(syncer::SEND_TAB_TO_SELF,
           base::BindOnce(&SendTabToSelfBridge::OnStoreCreated,
                          weak_ptr_factory_.GetWeakPtr()));
}

SendTabToSelfBridge::~SendTabToSelfBridge() {
  if (history_service_) {
    history_service_->RemoveObserver(this);
  }
}

std::unique_ptr<syncer::MetadataChangeList>
SendTabToSelfBridge::CreateMetadataChangeList() {
  return ModelTypeStore::WriteBatch::CreateMetadataChangeList();
}

base::Optional<syncer::ModelError> SendTabToSelfBridge::MergeSyncData(
    std::unique_ptr<syncer::MetadataChangeList> metadata_change_list,
    syncer::EntityChangeList entity_data) {
  DCHECK(entries_.empty());
  return ApplySyncChanges(std::move(metadata_change_list),
                          std::move(entity_data));
}

base::Optional<syncer::ModelError> SendTabToSelfBridge::ApplySyncChanges(
    std::unique_ptr<syncer::MetadataChangeList> metadata_change_list,
    syncer::EntityChangeList entity_changes) {
  std::vector<const SendTabToSelfEntry*> added;

  // The opened vector will accumulate both added entries that are already
  // opened as well as existing entries that have been updated to be marked as
  // opened.
  std::vector<const SendTabToSelfEntry*> opened;
  std::vector<std::string> removed;
  std::unique_ptr<ModelTypeStore::WriteBatch> batch =
      store_->CreateWriteBatch();

  for (const std::unique_ptr<syncer::EntityChange>& change : entity_changes) {
    const std::string& guid = change->storage_key();
    if (change->type() == syncer::EntityChange::ACTION_DELETE) {
      if (entries_.find(guid) != entries_.end()) {
        entries_.erase(change->storage_key());
        batch->DeleteData(guid);
        removed.push_back(change->storage_key());
      }
    } else {

      const sync_pb::SendTabToSelfSpecifics& specifics =
          change->data().specifics.send_tab_to_self();

      std::unique_ptr<SendTabToSelfEntry> remote_entry =
          SendTabToSelfEntry::FromProto(specifics, clock_->Now());
      if (!remote_entry) {
        continue;  // Skip invalid entries.
      }
      if (remote_entry->IsExpired(clock_->Now())) {
        // Remove expired data from server.
        change_processor()->Delete(guid, batch->GetMetadataChangeList());
      } else {
        SendTabToSelfEntry* local_entry =
            GetMutableEntryByGUID(remote_entry->GetGUID());
        SendTabToSelfLocal remote_entry_pb = remote_entry->AsLocalProto();

        if (local_entry == nullptr) {
          // This remote_entry is new. Add it to the model.
          added.push_back(remote_entry.get());
          if (remote_entry->IsOpened()) {
            opened.push_back(remote_entry.get());
          }
          entries_[remote_entry->GetGUID()] = std::move(remote_entry);
        } else {
          // Update existing model if entries have been opened.
          if (remote_entry->IsOpened() && !local_entry->IsOpened()) {
            local_entry->MarkOpened();
            opened.push_back(local_entry);
          }
        }

        // Write to the store.
        batch->WriteData(guid, remote_entry_pb.SerializeAsString());
      }
    }
  }

  batch->TakeMetadataChangesFrom(std::move(metadata_change_list));
  Commit(std::move(batch));

  if (!removed.empty()) {
    NotifyRemoteSendTabToSelfEntryDeleted(removed);
  }
  if (!added.empty()) {
    NotifyRemoteSendTabToSelfEntryAdded(added);
  }
  if (!opened.empty()) {
    NotifyRemoteSendTabToSelfEntryOpened(opened);
  }

  return base::nullopt;
}

void SendTabToSelfBridge::GetData(StorageKeyList storage_keys,
                                  DataCallback callback) {
  auto batch = std::make_unique<syncer::MutableDataBatch>();

  for (const std::string& guid : storage_keys) {
    const SendTabToSelfEntry* entry = GetEntryByGUID(guid);
    if (!entry) {
      continue;
    }

    batch->Put(guid, CopyToEntityData(entry->AsLocalProto().specifics()));
  }
  std::move(callback).Run(std::move(batch));
}

void SendTabToSelfBridge::GetAllDataForDebugging(DataCallback callback) {
  auto batch = std::make_unique<syncer::MutableDataBatch>();
  for (const auto& it : entries_) {
    batch->Put(it.first,
               CopyToEntityData(it.second->AsLocalProto().specifics()));
  }
  std::move(callback).Run(std::move(batch));
}

std::string SendTabToSelfBridge::GetClientTag(
    const syncer::EntityData& entity_data) {
  return GetStorageKey(entity_data);
}

std::string SendTabToSelfBridge::GetStorageKey(
    const syncer::EntityData& entity_data) {
  return entity_data.specifics.send_tab_to_self().guid();
}

std::vector<std::string> SendTabToSelfBridge::GetAllGuids() const {
  std::vector<std::string> keys;
  for (const auto& it : entries_) {
    DCHECK_EQ(it.first, it.second->GetGUID());
    keys.push_back(it.first);
  }
  return keys;
}

void SendTabToSelfBridge::DeleteAllEntries() {
  if (!change_processor()->IsTrackingMetadata()) {
    DCHECK_EQ(0ul, entries_.size());
    return;
  }

  std::unique_ptr<ModelTypeStore::WriteBatch> batch =
      store_->CreateWriteBatch();

  std::vector<std::string> all_guids = GetAllGuids();

  for (const auto& guid : all_guids) {
    change_processor()->Delete(guid, batch->GetMetadataChangeList());
    batch->DeleteData(guid);
  }
  entries_.clear();
  mru_entry_ = nullptr;

  NotifyRemoteSendTabToSelfEntryDeleted(all_guids);
}

const SendTabToSelfEntry* SendTabToSelfBridge::GetEntryByGUID(
    const std::string& guid) const {
  auto it = entries_.find(guid);
  if (it == entries_.end()) {
    return nullptr;
  }
  return it->second.get();
}

const SendTabToSelfEntry* SendTabToSelfBridge::AddEntry(
    const GURL& url,
    const std::string& title,
    base::Time navigation_time,
    const std::string& target_device_cache_guid) {
  if (!change_processor()->IsTrackingMetadata()) {
    // TODO(crbug.com/940512) handle failure case.
    UMA_HISTOGRAM_ENUMERATION(kAddEntryStatus, FAILURE);
    return nullptr;
  }

  if (!url.is_valid()) {
    UMA_HISTOGRAM_ENUMERATION(kAddEntryStatus, FAILURE);
    return nullptr;
  }

  // AddEntry should be a no-op if the UI is disabled
  if (!base::FeatureList::IsEnabled(kSendTabToSelfShowSendingUI)) {
    return nullptr;
  }

  // In the case where the user has attempted to send an identical URL
  // within the last |kDedupeTime| we think it is likely that user still
  // has the first sent tab in progress, and so we will not attempt to resend.
  base::Time shared_time = clock_->Now();
  if (mru_entry_ && url == mru_entry_->GetURL() &&
      navigation_time == mru_entry_->GetOriginalNavigationTime() &&
      shared_time - mru_entry_->GetSharedTime() < kDedupeTime) {
    UMA_HISTOGRAM_ENUMERATION(kAddEntryStatus, DUPLICATE);
    return mru_entry_;
  }

  std::string guid = base::GenerateGUID();

  // Assure that we don't have a guid collision.
  DCHECK_EQ(GetEntryByGUID(guid), nullptr);

  std::string trimmed_title = "";

  if (base::IsStringUTF8(title)) {
    trimmed_title = base::CollapseWhitespaceASCII(title, false);
  }

  auto entry = std::make_unique<SendTabToSelfEntry>(
      guid, url, trimmed_title, shared_time, navigation_time,
      local_device_name_, target_device_cache_guid);

  std::unique_ptr<ModelTypeStore::WriteBatch> batch =
      store_->CreateWriteBatch();
  // This entry is new. Add it to the store and model.
  auto entity_data = CopyToEntityData(entry->AsLocalProto().specifics());

  change_processor()->Put(guid, std::move(entity_data),
                          batch->GetMetadataChangeList());

  const SendTabToSelfEntry* result =
      entries_.emplace(guid, std::move(entry)).first->second.get();

  batch->WriteData(guid, result->AsLocalProto().SerializeAsString());

  Commit(std::move(batch));
  mru_entry_ = result;

  UMA_HISTOGRAM_ENUMERATION(kAddEntryStatus, SUCCESS);
  return result;
}

void SendTabToSelfBridge::DeleteEntry(const std::string& guid) {
  // Assure that an entry with that guid exists.
  if (GetEntryByGUID(guid) == nullptr) {
    return;
  }

  std::unique_ptr<ModelTypeStore::WriteBatch> batch =
      store_->CreateWriteBatch();

  DeleteEntryWithBatch(guid, batch.get());

  Commit(std::move(batch));
}

void SendTabToSelfBridge::DismissEntry(const std::string& guid) {
  SendTabToSelfEntry* entry = GetMutableEntryByGUID(guid);
  // Assure that an entry with that guid exists.
  if (!entry) {
    return;
  }

  entry->SetNotificationDismissed(true);

  std::unique_ptr<ModelTypeStore::WriteBatch> batch =
      store_->CreateWriteBatch();

  batch->WriteData(guid, entry->AsLocalProto().SerializeAsString());
  Commit(std::move(batch));
}

void SendTabToSelfBridge::MarkEntryOpened(const std::string& guid) {
  SendTabToSelfEntry* entry = GetMutableEntryByGUID(guid);
  // Assure that an entry with that guid exists.
  if (!entry) {
    return;
  }

  DCHECK(change_processor()->IsTrackingMetadata());

  entry->MarkOpened();

  std::unique_ptr<ModelTypeStore::WriteBatch> batch =
      store_->CreateWriteBatch();

  auto entity_data = CopyToEntityData(entry->AsLocalProto().specifics());

  change_processor()->Put(guid, std::move(entity_data),
                          batch->GetMetadataChangeList());

  batch->WriteData(guid, entry->AsLocalProto().SerializeAsString());
  Commit(std::move(batch));
}

void SendTabToSelfBridge::OnURLsDeleted(
    history::HistoryService* history_service,
    const history::DeletionInfo& deletion_info) {
  // We only care about actual user (or sync) deletions.

  if (!change_processor()->IsTrackingMetadata()) {
    return;  // Sync processor not yet ready, don't sync.
  }

  if (deletion_info.is_from_expiration())
    return;

  if (!deletion_info.IsAllHistory()) {
    std::vector<GURL> urls;

    for (const history::URLRow& row : deletion_info.deleted_rows()) {
      urls.push_back(row.url());
    }

    DeleteEntries(urls);
    return;
  }

  // All history was cleared: just delete all entries.
  DeleteAllEntries();
}

bool SendTabToSelfBridge::IsReady() {
  return change_processor()->IsTrackingMetadata();
}

bool SendTabToSelfBridge::HasValidTargetDevice() {
  if (ShouldUpdateTargetDeviceNameToCacheInfoMap()) {
    SetTargetDeviceNameToCacheInfoMap();
  }
  return target_device_name_to_cache_info_.size() > 0;
}

std::map<std::string, TargetDeviceInfo>
SendTabToSelfBridge::GetTargetDeviceNameToCacheInfoMap() {
  if (ShouldUpdateTargetDeviceNameToCacheInfoMap()) {
    SetTargetDeviceNameToCacheInfoMap();
  }
  return target_device_name_to_cache_info_;
}

// static
std::unique_ptr<syncer::ModelTypeStore>
SendTabToSelfBridge::DestroyAndStealStoreForTest(
    std::unique_ptr<SendTabToSelfBridge> bridge) {
  return std::move(bridge->store_);
}

bool SendTabToSelfBridge::ShouldUpdateTargetDeviceNameToCacheInfoMapForTest() {
  return ShouldUpdateTargetDeviceNameToCacheInfoMap();
}

void SendTabToSelfBridge::SetLocalDeviceNameForTest(
    const std::string& local_device_name) {
  local_device_name_ = local_device_name;
}

void SendTabToSelfBridge::NotifyRemoteSendTabToSelfEntryAdded(
    const std::vector<const SendTabToSelfEntry*>& new_entries) {
  std::vector<const SendTabToSelfEntry*> new_local_entries;

  if (base::FeatureList::IsEnabled(kSendTabToSelfBroadcast)) {
    new_local_entries = new_entries;
  } else {
    // Only pass along entries that are targeted at this device.
    DCHECK(!change_processor()->TrackedCacheGuid().empty());
    for (const SendTabToSelfEntry* entry : new_entries) {
      if (entry->GetTargetDeviceSyncCacheGuid() ==
          change_processor()->TrackedCacheGuid()) {
        new_local_entries.push_back(entry);
      }
    }
  }

  for (SendTabToSelfModelObserver& observer : observers_) {
    observer.EntriesAddedRemotely(new_local_entries);
  }
}

void SendTabToSelfBridge::NotifyRemoteSendTabToSelfEntryDeleted(
    const std::vector<std::string>& guids) {
  // TODO(crbug.com/956216): Only send the entries that targeted this device.
  for (SendTabToSelfModelObserver& observer : observers_) {
    observer.EntriesRemovedRemotely(guids);
  }
}

void SendTabToSelfBridge::NotifyRemoteSendTabToSelfEntryOpened(
    const std::vector<const SendTabToSelfEntry*>& opened_entries) {
  for (SendTabToSelfModelObserver& observer : observers_) {
    observer.EntriesOpenedRemotely(opened_entries);
  }
}

void SendTabToSelfBridge::NotifySendTabToSelfModelLoaded() {
  for (SendTabToSelfModelObserver& observer : observers_) {
    observer.SendTabToSelfModelLoaded();
  }
}

void SendTabToSelfBridge::OnStoreCreated(
    const base::Optional<syncer::ModelError>& error,
    std::unique_ptr<syncer::ModelTypeStore> store) {
  if (error) {
    change_processor()->ReportError(*error);
    return;
  }

  auto initial_entries = std::make_unique<SendTabToSelfEntries>();
  SendTabToSelfEntries* initial_entries_copy = initial_entries.get();

  auto local_device_name = std::make_unique<std::string>();
  std::string* local_device_name_copy = local_device_name.get();

  store_ = std::move(store);
  store_->ReadAllDataAndPreprocess(
      base::BindOnce(&ParseLocalEntriesOnBackendSequence, clock_->Now(),
                     base::Unretained(initial_entries_copy),
                     base::Unretained(local_device_name_copy)),
      base::BindOnce(&SendTabToSelfBridge::OnReadAllData,
                     weak_ptr_factory_.GetWeakPtr(), std::move(initial_entries),
                     std::move(local_device_name)));
}

void SendTabToSelfBridge::OnReadAllData(
    std::unique_ptr<SendTabToSelfEntries> initial_entries,
    std::unique_ptr<std::string> local_device_name,
    const base::Optional<syncer::ModelError>& error) {
  DCHECK(initial_entries);
  DCHECK(local_device_name);

  if (error) {
    change_processor()->ReportError(*error);
    return;
  }

  entries_ = std::move(*initial_entries);
  local_device_name_ = std::move(*local_device_name);

  store_->ReadAllMetadata(base::BindOnce(
      &SendTabToSelfBridge::OnReadAllMetadata, weak_ptr_factory_.GetWeakPtr()));
}

void SendTabToSelfBridge::OnReadAllMetadata(
    const base::Optional<syncer::ModelError>& error,
    std::unique_ptr<syncer::MetadataBatch> metadata_batch) {
  if (error) {
    change_processor()->ReportError(*error);
    return;
  }
  change_processor()->ModelReadyToSync(std::move(metadata_batch));
  NotifySendTabToSelfModelLoaded();

  DoGarbageCollection();
}

void SendTabToSelfBridge::OnCommit(
    const base::Optional<syncer::ModelError>& error) {
  if (error) {
    change_processor()->ReportError(*error);
  }
}

void SendTabToSelfBridge::Commit(
    std::unique_ptr<ModelTypeStore::WriteBatch> batch) {
  store_->CommitWriteBatch(std::move(batch),
                           base::BindOnce(&SendTabToSelfBridge::OnCommit,
                                          weak_ptr_factory_.GetWeakPtr()));
}

SendTabToSelfEntry* SendTabToSelfBridge::GetMutableEntryByGUID(
    const std::string& guid) const {
  auto it = entries_.find(guid);
  if (it == entries_.end()) {
    return nullptr;
  }
  return it->second.get();
}

void SendTabToSelfBridge::DoGarbageCollection() {
  std::vector<std::string> removed;

  auto entry = entries_.begin();
  while (entry != entries_.end()) {
    DCHECK_EQ(entry->first, entry->second->GetGUID());
    std::string guid = entry->first;
    bool expired = entry->second->IsExpired(clock_->Now());
    entry++;
    if (expired) {
      DeleteEntry(guid);
      removed.push_back(guid);
    }
  }
  NotifyRemoteSendTabToSelfEntryDeleted(removed);
}

bool SendTabToSelfBridge::ShouldUpdateTargetDeviceNameToCacheInfoMap() const {
  // The map should be updated if any of these is true:
  //   * The map is empty.
  //   * The number of total devices changed.
  //   * The oldest non-expired entry in the map is now expired.
  return target_device_name_to_cache_info_.empty() ||
         device_info_tracker_->GetAllDeviceInfo().size() !=
             number_of_devices_ ||
         clock_->Now() - oldest_non_expired_device_timestamp_ >
             kDeviceExpiration;
}

void SendTabToSelfBridge::SetTargetDeviceNameToCacheInfoMap() {
  std::vector<std::unique_ptr<syncer::DeviceInfo>> all_devices =
      device_info_tracker_->GetAllDeviceInfo();
  number_of_devices_ = all_devices.size();

  // Sort the DeviceInfo vector so the most recenly modified devices are first.
  std::stable_sort(all_devices.begin(), all_devices.end(),
                   [](const std::unique_ptr<syncer::DeviceInfo>& device1,
                      const std::unique_ptr<syncer::DeviceInfo>& device2) {
                     return device1->last_updated_timestamp() >
                            device2->last_updated_timestamp();
                   });

  target_device_name_to_cache_info_.clear();
  for (const auto& device : all_devices) {
    // If the current device is considered expired for our purposes, stop here
    // since the next devices in the vector are at least as expired than this
    // one.
    if (clock_->Now() - device->last_updated_timestamp() > kDeviceExpiration) {
      break;
    }

    // TODO(crbug.com/966413): Implement a better way to dedupe local devices in
    // case the user has other devices with the same name.
    // Don't include this device. Also compare the name as the device can have
    // different cache guids (e.g. after stopping and re-starting sync).
    if (device->guid() == change_processor()->TrackedCacheGuid() ||
        device->client_name() == local_device_name_) {
      continue;
    }

    // Don't include devices that have disabled the send tab to self receiving
    // feature.
    if (!device->send_tab_to_self_receiving_enabled()) {
      continue;
    }

    // Only keep one device per device name. We only keep the first occurrence
    // which is the most recent.
    TargetDeviceInfo target_device_info(device->guid(), device->device_type(),
                                        device->last_updated_timestamp());
    target_device_name_to_cache_info_.emplace(device->client_name(),
                                              target_device_info);
    oldest_non_expired_device_timestamp_ = device->last_updated_timestamp();
  }
}

void SendTabToSelfBridge::DeleteEntryWithBatch(
    const std::string& guid,
    ModelTypeStore::WriteBatch* batch) {
  // Assure that an entry with that guid exists.
  DCHECK(GetEntryByGUID(guid) != nullptr);
  DCHECK(change_processor()->IsTrackingMetadata());

  change_processor()->Delete(guid, batch->GetMetadataChangeList());

  if (mru_entry_ && mru_entry_->GetGUID() == guid) {
    mru_entry_ = nullptr;
  }

  entries_.erase(guid);
  batch->DeleteData(guid);
}

void SendTabToSelfBridge::DeleteEntries(const std::vector<GURL>& urls) {
  std::unique_ptr<ModelTypeStore::WriteBatch> batch =
      store_->CreateWriteBatch();

  std::vector<std::string> removed_guids;

  for (const GURL url : urls) {
    auto entry = entries_.begin();
    while (entry != entries_.end()) {
      bool to_delete = (url == entry->second->GetURL());

      std::string guid = entry->first;
      entry++;
      if (to_delete) {
        removed_guids.push_back(guid);
        DeleteEntryWithBatch(guid, batch.get());
      }
    }
  }
  Commit(std::move(batch));
  if (!removed_guids.empty()) {
    // To err on the side of completeness this notifies all clients that these
    // entries have been removed. Regardless of if these entries were removed
    // "remotely".
    NotifyRemoteSendTabToSelfEntryDeleted(removed_guids);
  }
}

}  // namespace send_tab_to_self
