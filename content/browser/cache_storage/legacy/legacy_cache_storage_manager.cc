// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/cache_storage/legacy/legacy_cache_storage_manager.h"

#include <stdint.h>

#include <map>
#include <numeric>
#include <set>
#include <utility>

#include "base/barrier_closure.h"
#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/containers/id_map.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/hash/sha1.h"
#include "base/memory/ptr_util.h"
#include "base/sequenced_task_runner.h"
#include "base/stl_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/threading/sequenced_task_runner_handle.h"
#include "base/time/time.h"
#include "content/browser/cache_storage/cache_storage.h"
#include "content/browser/cache_storage/cache_storage.pb.h"
#include "content/browser/cache_storage/cache_storage_quota_client.h"
#include "content/browser/service_worker/service_worker_context_core.h"
#include "net/base/url_util.h"
#include "storage/browser/quota/quota_manager_proxy.h"
#include "storage/common/database/database_identifier.h"
#include "third_party/blink/public/mojom/quota/quota_types.mojom.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace content {

namespace {

bool DeleteDir(const base::FilePath& path) {
  return base::DeleteFile(path, true /* recursive */);
}

void DeleteOriginDidDeleteDir(storage::QuotaClient::DeletionCallback callback,
                              bool rv) {
  // On scheduler sequence.
  base::SequencedTaskRunnerHandle::Get()->PostTask(
      FROM_HERE,
      base::BindOnce(std::move(callback),
                     rv ? blink::mojom::QuotaStatusCode::kOk
                        : blink::mojom::QuotaStatusCode::kErrorAbort));
}

// Calculate the sum of all cache sizes in this store, but only if all sizes are
// known. If one or more sizes are not known then return kSizeUnknown.
int64_t GetCacheStorageSize(const proto::CacheStorageIndex& index) {
  int64_t storage_size = 0;
  for (int i = 0, max = index.cache_size(); i < max; ++i) {
    const proto::CacheStorageIndex::Cache& cache = index.cache(i);
    if (!cache.has_size() || cache.size() == CacheStorage::kSizeUnknown)
      return CacheStorage::kSizeUnknown;
    storage_size += cache.size();
  }
  return storage_size;
}

// Open the various cache directories' index files and extract their origins,
// sizes (if current), and last modified times.
void ListOriginsAndLastModifiedOnTaskRunner(
    std::vector<StorageUsageInfo>* usages,
    base::FilePath root_path,
    CacheStorageOwner owner) {
  base::FileEnumerator file_enum(root_path, false /* recursive */,
                                 base::FileEnumerator::DIRECTORIES);

  base::FilePath path;
  while (!(path = file_enum.Next()).empty()) {
    base::FilePath index_path =
        path.AppendASCII(LegacyCacheStorage::kIndexFileName);
    base::File::Info file_info;
    base::Time index_last_modified;
    if (GetFileInfo(index_path, &file_info))
      index_last_modified = file_info.last_modified;
    std::string protobuf;
    base::ReadFileToString(path.AppendASCII(LegacyCacheStorage::kIndexFileName),
                           &protobuf);
    proto::CacheStorageIndex index;
    if (index.ParseFromString(protobuf)) {
      if (index.has_origin()) {
        if (path ==
            LegacyCacheStorageManager::ConstructOriginPath(
                root_path, url::Origin::Create(GURL(index.origin())), owner)) {
          if (base::GetFileInfo(path, &file_info)) {
            int64_t storage_size = CacheStorage::kSizeUnknown;
            if (file_info.last_modified < index_last_modified)
              storage_size = GetCacheStorageSize(index);
            usages->push_back(
                StorageUsageInfo(url::Origin::Create(GURL(index.origin())),
                                 storage_size, file_info.last_modified));
          }
        }
      }
    }
  }
}

std::set<url::Origin> ListOriginsOnTaskRunner(base::FilePath root_path,
                                              CacheStorageOwner owner) {
  std::vector<StorageUsageInfo> usages;
  ListOriginsAndLastModifiedOnTaskRunner(&usages, root_path, owner);

  std::set<url::Origin> out_origins;
  for (const StorageUsageInfo& usage : usages)
    out_origins.insert(usage.origin);

  return out_origins;
}

void GetOriginsForHostDidListOrigins(
    const std::string& host,
    storage::QuotaClient::GetOriginsCallback callback,
    const std::set<url::Origin>& origins) {
  // On scheduler sequence.
  std::set<url::Origin> out_origins;
  for (const url::Origin& origin : origins) {
    if (host == net::GetHostOrSpecFromURL(origin.GetURL()))
      out_origins.insert(origin);
  }
  base::SequencedTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), out_origins));
}

