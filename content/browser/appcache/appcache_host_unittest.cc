// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/appcache/appcache_host.h"

#include <stdint.h>

#include <memory>
#include <string>
#include <utility>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/test/scoped_task_environment.h"
#include "content/browser/appcache/appcache.h"
#include "content/browser/appcache/appcache_backend_impl.h"
#include "content/browser/appcache/appcache_group.h"
#include "content/browser/appcache/appcache_request_handler.h"
#include "content/browser/appcache/mock_appcache_policy.h"
#include "content/browser/appcache/mock_appcache_service.h"
#include "content/public/test/test_browser_context.h"
#include "content/public/test/test_renderer_host.h"
#include "content/test/test_web_contents.h"
#include "mojo/public/cpp/bindings/binding.h"
#include "mojo/public/cpp/test_support/test_utils.h"
#include "net/url_request/url_request.h"
#include "storage/browser/quota/quota_manager.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/mojom/appcache/appcache.mojom.h"
#include "third_party/blink/public/mojom/appcache/appcache_info.mojom.h"
#include "third_party/blink/public/mojom/devtools/console_message.mojom.h"
#include "url/origin.h"

namespace content {

class AppCacheHostTest : public testing::Test {
 public:
  AppCacheHostTest()
      : web_contents_(TestWebContents::Create(&browser_context_, nullptr)),
        kProcessIdForTest(web_contents_->GetMainFrame()->GetProcess()->GetID()),
        kRenderFrameIdForTest(web_contents_->GetMainFrame()->GetRoutingID()),
        mock_frontend_(web_contents_.get()) {
    get_status_callback_ = base::BindRepeating(
        &AppCacheHostTest::GetStatusCallback, base::Unretained(this));
    AppCacheRequestHandler::SetRunningInTests(true);
  }

  ~AppCacheHostTest() override {
    AppCacheRequestHandler::SetRunningInTests(false);
  }

  class MockFrontend : public blink::mojom::AppCacheFrontend,
                       public WebContentsObserver {
   public:
    MockFrontend(WebContents* web_contents)
        : WebContentsObserver(web_contents),
          last_cache_id_(-222),
          last_status_(blink::mojom::AppCacheStatus::APPCACHE_STATUS_OBSOLETE),
          last_event_id_(
              blink::mojom::AppCacheEventID::APPCACHE_OBSOLETE_EVENT),
          content_blocked_(false) {}

    void CacheSelected(blink::mojom::AppCacheInfoPtr info) override {
      last_cache_id_ = info->cache_id;
      last_status_ = info->status;
    }

    void EventRaised(blink::mojom::AppCacheEventID event_id) override {
      last_event_id_ = event_id;
    }

    void ErrorEventRaised(
        blink::mojom::AppCacheErrorDetailsPtr details) override {
      last_event_id_ = blink::mojom::AppCacheEventID::APPCACHE_ERROR_EVENT;
    }

    void ProgressEventRaised(const GURL& url,
                             int32_t num_total,
                             int32_t num_complete) override {
      last_event_id_ = blink::mojom::AppCacheEventID::APPCACHE_PROGRESS_EVENT;
    }

    void LogMessage(blink::mojom::ConsoleMessageLevel log_level,
                    const std::string& message) override {}

    void SetSubresourceFactory(
        network::mojom::URLLoaderFactoryPtr url_loader_factory) override {}

    // WebContentsObserver:
    void AppCacheAccessed(const GURL& manifest_url,
                          bool blocked_by_policy) override {
      appcache_accessed_ = true;
      if (blocked_by_policy)
        content_blocked_ = true;
    }

    int64_t last_cache_id_;
    blink::mojom::AppCacheStatus last_status_;
    blink::mojom::AppCacheEventID last_event_id_;
    bool content_blocked_;
    bool appcache_accessed_ = false;
  };

  class MockQuotaManagerProxy : public storage::QuotaManagerProxy {
   public:
    MockQuotaManagerProxy() : QuotaManagerProxy(nullptr, nullptr) {}

