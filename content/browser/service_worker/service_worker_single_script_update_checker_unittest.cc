// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/service_worker/service_worker_single_script_update_checker.h"

#include <vector>
#include "base/bind.h"
#include "base/containers/queue.h"
#include "base/run_loop.h"
#include "content/browser/service_worker/embedded_worker_test_helper.h"
#include "content/browser/service_worker/service_worker_context_core.h"
#include "content/browser/service_worker/service_worker_storage.h"
#include "content/browser/service_worker/service_worker_test_utils.h"
#include "content/public/test/test_browser_thread_bundle.h"
#include "net/base/load_flags.h"
#include "net/http/http_util.h"
#include "services/network/test/test_url_loader_factory.h"
#include "services/network/test/test_utils.h"

namespace content {
namespace {

constexpr char kScriptURL[] = "https://example.com/script.js";
constexpr char kImportedScriptURL[] = "https://example.com/imported-script.js";
constexpr char kScope[] = "https://example.com/";
constexpr char kSuccessHeader[] =
    "HTTP/1.1 200 OK\n"
    "Content-Type: text/javascript\n\n";

class ServiceWorkerSingleScriptUpdateCheckerTest : public testing::Test {
 public:
  struct CheckResult {
    CheckResult(
        const GURL& script_url,
        ServiceWorkerSingleScriptUpdateChecker::Result compare_result,
        std::unique_ptr<ServiceWorkerSingleScriptUpdateChecker::FailureInfo>
            failure_info,
        std::unique_ptr<ServiceWorkerSingleScriptUpdateChecker::PausedState>
            paused_state)
        : url(script_url),
          result(compare_result),
          failure_info(std::move(failure_info)),
          paused_state(std::move(paused_state)) {}

    CheckResult(CheckResult&& ref) = default;

    CheckResult& operator=(CheckResult&& ref) = default;

    ~CheckResult() = default;

    GURL url;
    ServiceWorkerSingleScriptUpdateChecker::Result result;
    std::unique_ptr<ServiceWorkerSingleScriptUpdateChecker::FailureInfo>
        failure_info;
    std::unique_ptr<ServiceWorkerSingleScriptUpdateChecker::PausedState>
        paused_state;
  };

  ServiceWorkerSingleScriptUpdateCheckerTest()
      : thread_bundle_(TestBrowserThreadBundle::IO_MAINLOOP) {}
  ~ServiceWorkerSingleScriptUpdateCheckerTest() override = default;

  ServiceWorkerStorage* storage() { return helper_->context()->storage(); }

  void SetUp() override {
    helper_ = std::make_unique<EmbeddedWorkerTestHelper>(base::FilePath());
    base::RunLoop run_loop;
    storage()->LazyInitializeForTest(run_loop.QuitClosure());
    run_loop.Run();
  }

  size_t TotalBytes(const std::vector<std::string>& data_chunks) {
    size_t bytes = 0;
    for (const auto& data : data_chunks)
      bytes += data.size();
    return bytes;
  }

  // Create an update checker which will always ask HTTP cache validation.
  std::unique_ptr<ServiceWorkerSingleScriptUpdateChecker>
  CreateSingleScriptUpdateCheckerWithoutHttpCache(
      const char* url,
      const GURL& scope,
      std::unique_ptr<ServiceWorkerResponseReader> compare_reader,
      std::unique_ptr<ServiceWorkerResponseReader> copy_reader,
      std::unique_ptr<ServiceWorkerResponseWriter> writer,
      network::TestURLLoaderFactory* loader_factory,
      base::Optional<CheckResult>* out_check_result) {
    return CreateSingleScriptUpdateChecker(
        url, scope, true /* is_main_script */, false /* force_bypass_cache */,
        blink::mojom::ServiceWorkerUpdateViaCache::kNone,
        base::TimeDelta() /* time_since_last_check */,
        std::move(compare_reader), std::move(copy_reader), std::move(writer),
        loader_factory, out_check_result);
  }

  std::unique_ptr<ServiceWorkerSingleScriptUpdateChecker>
  CreateSingleScriptUpdateChecker(
      const char* url,
      const GURL& scope,
      bool is_main_script,
      bool force_bypass_cache,
      blink::mojom::ServiceWorkerUpdateViaCache update_via_cache,
      base::TimeDelta time_since_last_check,
      std::unique_ptr<ServiceWorkerResponseReader> compare_reader,
      std::unique_ptr<ServiceWorkerResponseReader> copy_reader,
      std::unique_ptr<ServiceWorkerResponseWriter> writer,
      network::TestURLLoaderFactory* loader_factory,
      base::Optional<CheckResult>* out_check_result) {
    helper_->SetNetworkFactory(loader_factory);
    return std::make_unique<ServiceWorkerSingleScriptUpdateChecker>(
        GURL(url), is_main_script, scope, force_bypass_cache, update_via_cache,
        time_since_last_check,
        helper_->url_loader_factory_getter()->GetNetworkFactory(),
        std::move(compare_reader), std::move(copy_reader), std::move(writer),
        base::BindOnce(
            [](base::Optional<CheckResult>* out_check_result_param,
               const GURL& script_url,
               ServiceWorkerSingleScriptUpdateChecker::Result result,
               std::unique_ptr<
                   ServiceWorkerSingleScriptUpdateChecker::FailureInfo>
                   failure_info,
               std::unique_ptr<
                   ServiceWorkerSingleScriptUpdateChecker::PausedState>
                   paused_state) {
              *out_check_result_param =
                  CheckResult(script_url, result, std::move(failure_info),
                              std::move(paused_state));
            },
            out_check_result));
  }

  std::unique_ptr<network::TestURLLoaderFactory> CreateLoaderFactoryWithRespone(
      const GURL& url,
      const std::string& header,
      const std::string& body,
      net::Error error) {
    auto loader_factory = std::make_unique<network::TestURLLoaderFactory>();
    network::ResourceResponseHead head;
    head.headers = base::MakeRefCounted<net::HttpResponseHeaders>(
        net::HttpUtil::AssembleRawHeaders(header));
    head.headers->GetMimeType(&head.mime_type);
    network::URLLoaderCompletionStatus status(error);
    status.decoded_body_length = body.size();
    loader_factory->AddResponse(url, head, body, status);
    return loader_factory;
  }