void AllOriginSizesReported(
    std::unique_ptr<std::vector<StorageUsageInfo>> usages,
    CacheStorageContext::GetUsageInfoCallback callback) {
  // On scheduler sequence.
  base::SequencedTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), *usages));
}

void OneOriginSizeReported(base::OnceClosure callback,
                           StorageUsageInfo* usage,
                           int64_t size) {
  // On scheduler sequence.
  DCHECK_NE(size, CacheStorage::kSizeUnknown);
  usage->total_size_bytes = size;
  base::SequencedTaskRunnerHandle::Get()->PostTask(FROM_HERE,
                                                   std::move(callback));
}

}  // namespace

// static
scoped_refptr<LegacyCacheStorageManager> LegacyCacheStorageManager::Create(
    const base::FilePath& path,
    scoped_refptr<base::SequencedTaskRunner> cache_task_runner,
    scoped_refptr<base::SequencedTaskRunner> scheduler_task_runner,
    scoped_refptr<storage::QuotaManagerProxy> quota_manager_proxy,
    scoped_refptr<CacheStorageContextImpl::ObserverList> observers) {
  base::FilePath root_path = path;
  if (!path.empty()) {
    root_path = path.Append(ServiceWorkerContextCore::kServiceWorkerDirectory)
                    .AppendASCII("CacheStorage");
  }

  return base::WrapRefCounted(new LegacyCacheStorageManager(
      root_path, std::move(cache_task_runner), std::move(scheduler_task_runner),
      std::move(quota_manager_proxy), std::move(observers)));
}

// static
scoped_refptr<LegacyCacheStorageManager>
LegacyCacheStorageManager::CreateForTesting(
    LegacyCacheStorageManager* old_manager) {
  scoped_refptr<LegacyCacheStorageManager> manager(
      new LegacyCacheStorageManager(
          old_manager->root_path(), old_manager->cache_task_runner(),
          old_manager->scheduler_task_runner(),
          old_manager->quota_manager_proxy_.get(), old_manager->observers_));
  manager->SetBlobParametersForCache(old_manager->blob_storage_context());
  return manager;
}

LegacyCacheStorageManager::~LegacyCacheStorageManager() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

CacheStorageHandle LegacyCacheStorageManager::OpenCacheStorage(
    const url::Origin& origin,
    CacheStorageOwner owner) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // Wait to create the MemoryPressureListener until the first CacheStorage
  // object is needed.  This ensures we create the listener on the correct
  // thread.
  if (!memory_pressure_listener_) {
    memory_pressure_listener_ = std::make_unique<base::MemoryPressureListener>(
        base::BindRepeating(&LegacyCacheStorageManager::OnMemoryPressure,
                            base::Unretained(this)));
  }

  CacheStorageMap::const_iterator it = cache_storage_map_.find({origin, owner});
  if (it == cache_storage_map_.end()) {
    LegacyCacheStorage* cache_storage = new LegacyCacheStorage(
        ConstructOriginPath(root_path_, origin, owner), IsMemoryBacked(),
        cache_task_runner_.get(), scheduler_task_runner_, quota_manager_proxy_,
        blob_context_, this, origin, owner);
    cache_storage_map_[{origin, owner}] = base::WrapUnique(cache_storage);
    return cache_storage->CreateHandle();
  }
  return it->second.get()->CreateHandle();
}

void LegacyCacheStorageManager::SetBlobParametersForCache(
    base::WeakPtr<storage::BlobStorageContext> blob_storage_context) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(cache_storage_map_.empty());
  DCHECK(!blob_context_ || blob_context_.get() == blob_storage_context.get());
  blob_context_ = blob_storage_context;
}

void LegacyCacheStorageManager::NotifyCacheListChanged(
    const url::Origin& origin) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  observers_->Notify(FROM_HERE,
                     &CacheStorageContextImpl::Observer::OnCacheListChanged,
                     origin);
}

void LegacyCacheStorageManager::NotifyCacheContentChanged(
    const url::Origin& origin,
    const std::string& name) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  observers_->Notify(FROM_HERE,
                     &CacheStorageContextImpl::Observer::OnCacheContentChanged,
                     origin, name);
}