    // Not needed for our tests.
    void RegisterClient(storage::QuotaClient* client) override {}
    void NotifyStorageAccessed(storage::QuotaClient::ID client_id,
                               const url::Origin& origin,
                               blink::mojom::StorageType type) override {}
    void NotifyStorageModified(storage::QuotaClient::ID client_id,
                               const url::Origin& origin,
                               blink::mojom::StorageType type,
                               int64_t delta) override {}
    void SetUsageCacheEnabled(storage::QuotaClient::ID client_id,
                              const url::Origin& origin,
                              blink::mojom::StorageType type,
                              bool enabled) override {}
    void GetUsageAndQuota(base::SequencedTaskRunner* original_task_runner,
                          const url::Origin& origin,
                          blink::mojom::StorageType type,
                          UsageAndQuotaCallback callback) override {}

    void NotifyOriginInUse(const url::Origin& origin) override {
      inuse_[origin] += 1;
    }

    void NotifyOriginNoLongerInUse(const url::Origin& origin) override {
      inuse_[origin] -= 1;
    }

    int GetInUseCount(const url::Origin& origin) { return inuse_[origin]; }

    void reset() { inuse_.clear(); }

    // Map from origin to count of inuse notifications.
    std::map<url::Origin, int> inuse_;

   protected:
    ~MockQuotaManagerProxy() override {}
  };

  void GetStatusCallback(blink::mojom::AppCacheStatus status) {
    last_status_result_ = status;
  }

  void StartUpdateCallback(bool result) { last_start_result_ = result; }

  void SwapCacheCallback(bool result) { last_swap_result_ = result; }

  TestBrowserThreadBundle scoped_task_environment_;
  RenderViewHostTestEnabler rvh_enabler_;
  TestBrowserContext browser_context_;
  std::unique_ptr<TestWebContents> web_contents_;

  const int kProcessIdForTest;
  const int kRenderFrameIdForTest;
  const base::UnguessableToken kHostIdForTest =
      base::UnguessableToken::Create();

  // Mock classes for the 'host' to work with
  MockAppCacheService service_;
  MockFrontend mock_frontend_;

  // Mock callbacks we expect to receive from the 'host'
  blink::mojom::AppCacheHost::GetStatusCallback get_status_callback_;