 protected:
  TestBrowserThreadBundle thread_bundle_;
  std::unique_ptr<EmbeddedWorkerTestHelper> helper_;

 private:
  DISALLOW_COPY_AND_ASSIGN(ServiceWorkerSingleScriptUpdateCheckerTest);
};

class ServiceWorkerSingleScriptUpdateCheckerToggleAsyncTest
    : public ServiceWorkerSingleScriptUpdateCheckerTest,
      public testing::WithParamInterface<bool> {
 public:
  static bool IsAsync() { return GetParam(); }
};

INSTANTIATE_TEST_SUITE_P(ServiceWorkerSingleScriptUpdateCheckerToggleAsyncTestP,
                         ServiceWorkerSingleScriptUpdateCheckerToggleAsyncTest,
                         testing::Bool());

TEST_P(ServiceWorkerSingleScriptUpdateCheckerToggleAsyncTest,
       Identical_SingleRead) {
  // Response body from the network.
  const std::string body_from_net("abcdef");

  // Stored data for |kScriptURL|.
  const std::vector<std::string> body_from_storage{body_from_net};

  std::unique_ptr<network::TestURLLoaderFactory> loader_factory =
      CreateLoaderFactoryWithRespone(GURL(kScriptURL), kSuccessHeader,
                                     body_from_net, net::OK);

  auto compare_reader = std::make_unique<MockServiceWorkerResponseReader>();
  auto copy_reader = std::make_unique<MockServiceWorkerResponseReader>();
  auto writer = std::make_unique<MockServiceWorkerResponseWriter>();
  MockServiceWorkerResponseReader* compare_reader_rawptr = compare_reader.get();
  compare_reader->ExpectReadOk(body_from_storage, TotalBytes(body_from_storage),
                               IsAsync());

  base::Optional<CheckResult> check_result;
  std::unique_ptr<ServiceWorkerSingleScriptUpdateChecker> checker =
      CreateSingleScriptUpdateCheckerWithoutHttpCache(
          kScriptURL, GURL(kScope), std::move(compare_reader),
          std::move(copy_reader), std::move(writer), loader_factory.get(),
          &check_result);

  if (IsAsync()) {
    // Blocked on reading the header.
    base::RunLoop().RunUntilIdle();
    EXPECT_FALSE(check_result.has_value());

    // Unblock the header, and then blocked on reading the body.
    compare_reader_rawptr->CompletePendingRead();
    base::RunLoop().RunUntilIdle();
    EXPECT_FALSE(check_result.has_value());

    // Unblock the body.
    compare_reader_rawptr->CompletePendingRead();
  }

  // Complete the comparison of the body. It should be identical.
  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(check_result.has_value());
  EXPECT_EQ(check_result.value().result,
            ServiceWorkerSingleScriptUpdateChecker::Result::kIdentical);
  EXPECT_EQ(check_result.value().url, kScriptURL);
  EXPECT_TRUE(compare_reader_rawptr->AllExpectedReadsDone());
}

TEST_P(ServiceWorkerSingleScriptUpdateCheckerToggleAsyncTest,
       Identical_MultipleRead) {
  // Response body from the network.
  const std::string body_from_net("abcdef");

  // Stored data for |kScriptURL|.
  const std::vector<std::string> body_from_storage{"abc", "def"};

  std::unique_ptr<network::TestURLLoaderFactory> loader_factory =
      CreateLoaderFactoryWithRespone(GURL(kScriptURL), kSuccessHeader,
                                     body_from_net, net::OK);

  auto compare_reader = std::make_unique<MockServiceWorkerResponseReader>();
  auto copy_reader = std::make_unique<MockServiceWorkerResponseReader>();
  auto writer = std::make_unique<MockServiceWorkerResponseWriter>();
  MockServiceWorkerResponseReader* compare_reader_rawptr = compare_reader.get();
  compare_reader->ExpectReadOk(body_from_storage, TotalBytes(body_from_storage),
                               IsAsync());

  base::Optional<CheckResult> check_result;
  std::unique_ptr<ServiceWorkerSingleScriptUpdateChecker> checker =
      CreateSingleScriptUpdateCheckerWithoutHttpCache(
          kScriptURL, GURL(kScope), std::move(compare_reader),
          std::move(copy_reader), std::move(writer), loader_factory.get(),
          &check_result);

  if (IsAsync()) {
    // Blocked on reading the header.
    base::RunLoop().RunUntilIdle();
    EXPECT_FALSE(check_result.has_value());

    // Unblock the header, and then blocked on reading the body.
    compare_reader_rawptr->CompletePendingRead();
    base::RunLoop().RunUntilIdle();
    EXPECT_FALSE(check_result.has_value());

    // Unblock the body ("abc").
    compare_reader_rawptr->CompletePendingRead();
    base::RunLoop().RunUntilIdle();
    EXPECT_FALSE(check_result.has_value());

    // Unblock the body ("def").
    compare_reader_rawptr->CompletePendingRead();
  }

  // Complete the comparison of the body. It should be identical.
  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(check_result.has_value());
  EXPECT_EQ(check_result.value().result,
            ServiceWorkerSingleScriptUpdateChecker::Result::kIdentical);
  EXPECT_EQ(check_result.value().url, kScriptURL);
  EXPECT_TRUE(compare_reader_rawptr->AllExpectedReadsDone());
}

TEST_P(ServiceWorkerSingleScriptUpdateCheckerToggleAsyncTest, Identical_Empty) {
  // Response body from the network, which is empty.
  const std::string body_from_net("");

  // Stored data for |kScriptURL| (the data for compare reader).
  const std::vector<std::string> body_from_storage{body_from_net};

  std::unique_ptr<network::TestURLLoaderFactory> loader_factory =
      CreateLoaderFactoryWithRespone(GURL(kScriptURL), kSuccessHeader,
                                     body_from_net, net::OK);

  auto compare_reader = std::make_unique<MockServiceWorkerResponseReader>();
  auto copy_reader = std::make_unique<MockServiceWorkerResponseReader>();
  auto writer = std::make_unique<MockServiceWorkerResponseWriter>();
  MockServiceWorkerResponseReader* compare_reader_rawptr = compare_reader.get();
  compare_reader->ExpectReadOk(body_from_storage, TotalBytes(body_from_storage),
                               IsAsync());

  base::Optional<CheckResult> check_result;
  std::unique_ptr<ServiceWorkerSingleScriptUpdateChecker> checker =
      CreateSingleScriptUpdateCheckerWithoutHttpCache(
          kScriptURL, GURL(kScope), std::move(compare_reader),
          std::move(copy_reader), std::move(writer), loader_factory.get(),
          &check_result);

  if (IsAsync()) {
    // Blocked on reading the header.
    base::RunLoop().RunUntilIdle();
    EXPECT_FALSE(check_result.has_value());

    // Unblock the header. The initial block of the network body is empty, and
    // the empty body is passed to the cache writer. It will finish the
    // comparison immediately.
    compare_reader_rawptr->CompletePendingRead();
  }

  // Both network and storage are empty. The result should be kIdentical.
  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(check_result.has_value());
  EXPECT_EQ(check_result.value().result,
            ServiceWorkerSingleScriptUpdateChecker::Result::kIdentical);
  EXPECT_EQ(check_result.value().url, kScriptURL);
  EXPECT_FALSE(check_result.value().paused_state);
}

TEST_P(ServiceWorkerSingleScriptUpdateCheckerToggleAsyncTest,
       Different_SingleRead_NetworkIsLonger) {
  // Response body from the network.
  const std::string body_from_net = "abcdef";

  // Stored data for |kScriptURL|.
  const std::vector<std::string> body_from_storage{"abc", ""};

  std::unique_ptr<network::TestURLLoaderFactory> loader_factory =
      CreateLoaderFactoryWithRespone(GURL(kScriptURL), kSuccessHeader,
                                     body_from_net, net::OK);

  auto compare_reader = std::make_unique<MockServiceWorkerResponseReader>();
  auto copy_reader = std::make_unique<MockServiceWorkerResponseReader>();
  auto writer = std::make_unique<MockServiceWorkerResponseWriter>();
  MockServiceWorkerResponseReader* compare_reader_rawptr = compare_reader.get();
  compare_reader->ExpectReadOk(body_from_storage, TotalBytes(body_from_storage),
                               IsAsync());

  base::Optional<CheckResult> check_result;
  std::unique_ptr<ServiceWorkerSingleScriptUpdateChecker> checker =
      CreateSingleScriptUpdateCheckerWithoutHttpCache(
          kScriptURL, GURL(kScope), std::move(compare_reader),
          std::move(copy_reader), std::move(writer), loader_factory.get(),
          &check_result);

  if (IsAsync()) {
    // Blocked on reading the header.
    base::RunLoop().RunUntilIdle();
    EXPECT_FALSE(check_result.has_value());

    // Unblock the header, and then blocked on reading the body.
    compare_reader_rawptr->CompletePendingRead();
    base::RunLoop().RunUntilIdle();
    EXPECT_FALSE(check_result.has_value());

    // Unblock the body ("abc").
    compare_reader_rawptr->CompletePendingRead();
    base::RunLoop().RunUntilIdle();
    EXPECT_FALSE(check_result.has_value());

    // Unblock the body from storage (""). The cache writer detects the end of
    // the body from the disk cache.
    compare_reader_rawptr->CompletePendingRead();
  }

  // Complete the comparison of the body. It should be different.
  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(check_result.has_value());
  EXPECT_EQ(check_result.value().result,
            ServiceWorkerSingleScriptUpdateChecker::Result::kDifferent);
  EXPECT_EQ(check_result.value().url, kScriptURL);
  EXPECT_TRUE(compare_reader_rawptr->AllExpectedReadsDone());
}

TEST_P(ServiceWorkerSingleScriptUpdateCheckerToggleAsyncTest,
       Different_SingleRead_StorageIsLonger) {
  // Response body from the network.
  const std::string body_from_net = "abc";

  // Stored data for |kScriptURL|.
  const std::vector<std::string> body_from_storage{"abc", "def"};

  std::unique_ptr<network::TestURLLoaderFactory> loader_factory =
      CreateLoaderFactoryWithRespone(GURL(kScriptURL), kSuccessHeader,
                                     body_from_net, net::OK);

  auto compare_reader = std::make_unique<MockServiceWorkerResponseReader>();
  auto copy_reader = std::make_unique<MockServiceWorkerResponseReader>();
  auto writer = std::make_unique<MockServiceWorkerResponseWriter>();
  MockServiceWorkerResponseReader* compare_reader_rawptr = compare_reader.get();
  compare_reader->ExpectReadOk(body_from_storage, TotalBytes(body_from_storage),
                               IsAsync());

  base::Optional<CheckResult> check_result;
  std::unique_ptr<ServiceWorkerSingleScriptUpdateChecker> checker =
      CreateSingleScriptUpdateCheckerWithoutHttpCache(
          kScriptURL, GURL(kScope), std::move(compare_reader),
          std::move(copy_reader), std::move(writer), loader_factory.get(),
          &check_result);

  if (IsAsync()) {
    // Blocked on reading the header.
    base::RunLoop().RunUntilIdle();
    EXPECT_FALSE(check_result.has_value());

    // Unblock the header, and then blocked on reading the body.
    compare_reader_rawptr->CompletePendingRead();
    base::RunLoop().RunUntilIdle();
    EXPECT_FALSE(check_result.has_value());

    // Unblock the body ("abc"). At this point, data from the network reaches
    // the end.
    compare_reader_rawptr->CompletePendingRead();
  }

  // Complete the comparison of the body. It should be different.
  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(check_result.has_value());
  EXPECT_EQ(check_result.value().result,
            ServiceWorkerSingleScriptUpdateChecker::Result::kDifferent);
  EXPECT_EQ(check_result.value().url, kScriptURL);

  // The update checker realizes that the script is different before reaching
  // the end of the script from the disk cache.
  EXPECT_FALSE(compare_reader_rawptr->AllExpectedReadsDone());
}

TEST_P(ServiceWorkerSingleScriptUpdateCheckerToggleAsyncTest,
       Different_SingleRead_DifferentBody) {
  // Response body from the network.
  const std::string body_from_net = "abc";

  // Stored data for |kScriptURL|.
  const std::vector<std::string> body_from_storage{"abx"};

  std::unique_ptr<network::TestURLLoaderFactory> loader_factory =
      CreateLoaderFactoryWithRespone(GURL(kScriptURL), kSuccessHeader,
                                     body_from_net, net::OK);

  auto compare_reader = std::make_unique<MockServiceWorkerResponseReader>();
  auto copy_reader = std::make_unique<MockServiceWorkerResponseReader>();
  auto writer = std::make_unique<MockServiceWorkerResponseWriter>();
  MockServiceWorkerResponseReader* compare_reader_rawptr = compare_reader.get();
  compare_reader->ExpectReadOk(body_from_storage, TotalBytes(body_from_storage),
                               IsAsync());

  base::Optional<CheckResult> check_result;
  std::unique_ptr<ServiceWorkerSingleScriptUpdateChecker> checker =
      CreateSingleScriptUpdateCheckerWithoutHttpCache(
          kScriptURL, GURL(kScope), std::move(compare_reader),
          std::move(copy_reader), std::move(writer), loader_factory.get(),
          &check_result);

  if (IsAsync()) {
    // Blocked on reading the header.
    base::RunLoop().RunUntilIdle();
    EXPECT_FALSE(check_result.has_value());

    // Unblock the header, and then blocked on reading the body.
    compare_reader_rawptr->CompletePendingRead();
    base::RunLoop().RunUntilIdle();
    EXPECT_FALSE(check_result.has_value());

    // Unblock the body ("abx").
    compare_reader_rawptr->CompletePendingRead();
  }

  // Complete the comparison of the body. It should be different.
  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(check_result.has_value());
  EXPECT_EQ(check_result.value().result,
            ServiceWorkerSingleScriptUpdateChecker::Result::kDifferent);
  EXPECT_EQ(check_result.value().url, kScriptURL);
  EXPECT_TRUE(compare_reader_rawptr->AllExpectedReadsDone());
}

TEST_P(ServiceWorkerSingleScriptUpdateCheckerToggleAsyncTest,
       Different_MultipleRead_NetworkIsLonger) {
  // Response body from the network.
  const std::string body_from_net = "abcdef";

  // Stored data for |kScriptURL| (the data for compare reader).
  const std::vector<std::string> body_from_storage{"ab", "c", ""};

  std::unique_ptr<network::TestURLLoaderFactory> loader_factory =
      CreateLoaderFactoryWithRespone(GURL(kScriptURL), kSuccessHeader,
                                     body_from_net, net::OK);

  auto compare_reader = std::make_unique<MockServiceWorkerResponseReader>();
  auto copy_reader = std::make_unique<MockServiceWorkerResponseReader>();
  auto writer = std::make_unique<MockServiceWorkerResponseWriter>();
  MockServiceWorkerResponseReader* compare_reader_rawptr = compare_reader.get();
  compare_reader->ExpectReadOk(body_from_storage, TotalBytes(body_from_storage),
                               IsAsync());

  base::Optional<CheckResult> check_result;
  std::unique_ptr<ServiceWorkerSingleScriptUpdateChecker> checker =
      CreateSingleScriptUpdateCheckerWithoutHttpCache(
          kScriptURL, GURL(kScope), std::move(compare_reader),
          std::move(copy_reader), std::move(writer), loader_factory.get(),
          &check_result);

  if (IsAsync()) {
    // Blocked on reading the header.
    base::RunLoop().RunUntilIdle();
    EXPECT_FALSE(check_result.has_value());

    // Unblock the header, and then blocked on reading the body.
    compare_reader_rawptr->CompletePendingRead();
    base::RunLoop().RunUntilIdle();
    EXPECT_FALSE(check_result.has_value());

    // Unblock the body from storage ("ab"), and then blocked on reading the
    // body again.
    compare_reader_rawptr->CompletePendingRead();
    base::RunLoop().RunUntilIdle();
    EXPECT_FALSE(check_result.has_value());

    // Unblock the body from storage ("c"), and then blocked on reading the body
    // again.
    compare_reader_rawptr->CompletePendingRead();
    base::RunLoop().RunUntilIdle();
    EXPECT_FALSE(check_result.has_value());

    // Unblock the body from storage (""). The cache writer detects the end of
    // the body from the disk cache.
    compare_reader_rawptr->CompletePendingRead();
  }

  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(check_result.has_value());
  EXPECT_EQ(check_result.value().result,
            ServiceWorkerSingleScriptUpdateChecker::Result::kDifferent);
  EXPECT_EQ(check_result.value().url, kScriptURL);
  EXPECT_TRUE(compare_reader_rawptr->AllExpectedReadsDone());
}

TEST_P(ServiceWorkerSingleScriptUpdateCheckerToggleAsyncTest,
       Different_MultipleRead_StorageIsLonger) {
  // Response body from the network.
  const std::string body_from_net = "abc";

  // Stored data for |kScriptURL| (the data for compare reader).
  const std::vector<std::string> body_from_storage{"ab", "c", "def"};

  std::unique_ptr<network::TestURLLoaderFactory> loader_factory =
      CreateLoaderFactoryWithRespone(GURL(kScriptURL), kSuccessHeader,
                                     body_from_net, net::OK);

  auto compare_reader = std::make_unique<MockServiceWorkerResponseReader>();
  auto copy_reader = std::make_unique<MockServiceWorkerResponseReader>();
  auto writer = std::make_unique<MockServiceWorkerResponseWriter>();
  MockServiceWorkerResponseReader* compare_reader_rawptr = compare_reader.get();
  compare_reader->ExpectReadOk(body_from_storage, TotalBytes(body_from_storage),
                               IsAsync());

  base::Optional<CheckResult> check_result;
  std::unique_ptr<ServiceWorkerSingleScriptUpdateChecker> checker =
      CreateSingleScriptUpdateCheckerWithoutHttpCache(
          kScriptURL, GURL(kScope), std::move(compare_reader),
          std::move(copy_reader), std::move(writer), loader_factory.get(),
          &check_result);

  if (IsAsync()) {
    // Blocked on reading the header.
    base::RunLoop().RunUntilIdle();
    EXPECT_FALSE(check_result.has_value());

    // Unblock the header, and then blocked on reading the body.
    compare_reader_rawptr->CompletePendingRead();
    base::RunLoop().RunUntilIdle();
    EXPECT_FALSE(check_result.has_value());

    // Unblock the body from storage ("ab"), and then blocked on reading the
    // body again.
    compare_reader_rawptr->CompletePendingRead();
    base::RunLoop().RunUntilIdle();
    EXPECT_FALSE(check_result.has_value());

    // Unblock the body from storage ("c"). At this point, data from the network
    // reaches the end.
    compare_reader_rawptr->CompletePendingRead();
  }

  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(check_result.has_value());
  EXPECT_EQ(check_result.value().result,
            ServiceWorkerSingleScriptUpdateChecker::Result::kDifferent);
  EXPECT_EQ(check_result.value().url, kScriptURL);

  // The update checker realizes that the script is different before reaching
  // the end of the script from the disk cache.
  EXPECT_FALSE(compare_reader_rawptr->AllExpectedReadsDone());
}

TEST_P(ServiceWorkerSingleScriptUpdateCheckerToggleAsyncTest,
       Different_MultipleRead_DifferentBody) {
  // Response body from the network.
  const std::string body_from_net = "abc";

  // Stored data for |kScriptURL| (the data for compare reader).
  const std::vector<std::string> body_from_storage{"ab", "x"};

  std::unique_ptr<network::TestURLLoaderFactory> loader_factory =
      CreateLoaderFactoryWithRespone(GURL(kScriptURL), kSuccessHeader,
                                     body_from_net, net::OK);

  auto compare_reader = std::make_unique<MockServiceWorkerResponseReader>();
  auto copy_reader = std::make_unique<MockServiceWorkerResponseReader>();
  auto writer = std::make_unique<MockServiceWorkerResponseWriter>();
  MockServiceWorkerResponseReader* compare_reader_rawptr = compare_reader.get();
  compare_reader->ExpectReadOk(body_from_storage, TotalBytes(body_from_storage),
                               IsAsync());

  base::Optional<CheckResult> check_result;
  std::unique_ptr<ServiceWorkerSingleScriptUpdateChecker> checker =
      CreateSingleScriptUpdateCheckerWithoutHttpCache(
          kScriptURL, GURL(kScope), std::move(compare_reader),
          std::move(copy_reader), std::move(writer), loader_factory.get(),
          &check_result);

  if (IsAsync()) {
    // Blocked on reading the header.
    base::RunLoop().RunUntilIdle();
    EXPECT_FALSE(check_result.has_value());

    // Unblock the header, and then blocked on reading the body.
    compare_reader_rawptr->CompletePendingRead();
    base::RunLoop().RunUntilIdle();
    EXPECT_FALSE(check_result.has_value());

    // Unblock the body from storage ("ab"), and then blocked on reading the
    // body again.
    compare_reader_rawptr->CompletePendingRead();
    base::RunLoop().RunUntilIdle();
    EXPECT_FALSE(check_result.has_value());

    // Unblock the body from storage ("x"), which is different from the body
    // from the network.
    compare_reader_rawptr->CompletePendingRead();
  }

  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(check_result.has_value());
  EXPECT_EQ(check_result.value().result,
            ServiceWorkerSingleScriptUpdateChecker::Result::kDifferent);
  EXPECT_EQ(check_result.value().url, kScriptURL);
  EXPECT_TRUE(compare_reader_rawptr->AllExpectedReadsDone());
}

TEST_F(ServiceWorkerSingleScriptUpdateCheckerTest,
       PendingReadWithErrorStatusShouldNotLeak) {
  // Response body from the network.
  const std::string body_from_net("abc");

  // Stored data for |kScriptURL| (the data for compare reader).
  const std::vector<std::string> body_from_storage{"ab", "c"};

  auto loader_factory = std::make_unique<network::TestURLLoaderFactory>();
  auto compare_reader = std::make_unique<MockServiceWorkerResponseReader>();
  auto copy_reader = std::make_unique<MockServiceWorkerResponseReader>();
  auto writer = std::make_unique<MockServiceWorkerResponseWriter>();
  MockServiceWorkerResponseReader* compare_reader_rawptr = compare_reader.get();
  compare_reader->ExpectReadOk(body_from_storage, TotalBytes(body_from_storage),
                               /*async=*/true);

  base::Optional<CheckResult> check_result;
  std::unique_ptr<ServiceWorkerSingleScriptUpdateChecker> checker =
      CreateSingleScriptUpdateCheckerWithoutHttpCache(
          kScriptURL, GURL(kScope), std::move(compare_reader),
          std::move(copy_reader), std::move(writer), loader_factory.get(),
          &check_result);

  // The update checker sends a request to the loader. The testing loader keeps
  // the request.
  base::RunLoop().RunUntilIdle();
  network::TestURLLoaderFactory::PendingRequest* request =
      loader_factory->GetPendingRequest(0);
  ASSERT_TRUE(request);

  // Simulate to send the head and the body back to the checker.
  // Note that OnComplete() is not called yet.
  {
    network::ResourceResponseHead head =
        network::CreateResourceResponseHead(net::HTTP_OK);
    head.headers = base::MakeRefCounted<net::HttpResponseHeaders>(
        net::HttpUtil::AssembleRawHeaders(kSuccessHeader));
    head.headers->GetMimeType(&head.mime_type);
    request->client->OnReceiveResponse(head);

    MojoCreateDataPipeOptions options;
    options.struct_size = sizeof(MojoCreateDataPipeOptions);
    options.flags = MOJO_CREATE_DATA_PIPE_FLAG_NONE;
    options.element_num_bytes = 1;
    options.capacity_num_bytes = body_from_net.size();
    mojo::ScopedDataPipeConsumerHandle consumer;
    mojo::ScopedDataPipeProducerHandle producer;
    EXPECT_EQ(MOJO_RESULT_OK,
              mojo::CreateDataPipe(&options, &producer, &consumer));
    uint32_t bytes_written = body_from_net.size();
    EXPECT_EQ(MOJO_RESULT_OK,
              producer->WriteData(body_from_net.data(), &bytes_written,
                                  MOJO_WRITE_DATA_FLAG_ALL_OR_NONE));
    request->client->OnStartLoadingResponseBody(std::move(consumer));
  }

  // Blocked on reading the header from the storage due to the asynchronous
  // read.
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(check_result.has_value());

  // Update check stops in CompareReader() due to the asynchronous read of the
  // |compare_reader|.
  compare_reader_rawptr->CompletePendingRead();
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(check_result.has_value());

  // Return failed status code at this point. The update checker will throw the
  // internal state away.
  request->client->OnComplete(
      network::URLLoaderCompletionStatus(net::ERR_ABORTED));
  base::RunLoop().RunUntilIdle();

  // Resume the pending read. This should not crash and return kFailed.
  compare_reader_rawptr->CompletePendingRead();
  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(check_result.has_value());
  EXPECT_EQ(ServiceWorkerSingleScriptUpdateChecker::Result::kFailed,
            check_result.value().result);
}

// Tests cache validation behavior when updateViaCache is 'all'.
TEST_F(ServiceWorkerSingleScriptUpdateCheckerTest, UpdateViaCache_All) {
  auto loader_factory = std::make_unique<network::TestURLLoaderFactory>();
  base::Optional<CheckResult> check_result;

  // Load the main script. Should not validate the cache.
  std::unique_ptr<ServiceWorkerSingleScriptUpdateChecker> checker =
      CreateSingleScriptUpdateChecker(
          kScriptURL, GURL(kScope), true /* is_main_script */,
          false /* force_bypass_cache */,
          blink::mojom::ServiceWorkerUpdateViaCache::kAll, base::TimeDelta(),
          std::make_unique<MockServiceWorkerResponseReader>(),
          std::make_unique<MockServiceWorkerResponseReader>(),
          std::make_unique<MockServiceWorkerResponseWriter>(),
          loader_factory.get(), &check_result);

  const network::ResourceRequest* request = nullptr;
  ASSERT_TRUE(loader_factory->IsPending(kScriptURL, &request));
  EXPECT_FALSE(request->load_flags & net::LOAD_VALIDATE_CACHE);

  // Load imported script. Should not validate the cache.
  checker = CreateSingleScriptUpdateChecker(
      kImportedScriptURL, GURL(kScope), false /* is_main_script */,
      false /* force_bypass_cache */,
      blink::mojom::ServiceWorkerUpdateViaCache::kAll, base::TimeDelta(),
      std::make_unique<MockServiceWorkerResponseReader>(),
      std::make_unique<MockServiceWorkerResponseReader>(),
      std::make_unique<MockServiceWorkerResponseWriter>(), loader_factory.get(),
      &check_result);

  ASSERT_TRUE(loader_factory->IsPending(kImportedScriptURL, &request));
  EXPECT_FALSE(request->load_flags & net::LOAD_VALIDATE_CACHE);
}

// Tests cache validation behavior when updateViaCache is 'none'.
TEST_F(ServiceWorkerSingleScriptUpdateCheckerTest, UpdateViaCache_None) {
  auto loader_factory = std::make_unique<network::TestURLLoaderFactory>();
  base::Optional<CheckResult> check_result;

  // Load the main script. Should validate the cache.
  std::unique_ptr<ServiceWorkerSingleScriptUpdateChecker> checker =
      CreateSingleScriptUpdateChecker(
          kScriptURL, GURL(kScope), true /* is_main_script */,
          false /* force_bypass_cache */,
          blink::mojom::ServiceWorkerUpdateViaCache::kNone, base::TimeDelta(),
          std::make_unique<MockServiceWorkerResponseReader>(),
          std::make_unique<MockServiceWorkerResponseReader>(),
          std::make_unique<MockServiceWorkerResponseWriter>(),
          loader_factory.get(), &check_result);

  const network::ResourceRequest* request = nullptr;
  ASSERT_TRUE(loader_factory->IsPending(kScriptURL, &request));
  EXPECT_TRUE(request->load_flags & net::LOAD_VALIDATE_CACHE);

  // Load imported script. Should validate the cache.
  checker = CreateSingleScriptUpdateChecker(
      kImportedScriptURL, GURL(kScope), false /* is_main_script */,
      false /* force_bypass_cache */,
      blink::mojom::ServiceWorkerUpdateViaCache::kNone, base::TimeDelta(),
      std::make_unique<MockServiceWorkerResponseReader>(),
      std::make_unique<MockServiceWorkerResponseReader>(),
      std::make_unique<MockServiceWorkerResponseWriter>(), loader_factory.get(),
      &check_result);

  ASSERT_TRUE(loader_factory->IsPending(kImportedScriptURL, &request));
  EXPECT_TRUE(request->load_flags & net::LOAD_VALIDATE_CACHE);
}

// Tests cache validation behavior when updateViaCache is 'imports'.
TEST_F(ServiceWorkerSingleScriptUpdateCheckerTest, UpdateViaCache_Imports) {
  auto loader_factory = std::make_unique<network::TestURLLoaderFactory>();
  base::Optional<CheckResult> check_result;

  // Load main script. Should validate the cache.
  std::unique_ptr<ServiceWorkerSingleScriptUpdateChecker> checker =
      CreateSingleScriptUpdateChecker(
          kScriptURL, GURL(kScope), true /* is_main_script */,
          false /* force_bypass_cache */,
          blink::mojom::ServiceWorkerUpdateViaCache::kImports,
          base::TimeDelta(),
          std::make_unique<MockServiceWorkerResponseReader>(),
          std::make_unique<MockServiceWorkerResponseReader>(),
          std::make_unique<MockServiceWorkerResponseWriter>(),
          loader_factory.get(), &check_result);

  const network::ResourceRequest* request = nullptr;
  ASSERT_TRUE(loader_factory->IsPending(kScriptURL, &request));
  EXPECT_TRUE(request->load_flags & net::LOAD_VALIDATE_CACHE);

  // Load imported script. Should not validate the cache.
  checker = CreateSingleScriptUpdateChecker(
      kImportedScriptURL, GURL(kScope), false /* is_main_script */,
      false /* force_bypass_cache */,
      blink::mojom::ServiceWorkerUpdateViaCache::kImports, base::TimeDelta(),
      std::make_unique<MockServiceWorkerResponseReader>(),
      std::make_unique<MockServiceWorkerResponseReader>(),
      std::make_unique<MockServiceWorkerResponseWriter>(), loader_factory.get(),
      &check_result);

  ASSERT_TRUE(loader_factory->IsPending(kImportedScriptURL, &request));
  EXPECT_FALSE(request->load_flags & net::LOAD_VALIDATE_CACHE);
}

// Tests cache validation behavior when version's
// |force_bypass_cache_for_scripts_| is true.
TEST_F(ServiceWorkerSingleScriptUpdateCheckerTest, ForceBypassCache) {
  auto loader_factory = std::make_unique<network::TestURLLoaderFactory>();
  base::Optional<CheckResult> check_result;

  // Load main script. Should validate the cache.
  std::unique_ptr<ServiceWorkerSingleScriptUpdateChecker> checker =
      CreateSingleScriptUpdateChecker(
          kScriptURL, GURL(kScope), true /* is_main_script */,
          true /* force_bypass_cache */,
          blink::mojom::ServiceWorkerUpdateViaCache::kAll, base::TimeDelta(),
          std::make_unique<MockServiceWorkerResponseReader>(),
          std::make_unique<MockServiceWorkerResponseReader>(),
          std::make_unique<MockServiceWorkerResponseWriter>(),
          loader_factory.get(), &check_result);

  const network::ResourceRequest* request = nullptr;
  ASSERT_TRUE(loader_factory->IsPending(kScriptURL, &request));
  EXPECT_TRUE(request->load_flags & net::LOAD_VALIDATE_CACHE);

  // Load imported script. Should validate the cache.
  checker = CreateSingleScriptUpdateChecker(
      kImportedScriptURL, GURL(kScope), false /* is_main_script */,
      true /* force_bypass_cache */,
      blink::mojom::ServiceWorkerUpdateViaCache::kAll, base::TimeDelta(),
      std::make_unique<MockServiceWorkerResponseReader>(),
      std::make_unique<MockServiceWorkerResponseReader>(),
      std::make_unique<MockServiceWorkerResponseWriter>(), loader_factory.get(),
      &check_result);

  ASSERT_TRUE(loader_factory->IsPending(kImportedScriptURL, &request));
  EXPECT_TRUE(request->load_flags & net::LOAD_VALIDATE_CACHE);
}

// Tests cache validation behavior when more than 24 hours passed.
TEST_F(ServiceWorkerSingleScriptUpdateCheckerTest, MoreThan24Hours) {
  auto loader_factory = std::make_unique<network::TestURLLoaderFactory>();
  base::Optional<CheckResult> check_result;

  // Load main script. Should validate the cache.
  std::unique_ptr<ServiceWorkerSingleScriptUpdateChecker> checker =
      CreateSingleScriptUpdateChecker(
          kScriptURL, GURL(kScope), true /* is_main_script */,
          false /* force_bypass_cache */,
          blink::mojom::ServiceWorkerUpdateViaCache::kAll,
          base::TimeDelta::FromDays(1) + base::TimeDelta::FromHours(1),
          std::make_unique<MockServiceWorkerResponseReader>(),
          std::make_unique<MockServiceWorkerResponseReader>(),
          std::make_unique<MockServiceWorkerResponseWriter>(),
          loader_factory.get(), &check_result);

  const network::ResourceRequest* request = nullptr;
  ASSERT_TRUE(loader_factory->IsPending(kScriptURL, &request));
  EXPECT_TRUE(request->load_flags & net::LOAD_VALIDATE_CACHE);

  // Load imported script. Should validate the cache.
  checker = CreateSingleScriptUpdateChecker(
      kImportedScriptURL, GURL(kScope), false /* is_main_script */,
      false /* force_bypass_cache */,
      blink::mojom::ServiceWorkerUpdateViaCache::kAll,
      base::TimeDelta::FromDays(1) + base::TimeDelta::FromHours(1),
      std::make_unique<MockServiceWorkerResponseReader>(),
      std::make_unique<MockServiceWorkerResponseReader>(),
      std::make_unique<MockServiceWorkerResponseWriter>(), loader_factory.get(),
      &check_result);

  ASSERT_TRUE(loader_factory->IsPending(kImportedScriptURL, &request));
  EXPECT_TRUE(request->load_flags & net::LOAD_VALIDATE_CACHE);
}

// Tests MIME type header checking.
TEST_F(ServiceWorkerSingleScriptUpdateCheckerTest, MimeTypeError) {
  // Response body from the network.
  const std::string kBodyFromNet = "abcdef";

  // It should report error for no/bad MIME types.
  const char* kNoMimeHeader = "HTTP/1.1 200 OK\n\n";
  const char* kBadMimeHeader =
      "HTTP/1.1 200 OK\n"
      "Content-Type: text/css\n\n";
  const std::string headers[] = {kNoMimeHeader, kBadMimeHeader};

  for (const std::string& header : headers) {
    std::unique_ptr<network::TestURLLoaderFactory> loader_factory =
        CreateLoaderFactoryWithRespone(GURL(kScriptURL), header, kBodyFromNet,
                                       net::OK);

    auto compare_reader = std::make_unique<MockServiceWorkerResponseReader>();
    auto copy_reader = std::make_unique<MockServiceWorkerResponseReader>();
    auto writer = std::make_unique<MockServiceWorkerResponseWriter>();

    base::Optional<CheckResult> check_result;
    std::unique_ptr<ServiceWorkerSingleScriptUpdateChecker> checker =
        CreateSingleScriptUpdateChecker(
            kScriptURL, GURL(kScope), true /* is_main_script */,
            false /* force_bypass_cache */,
            blink::mojom::ServiceWorkerUpdateViaCache::kNone, base::TimeDelta(),
            std::move(compare_reader), std::move(copy_reader),
            std::move(writer), loader_factory.get(), &check_result);
    base::RunLoop().RunUntilIdle();

    EXPECT_TRUE(check_result.has_value());
    EXPECT_EQ(check_result.value().result,
              ServiceWorkerSingleScriptUpdateChecker::Result::kFailed);
    EXPECT_EQ(check_result.value().failure_info->status,
              blink::ServiceWorkerStatusCode::kErrorSecurity);
  }
}

// Tests path restriction check error for main script.
// |kOutScope| is not under the default scope ("/in-scope/") and the
// Service-Worker-Allowed header is not specified. The check should fail.
TEST_F(ServiceWorkerSingleScriptUpdateCheckerTest, PathRestrictionError) {
  // Response body from the network.
  const std::string kBodyFromNet = "abcdef";
  const char kMainScriptURL[] = "https://example.com/in-scope/worker.js";
  const char kOutScope[] = "https://example.com/out-scope/";
  const char kHeader[] =
      "HTTP/1.1 200 OK\n"
      "Content-Type: text/javascript\n\n";
  std::unique_ptr<network::TestURLLoaderFactory> loader_factory =
      CreateLoaderFactoryWithRespone(GURL(kMainScriptURL), kHeader,
                                     kBodyFromNet, net::OK);

  auto compare_reader = std::make_unique<MockServiceWorkerResponseReader>();
  auto copy_reader = std::make_unique<MockServiceWorkerResponseReader>();
  auto writer = std::make_unique<MockServiceWorkerResponseWriter>();

  base::Optional<CheckResult> check_result;
  std::unique_ptr<ServiceWorkerSingleScriptUpdateChecker> checker =
      CreateSingleScriptUpdateChecker(
          kMainScriptURL, GURL(kOutScope), true /* is_main_script */,
          false /* force_bypass_cache */,
          blink::mojom::ServiceWorkerUpdateViaCache::kNone, base::TimeDelta(),
          std::move(compare_reader), std::move(copy_reader), std::move(writer),
          loader_factory.get(), &check_result);
  base::RunLoop().RunUntilIdle();

  EXPECT_TRUE(check_result.has_value());
  EXPECT_EQ(check_result.value().result,
            ServiceWorkerSingleScriptUpdateChecker::Result::kFailed);
  EXPECT_EQ(check_result.value().failure_info->status,
            blink::ServiceWorkerStatusCode::kErrorSecurity);
}

// Tests path restriction check success for main script.
// |kOutScope| is not under the default scope ("/in-scope/") but the
// Service-Worker-Allowed header allows it. The check should pass.
TEST_F(ServiceWorkerSingleScriptUpdateCheckerTest, PathRestrictionPass) {
  // Response body from the network.
  const std::string body_from_net("abcdef");
  const char kMainScriptURL[] = "https://example.com/in-scope/worker.js";
  const char kOutScope[] = "https://example.com/out-scope/";
  const char kHeader[] =
      "HTTP/1.1 200 OK\n"
      "Content-Type: text/javascript\n"
      "Service-Worker-Allowed: /out-scope/\n\n";

  // Stored data for |kMainScriptURL|.
  const std::vector<std::string> body_from_storage{body_from_net};

  std::unique_ptr<network::TestURLLoaderFactory> loader_factory =
      CreateLoaderFactoryWithRespone(GURL(kMainScriptURL), kHeader,
                                     body_from_net, net::OK);

  auto compare_reader = std::make_unique<MockServiceWorkerResponseReader>();
  auto copy_reader = std::make_unique<MockServiceWorkerResponseReader>();
  auto writer = std::make_unique<MockServiceWorkerResponseWriter>();
  MockServiceWorkerResponseReader* compare_reader_rawptr = compare_reader.get();
  compare_reader->ExpectReadOk(body_from_storage, TotalBytes(body_from_storage),
                               false /* async */);

  base::Optional<CheckResult> check_result;
  std::unique_ptr<ServiceWorkerSingleScriptUpdateChecker> checker =
      CreateSingleScriptUpdateChecker(
          kMainScriptURL, GURL(kOutScope), true /* is_main_script */,
          false /* force_bypass_cache */,
          blink::mojom::ServiceWorkerUpdateViaCache::kNone, base::TimeDelta(),
          std::move(compare_reader), std::move(copy_reader), std::move(writer),
          loader_factory.get(), &check_result);
  base::RunLoop().RunUntilIdle();

  EXPECT_TRUE(check_result.has_value());
  EXPECT_EQ(check_result.value().result,
            ServiceWorkerSingleScriptUpdateChecker::Result::kIdentical);
  EXPECT_EQ(check_result.value().url, kMainScriptURL);
  EXPECT_EQ(check_result.value().failure_info, nullptr);
  EXPECT_TRUE(compare_reader_rawptr->AllExpectedReadsDone());
}

// Tests network error is reported.
TEST_F(ServiceWorkerSingleScriptUpdateCheckerTest, NetworkError) {
  // Response body from the network.
  const std::string kBodyFromNet = "abcdef";
  const char kFailHeader[] = "HTTP/1.1 404 Not Found\n\n";
  std::unique_ptr<network::TestURLLoaderFactory> loader_factory =
      CreateLoaderFactoryWithRespone(GURL(kScriptURL), kFailHeader,
                                     kBodyFromNet, net::OK);

  auto compare_reader = std::make_unique<MockServiceWorkerResponseReader>();
  auto copy_reader = std::make_unique<MockServiceWorkerResponseReader>();
  auto writer = std::make_unique<MockServiceWorkerResponseWriter>();

  base::Optional<CheckResult> check_result;
  std::unique_ptr<ServiceWorkerSingleScriptUpdateChecker> checker =
      CreateSingleScriptUpdateChecker(
          kScriptURL, GURL(kScope), true /* is_main_script */,
          false /* force_bypass_cache */,
          blink::mojom::ServiceWorkerUpdateViaCache::kNone, base::TimeDelta(),
          std::move(compare_reader), std::move(copy_reader), std::move(writer),
          loader_factory.get(), &check_result);
  base::RunLoop().RunUntilIdle();

  EXPECT_TRUE(check_result.has_value());
  EXPECT_EQ(check_result.value().result,
            ServiceWorkerSingleScriptUpdateChecker::Result::kFailed);
  EXPECT_EQ(check_result.value().failure_info->status,
            blink::ServiceWorkerStatusCode::kErrorNetwork);
}

}  // namespace
}  // namespace content