void LegacyCacheStorageManager::CacheStorageUnreferenced(
    LegacyCacheStorage* cache_storage,
    const url::Origin& origin,
    CacheStorageOwner owner) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(cache_storage);
  cache_storage->AssertUnreferenced();
  auto it = cache_storage_map_.find({origin, owner});
  DCHECK(it != cache_storage_map_.end());
  DCHECK(it->second.get() == cache_storage);

  // Currently we don't do anything when a CacheStorage instance becomes
  // unreferenced.  In the future we will deallocate some or all of the
  // CacheStorage's state.
}

void LegacyCacheStorageManager::GetAllOriginsUsage(
    CacheStorageOwner owner,
    CacheStorageContext::GetUsageInfoCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  auto usages = std::make_unique<std::vector<StorageUsageInfo>>();

  if (IsMemoryBacked()) {
    for (const auto& origin_details : cache_storage_map_) {
      if (origin_details.first.second != owner)
        continue;
      usages->emplace_back(origin_details.first.first,
                           /*total_size_bytes=*/0,
                           /*last_modified=*/base::Time());
    }
    GetAllOriginsUsageGetSizes(std::move(usages), std::move(callback));
    return;
  }

  std::vector<StorageUsageInfo>* usages_ptr = usages.get();
  cache_task_runner_->PostTaskAndReply(
      FROM_HERE,
      base::BindOnce(&ListOriginsAndLastModifiedOnTaskRunner, usages_ptr,
                     root_path_, owner),
      base::BindOnce(&LegacyCacheStorageManager::GetAllOriginsUsageGetSizes,
                     base::WrapRefCounted(this), std::move(usages),
                     std::move(callback)));
}

void LegacyCacheStorageManager::GetAllOriginsUsageGetSizes(
    std::unique_ptr<std::vector<StorageUsageInfo>> usages,
    CacheStorageContext::GetUsageInfoCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(usages);

  // The origin GURL and last modified times are set in |usages| but not the
  // size in bytes. Call each CacheStorage's Size() function to fill that out.
  std::vector<StorageUsageInfo>* usages_ptr = usages.get();

  if (usages->empty()) {
    scheduler_task_runner_->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback), *usages));
    return;
  }

  base::RepeatingClosure barrier_closure = base::BarrierClosure(
      usages_ptr->size(),
      base::BindOnce(&AllOriginSizesReported, std::move(usages),
                     std::move(callback)));

  for (StorageUsageInfo& usage : *usages_ptr) {
    if (usage.total_size_bytes != CacheStorage::kSizeUnknown ||
        !IsValidQuotaOrigin(usage.origin)) {
      scheduler_task_runner_->PostTask(FROM_HERE, barrier_closure);
      continue;
    }
    CacheStorageHandle cache_storage =
        OpenCacheStorage(usage.origin, CacheStorageOwner::kCacheAPI);
    LegacyCacheStorage::From(cache_storage)
        ->Size(base::BindOnce(&OneOriginSizeReported, barrier_closure, &usage));
  }
}

void LegacyCacheStorageManager::GetOriginUsage(
    const url::Origin& origin,
    CacheStorageOwner owner,
    storage::QuotaClient::GetUsageCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  CacheStorageHandle cache_storage = OpenCacheStorage(origin, owner);
  LegacyCacheStorage::From(cache_storage)->Size(std::move(callback));
}

void LegacyCacheStorageManager::GetOrigins(
    CacheStorageOwner owner,
    storage::QuotaClient::GetOriginsCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (IsMemoryBacked()) {
    std::set<url::Origin> origins;
    for (const auto& key_value : cache_storage_map_)
      if (key_value.first.second == owner)
        origins.insert(key_value.first.first);

    scheduler_task_runner_->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback), origins));
    return;
  }

  PostTaskAndReplyWithResult(
      cache_task_runner_.get(), FROM_HERE,
      base::BindOnce(&ListOriginsOnTaskRunner, root_path_, owner),
      std::move(callback));
}

void LegacyCacheStorageManager::GetOriginsForHost(
    const std::string& host,
    CacheStorageOwner owner,
    storage::QuotaClient::GetOriginsCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (IsMemoryBacked()) {
    std::set<url::Origin> origins;
    for (const auto& key_value : cache_storage_map_) {
      if (key_value.first.second != owner)
        continue;
      if (host == net::GetHostOrSpecFromURL(key_value.first.first.GetURL()))
        origins.insert(key_value.first.first);
    }
    scheduler_task_runner_->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback), origins));
    return;
  }

  PostTaskAndReplyWithResult(
      cache_task_runner_.get(), FROM_HERE,
      base::BindOnce(&ListOriginsOnTaskRunner, root_path_, owner),
      base::BindOnce(&GetOriginsForHostDidListOrigins, host,
                     std::move(callback)));
}