  blink::mojom::AppCacheStatus last_status_result_;
  bool last_swap_result_;
  bool last_start_result_;
};

TEST_F(AppCacheHostTest, Basic) {
  // Construct a host and test what state it appears to be in.
  AppCacheHost host(kHostIdForTest, kProcessIdForTest, kRenderFrameIdForTest,
                    nullptr, &service_);
  host.set_frontend_for_testing(&mock_frontend_);
  EXPECT_EQ(kHostIdForTest, host.host_id());
  EXPECT_EQ(kProcessIdForTest, host.process_id());
  EXPECT_EQ(&service_, host.service());
  EXPECT_EQ(nullptr, host.associated_cache());
  EXPECT_FALSE(host.is_selection_pending());

  // See that the callbacks are delivered immediately
  // and respond as if there is no cache selected.
  last_status_result_ = blink::mojom::AppCacheStatus::APPCACHE_STATUS_OBSOLETE;
  host.GetStatus(std::move(get_status_callback_));
  EXPECT_EQ(blink::mojom::AppCacheStatus::APPCACHE_STATUS_UNCACHED,
            last_status_result_);

  last_start_result_ = true;
  host.StartUpdate(base::BindOnce(&AppCacheHostTest::StartUpdateCallback,
                                  base::Unretained(this)));
  EXPECT_FALSE(last_start_result_);

  last_swap_result_ = true;
  host.SwapCache(base::BindOnce(&AppCacheHostTest::SwapCacheCallback,
                                base::Unretained(this)));
  EXPECT_FALSE(last_swap_result_);
}

TEST_F(AppCacheHostTest, SelectNoCache) {
  scoped_refptr<MockQuotaManagerProxy> mock_quota_proxy =
      base::MakeRefCounted<MockQuotaManagerProxy>();
  service_.set_quota_manager_proxy(mock_quota_proxy.get());

  // Reset our mock frontend
  mock_frontend_.last_cache_id_ = -333;
  mock_frontend_.last_status_ =
      blink::mojom::AppCacheStatus::APPCACHE_STATUS_OBSOLETE;

  const GURL kDocAndOriginUrl(GURL("http://whatever/").GetOrigin());
  const url::Origin kOrigin(url::Origin::Create(kDocAndOriginUrl));
  {
    AppCacheHost host(kHostIdForTest, kProcessIdForTest, kRenderFrameIdForTest,
                      nullptr, &service_);
    host.set_frontend_for_testing(&mock_frontend_);
    host.SelectCache(kDocAndOriginUrl, blink::mojom::kAppCacheNoCacheId,
                     GURL());
    EXPECT_EQ(1, mock_quota_proxy->GetInUseCount(kOrigin));

    // We should have received an OnCacheSelected msg
    EXPECT_EQ(blink::mojom::kAppCacheNoCacheId, mock_frontend_.last_cache_id_);
    EXPECT_EQ(blink::mojom::AppCacheStatus::APPCACHE_STATUS_UNCACHED,
              mock_frontend_.last_status_);

    // Otherwise, see that it respond as if there is no cache selected.
    EXPECT_EQ(kHostIdForTest, host.host_id());
    EXPECT_EQ(&service_, host.service());
    EXPECT_EQ(nullptr, host.associated_cache());
    EXPECT_FALSE(host.is_selection_pending());
    EXPECT_TRUE(host.preferred_manifest_url().is_empty());
  }
  EXPECT_EQ(0, mock_quota_proxy->GetInUseCount(kOrigin));
  service_.set_quota_manager_proxy(nullptr);
}

TEST_F(AppCacheHostTest, ForeignEntry) {
  // Reset our mock frontend
  mock_frontend_.last_cache_id_ = -333;
  mock_frontend_.last_status_ =
      blink::mojom::AppCacheStatus::APPCACHE_STATUS_OBSOLETE;

  // Precondition, a cache with an entry that is not marked as foreign.
  const int kCacheId = 22;
  const GURL kDocumentURL("http://origin/document");
  auto cache = base::MakeRefCounted<AppCache>(service_.storage(), kCacheId);
  cache->AddEntry(kDocumentURL, AppCacheEntry(AppCacheEntry::EXPLICIT));

  AppCacheHost host(kHostIdForTest, kProcessIdForTest, kRenderFrameIdForTest,
                    nullptr, &service_);
  host.set_frontend_for_testing(&mock_frontend_);
  host.MarkAsForeignEntry(kDocumentURL, kCacheId);

  // We should have received an OnCacheSelected msg for kAppCacheNoCacheId.
  EXPECT_EQ(blink::mojom::kAppCacheNoCacheId, mock_frontend_.last_cache_id_);
  EXPECT_EQ(blink::mojom::AppCacheStatus::APPCACHE_STATUS_UNCACHED,
            mock_frontend_.last_status_);

  // See that it respond as if there is no cache selected.
  EXPECT_EQ(kHostIdForTest, host.host_id());
  EXPECT_EQ(&service_, host.service());
  EXPECT_EQ(nullptr, host.associated_cache());
  EXPECT_FALSE(host.is_selection_pending());

  // See that the entry was marked as foreign.
  EXPECT_TRUE(cache->GetEntry(kDocumentURL)->IsForeign());
}

TEST_F(AppCacheHostTest, ForeignFallbackEntry) {
  // Reset our mock frontend
  mock_frontend_.last_cache_id_ = -333;
  mock_frontend_.last_status_ =
      blink::mojom::AppCacheStatus::APPCACHE_STATUS_OBSOLETE;

  // Precondition, a cache with a fallback entry that is not marked as foreign.
  const int kCacheId = 22;
  const GURL kFallbackURL("http://origin/fallback_resource");
  scoped_refptr<AppCache> cache =
      base::MakeRefCounted<AppCache>(service_.storage(), kCacheId);
  cache->AddEntry(kFallbackURL, AppCacheEntry(AppCacheEntry::FALLBACK));

  AppCacheHost host(kHostIdForTest, kProcessIdForTest, kRenderFrameIdForTest,
                    nullptr, &service_);
  host.set_frontend_for_testing(&mock_frontend_);
  host.NotifyMainResourceIsNamespaceEntry(kFallbackURL);
  host.MarkAsForeignEntry(GURL("http://origin/missing_document"), kCacheId);

  // We should have received an OnCacheSelected msg for kAppCacheNoCacheId.
  EXPECT_EQ(blink::mojom::kAppCacheNoCacheId, mock_frontend_.last_cache_id_);
  EXPECT_EQ(blink::mojom::AppCacheStatus::APPCACHE_STATUS_UNCACHED,
            mock_frontend_.last_status_);

  // See that the fallback entry was marked as foreign.
  EXPECT_TRUE(cache->GetEntry(kFallbackURL)->IsForeign());
}

TEST_F(AppCacheHostTest, FailedCacheLoad) {
  // Reset our mock frontend
  mock_frontend_.last_cache_id_ = -333;
  mock_frontend_.last_status_ =
      blink::mojom::AppCacheStatus::APPCACHE_STATUS_OBSOLETE;

  AppCacheHost host(kHostIdForTest, kProcessIdForTest, kRenderFrameIdForTest,
                    nullptr, &service_);
  host.set_frontend_for_testing(&mock_frontend_);
  EXPECT_FALSE(host.is_selection_pending());

  const int kMockCacheId = 333;

  // Put it in a state where we're waiting on a cache
  // load prior to finishing cache selection.
  host.pending_selected_cache_id_ = kMockCacheId;
  EXPECT_TRUE(host.is_selection_pending());

  // The callback should not occur until we finish cache selection.
  last_status_result_ = blink::mojom::AppCacheStatus::APPCACHE_STATUS_OBSOLETE;
  host.GetStatus(std::move(get_status_callback_));
  EXPECT_EQ(blink::mojom::AppCacheStatus::APPCACHE_STATUS_OBSOLETE,
            last_status_result_);

  // Satisfy the load with NULL, a failure.
  host.OnCacheLoaded(nullptr, kMockCacheId);

  // Cache selection should have finished
  EXPECT_FALSE(host.is_selection_pending());
  EXPECT_EQ(blink::mojom::kAppCacheNoCacheId, mock_frontend_.last_cache_id_);
  EXPECT_EQ(blink::mojom::AppCacheStatus::APPCACHE_STATUS_UNCACHED,
            mock_frontend_.last_status_);

  // Callback should have fired upon completing the cache load too.
  EXPECT_EQ(blink::mojom::AppCacheStatus::APPCACHE_STATUS_UNCACHED,
            last_status_result_);
}

TEST_F(AppCacheHostTest, FailedGroupLoad) {
  AppCacheHost host(kHostIdForTest, kProcessIdForTest, kRenderFrameIdForTest,
                    nullptr, &service_);
  host.set_frontend_for_testing(&mock_frontend_);

  const GURL kMockManifestUrl("http://foo.bar/baz");

  // Put it in a state where we're waiting on a cache
  // load prior to finishing cache selection.
  host.pending_selected_manifest_url_ = kMockManifestUrl;
  EXPECT_TRUE(host.is_selection_pending());

  // The callback should not occur until we finish cache selection.
  last_status_result_ = blink::mojom::AppCacheStatus::APPCACHE_STATUS_OBSOLETE;
  host.GetStatus(std::move(get_status_callback_));
  EXPECT_EQ(blink::mojom::AppCacheStatus::APPCACHE_STATUS_OBSOLETE,
            last_status_result_);

  // Satisfy the load will NULL, a failure.
  host.OnGroupLoaded(nullptr, kMockManifestUrl);

  // Cache selection should have finished
  EXPECT_FALSE(host.is_selection_pending());
  EXPECT_EQ(blink::mojom::kAppCacheNoCacheId, mock_frontend_.last_cache_id_);
  EXPECT_EQ(blink::mojom::AppCacheStatus::APPCACHE_STATUS_UNCACHED,
            mock_frontend_.last_status_);

  // Callback should have fired upon completing the group load.
  EXPECT_EQ(blink::mojom::AppCacheStatus::APPCACHE_STATUS_UNCACHED,
            last_status_result_);
}

TEST_F(AppCacheHostTest, SetSwappableCache) {
  AppCacheHost host(kHostIdForTest, kProcessIdForTest, kRenderFrameIdForTest,
                    nullptr, &service_);
  host.set_frontend_for_testing(&mock_frontend_);
  host.SetSwappableCache(nullptr);
  EXPECT_FALSE(host.swappable_cache_.get());

  const GURL kGroup1ManifestUrl("http://bar.com");
  scoped_refptr<AppCacheGroup> group1 = base::MakeRefCounted<AppCacheGroup>(
      service_.storage(), kGroup1ManifestUrl, service_.storage()->NewGroupId());
  host.SetSwappableCache(group1.get());
  EXPECT_FALSE(host.swappable_cache_.get());

  scoped_refptr<AppCache> cache1 =
      base::MakeRefCounted<AppCache>(service_.storage(), 111);
  cache1->set_complete(true);
  group1->AddCache(cache1.get());
  host.SetSwappableCache(group1.get());
  EXPECT_EQ(cache1, host.swappable_cache_.get());

  mock_frontend_.last_cache_id_ =
      -222;  // to verify we received OnCacheSelected

  host.AssociateCompleteCache(cache1.get());
  EXPECT_FALSE(host.swappable_cache_.get());  // was same as associated cache
  EXPECT_EQ(blink::mojom::AppCacheStatus::APPCACHE_STATUS_IDLE,
            host.GetStatusSync());
  // verify OnCacheSelected was called
  EXPECT_EQ(cache1->cache_id(), mock_frontend_.last_cache_id_);
  EXPECT_EQ(blink::mojom::AppCacheStatus::APPCACHE_STATUS_IDLE,
            mock_frontend_.last_status_);

  scoped_refptr<AppCache> cache2 =
      base::MakeRefCounted<AppCache>(service_.storage(), 222);
  cache2->set_complete(true);
  group1->AddCache(cache2.get());
  EXPECT_EQ(cache2.get(), host.swappable_cache_.get());  // updated to newest

  const GURL kGroup2ManifestUrl("http://foo.com/");
  scoped_refptr<AppCacheGroup> group2 = base::MakeRefCounted<AppCacheGroup>(
      service_.storage(), kGroup2ManifestUrl, service_.storage()->NewGroupId());
  scoped_refptr<AppCache> cache3 =
      base::MakeRefCounted<AppCache>(service_.storage(), 333);
  cache3->set_complete(true);
  group2->AddCache(cache3.get());

  scoped_refptr<AppCache> cache4 =
      base::MakeRefCounted<AppCache>(service_.storage(), 444);
  cache4->set_complete(true);
  group2->AddCache(cache4.get());
  EXPECT_EQ(cache2.get(), host.swappable_cache_.get());  // unchanged

  cache1.reset();
  cache2.reset();

  host.AssociateCompleteCache(cache3.get());
  EXPECT_EQ(cache4.get(),
            host.swappable_cache_.get());  // newest cache in group2
  EXPECT_FALSE(group1->HasCache());  // both caches in group1 have refcount 0

  cache3.reset();
  cache4.reset();

  host.AssociateNoCache(kGroup1ManifestUrl);
  EXPECT_FALSE(host.swappable_cache_.get());
  EXPECT_FALSE(group2->HasCache());  // both caches in group2 have refcount 0

  // Host adds reference to newest cache when an update is complete.
  scoped_refptr<AppCache> cache5 =
      base::MakeRefCounted<AppCache>(service_.storage(), 555);
  cache5->set_complete(true);
  group2->AddCache(cache5.get());
  host.group_being_updated_ = group2;
  host.OnUpdateComplete(group2.get());
  EXPECT_FALSE(host.group_being_updated_.get());
  EXPECT_EQ(cache5.get(), host.swappable_cache_.get());

  group2->RemoveCache(cache5.get());
  EXPECT_FALSE(group2->HasCache());
  host.group_being_updated_ = group2;
  host.OnUpdateComplete(group2.get());
  EXPECT_FALSE(host.group_being_updated_.get());
  EXPECT_FALSE(host.swappable_cache_.get());  // group2 had no newest cache
}

TEST_F(AppCacheHostTest, SelectCacheAllowed) {
  scoped_refptr<MockQuotaManagerProxy> mock_quota_proxy =
      base::MakeRefCounted<MockQuotaManagerProxy>();
  MockAppCachePolicy mock_appcache_policy;
  mock_appcache_policy.can_create_return_value_ = true;
  service_.set_quota_manager_proxy(mock_quota_proxy.get());
  service_.set_appcache_policy(&mock_appcache_policy);

  // Reset our mock frontend
  mock_frontend_.last_cache_id_ = -333;
  mock_frontend_.last_status_ =
      blink::mojom::AppCacheStatus::APPCACHE_STATUS_OBSOLETE;
  mock_frontend_.last_event_id_ =
      blink::mojom::AppCacheEventID::APPCACHE_OBSOLETE_EVENT;
  mock_frontend_.content_blocked_ = false;
  mock_frontend_.appcache_accessed_ = false;

  const GURL kDocAndOriginUrl(GURL("http://whatever/").GetOrigin());
  const url::Origin kOrigin(url::Origin::Create(kDocAndOriginUrl));
  const GURL kManifestUrl(GURL("http://whatever/cache.manifest"));
  {
    AppCacheHost host(kHostIdForTest, kProcessIdForTest, kRenderFrameIdForTest,
                      nullptr, &service_);
    host.set_frontend_for_testing(&mock_frontend_);
    host.SetFirstPartyUrlForTesting(kDocAndOriginUrl);
    host.SelectCache(kDocAndOriginUrl, blink::mojom::kAppCacheNoCacheId,
                     kManifestUrl);
    EXPECT_EQ(1, mock_quota_proxy->GetInUseCount(kOrigin));

    // MockAppCacheService::LoadOrCreateGroup is asynchronous, so we shouldn't
    // have received an OnCacheSelected msg yet.
    EXPECT_EQ(-333, mock_frontend_.last_cache_id_);
    EXPECT_EQ(blink::mojom::AppCacheStatus::APPCACHE_STATUS_OBSOLETE,
              mock_frontend_.last_status_);
    // No error events either
    EXPECT_EQ(blink::mojom::AppCacheEventID::APPCACHE_OBSOLETE_EVENT,
              mock_frontend_.last_event_id_);
    EXPECT_FALSE(mock_frontend_.content_blocked_);

    EXPECT_TRUE(host.is_selection_pending());

    base::RunLoop().RunUntilIdle();
    EXPECT_FALSE(mock_frontend_.content_blocked_);
    EXPECT_TRUE(mock_frontend_.appcache_accessed_);
  }
  EXPECT_EQ(0, mock_quota_proxy->GetInUseCount(kOrigin));
  service_.set_quota_manager_proxy(nullptr);
}

TEST_F(AppCacheHostTest, SelectCacheBlocked) {
  scoped_refptr<MockQuotaManagerProxy> mock_quota_proxy =
      base::MakeRefCounted<MockQuotaManagerProxy>();
  MockAppCachePolicy mock_appcache_policy;
  mock_appcache_policy.can_create_return_value_ = false;
  service_.set_quota_manager_proxy(mock_quota_proxy.get());
  service_.set_appcache_policy(&mock_appcache_policy);

  // Reset our mock frontend
  mock_frontend_.last_cache_id_ = -333;
  mock_frontend_.last_status_ =
      blink::mojom::AppCacheStatus::APPCACHE_STATUS_OBSOLETE;
  mock_frontend_.last_event_id_ =
      blink::mojom::AppCacheEventID::APPCACHE_OBSOLETE_EVENT;
  mock_frontend_.content_blocked_ = false;
  mock_frontend_.appcache_accessed_ = false;

  const GURL kDocAndOriginUrl(GURL("http://whatever/").GetOrigin());
  const url::Origin kOrigin(url::Origin::Create(kDocAndOriginUrl));
  const GURL kManifestUrl(GURL("http://whatever/cache.manifest"));
  {
    AppCacheHost host(kHostIdForTest, kProcessIdForTest, kRenderFrameIdForTest,
                      nullptr, &service_);
    host.set_frontend_for_testing(&mock_frontend_);
    host.SetFirstPartyUrlForTesting(kDocAndOriginUrl);
    host.SelectCache(kDocAndOriginUrl, blink::mojom::kAppCacheNoCacheId,
                     kManifestUrl);
    EXPECT_EQ(1, mock_quota_proxy->GetInUseCount(kOrigin));

    // We should have received an OnCacheSelected msg
    EXPECT_EQ(blink::mojom::kAppCacheNoCacheId, mock_frontend_.last_cache_id_);
    EXPECT_EQ(blink::mojom::AppCacheStatus::APPCACHE_STATUS_UNCACHED,
              mock_frontend_.last_status_);

    // Also, an error event was raised
    EXPECT_EQ(blink::mojom::AppCacheEventID::APPCACHE_ERROR_EVENT,
              mock_frontend_.last_event_id_);

    // Otherwise, see that it respond as if there is no cache selected.
    EXPECT_EQ(kHostIdForTest, host.host_id());
    EXPECT_EQ(&service_, host.service());
    EXPECT_EQ(nullptr, host.associated_cache());
    EXPECT_FALSE(host.is_selection_pending());
    EXPECT_TRUE(host.preferred_manifest_url().is_empty());

    base::RunLoop().RunUntilIdle();
    EXPECT_TRUE(mock_frontend_.content_blocked_);
    EXPECT_TRUE(mock_frontend_.appcache_accessed_);
  }
  EXPECT_EQ(0, mock_quota_proxy->GetInUseCount(kOrigin));
  service_.set_quota_manager_proxy(nullptr);
}

TEST_F(AppCacheHostTest, SelectCacheTwice) {
  const GURL kDocAndOriginUrl(GURL("http://whatever/").GetOrigin());
  AppCacheHost host(kHostIdForTest, kProcessIdForTest, kRenderFrameIdForTest,
                    nullptr, &service_);
  host.set_frontend_for_testing(&mock_frontend_);
  blink::mojom::AppCacheHostPtr host_ptr;
  host.BindRequest(mojo::MakeRequest(&host_ptr));

  {
    mojo::test::BadMessageObserver bad_message_observer;
    host_ptr->SelectCache(kDocAndOriginUrl, blink::mojom::kAppCacheNoCacheId,
                          GURL());

    base::RunLoop().RunUntilIdle();
    EXPECT_FALSE(bad_message_observer.got_bad_message());
  }

  // Select methods should bail if cache has already been selected.
  {
    mojo::test::BadMessageObserver bad_message_observer;
    host_ptr->SelectCache(kDocAndOriginUrl, blink::mojom::kAppCacheNoCacheId,
                          GURL());
    EXPECT_EQ("ACH_SELECT_CACHE", bad_message_observer.WaitForBadMessage());
  }
  {
    mojo::test::BadMessageObserver bad_message_observer;
    host_ptr->SelectCacheForSharedWorker(blink::mojom::kAppCacheNoCacheId);
    EXPECT_EQ("ACH_SELECT_CACHE_FOR_SHARED_WORKER",
              bad_message_observer.WaitForBadMessage());
  }
  {
    mojo::test::BadMessageObserver bad_message_observer;
    host_ptr->MarkAsForeignEntry(kDocAndOriginUrl,
                                 blink::mojom::kAppCacheNoCacheId);
    EXPECT_EQ("ACH_MARK_AS_FOREIGN_ENTRY",
              bad_message_observer.WaitForBadMessage());
  }
}

TEST_F(AppCacheHostTest, SelectCacheInvalidCacheId) {
  const GURL kDocAndOriginUrl(GURL("http://whatever/").GetOrigin());

  // A cache that the document wasn't actually loaded from. Trying to select it
  // should cause a BadMessage.
  const int kCacheId = 22;
  const GURL kDocumentURL("http://origin/document");
  auto cache = base::MakeRefCounted<AppCache>(service_.storage(), kCacheId);
  AppCacheHost host(kHostIdForTest, kProcessIdForTest, kRenderFrameIdForTest,
                    nullptr, &service_);
  host.set_frontend_for_testing(&mock_frontend_);
  blink::mojom::AppCacheHostPtr host_ptr;
  host.BindRequest(mojo::MakeRequest(&host_ptr));

  {
    mojo::test::BadMessageObserver bad_message_observer;
    host_ptr->SelectCache(kDocAndOriginUrl, kCacheId, GURL());

    EXPECT_EQ("ACH_SELECT_CACHE_ID_NOT_OWNED",
              bad_message_observer.WaitForBadMessage());
  }
}

}  // namespace content