void LegacyCacheStorageManager::DeleteOriginData(
    const url::Origin& origin,
    CacheStorageOwner owner,
    storage::QuotaClient::DeletionCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // Create the CacheStorage for the origin if it hasn't been loaded yet.
  CacheStorageHandle handle = OpenCacheStorage(origin, owner);

  auto it = cache_storage_map_.find({origin, owner});
  DCHECK(it != cache_storage_map_.end());

  LegacyCacheStorage* cache_storage = it->second.release();
  cache_storage->ResetManager();
  cache_storage_map_.erase({origin, owner});
  cache_storage->GetSizeThenCloseAllCaches(
      base::BindOnce(&LegacyCacheStorageManager::DeleteOriginDidClose,
                     base::WrapRefCounted(this), origin, owner,
                     std::move(callback), base::WrapUnique(cache_storage)));
}

void LegacyCacheStorageManager::DeleteOriginData(const url::Origin& origin,
                                                 CacheStorageOwner owner) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DeleteOriginData(origin, owner, base::DoNothing());
}

void LegacyCacheStorageManager::DeleteOriginDidClose(
    const url::Origin& origin,
    CacheStorageOwner owner,
    storage::QuotaClient::DeletionCallback callback,
    std::unique_ptr<LegacyCacheStorage> cache_storage,
    int64_t origin_size) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // TODO(jkarlin): Deleting the storage leaves any unfinished operations
  // hanging, resulting in unresolved promises. Fix this by returning early from
  // CacheStorage operations posted after GetSizeThenCloseAllCaches is called.
  cache_storage.reset();

  quota_manager_proxy_->NotifyStorageModified(
      CacheStorageQuotaClient::GetIDFromOwner(owner), origin,
      blink::mojom::StorageType::kTemporary, -1 * origin_size);

  if (owner == CacheStorageOwner::kCacheAPI)
    NotifyCacheListChanged(origin);

  if (IsMemoryBacked()) {
    scheduler_task_runner_->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback),
                                  blink::mojom::QuotaStatusCode::kOk));
    return;
  }

  PostTaskAndReplyWithResult(
      cache_task_runner_.get(), FROM_HERE,
      base::BindOnce(&DeleteDir,
                     ConstructOriginPath(root_path_, origin, owner)),
      base::BindOnce(&DeleteOriginDidDeleteDir, std::move(callback)));
}

LegacyCacheStorageManager::LegacyCacheStorageManager(
    const base::FilePath& path,
    scoped_refptr<base::SequencedTaskRunner> cache_task_runner,
    scoped_refptr<base::SequencedTaskRunner> scheduler_task_runner,
    scoped_refptr<storage::QuotaManagerProxy> quota_manager_proxy,
    scoped_refptr<CacheStorageContextImpl::ObserverList> observers)
    : root_path_(path),
      cache_task_runner_(std::move(cache_task_runner)),
      scheduler_task_runner_(std::move(scheduler_task_runner)),
      quota_manager_proxy_(std::move(quota_manager_proxy)),
      observers_(std::move(observers)) {}

// static
base::FilePath LegacyCacheStorageManager::ConstructOriginPath(
    const base::FilePath& root_path,
    const url::Origin& origin,
    CacheStorageOwner owner) {
  std::string identifier = storage::GetIdentifierFromOrigin(origin);
  if (owner != CacheStorageOwner::kCacheAPI) {
    identifier += "-" + std::to_string(static_cast<int>(owner));
  }
  const std::string origin_hash = base::SHA1HashString(identifier);
  const std::string origin_hash_hex = base::ToLowerASCII(
      base::HexEncode(origin_hash.c_str(), origin_hash.length()));
  return root_path.AppendASCII(origin_hash_hex);
}

void LegacyCacheStorageManager::OnMemoryPressure(
    base::MemoryPressureListener::MemoryPressureLevel level) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (level != base::MemoryPressureListener::MEMORY_PRESSURE_LEVEL_CRITICAL)
    return;

  for (auto& entry : cache_storage_map_) {
    entry.second->ReleaseUnreferencedCaches();
  }
}

}  // namespace content
