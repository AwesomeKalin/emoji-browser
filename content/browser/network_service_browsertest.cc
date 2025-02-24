// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/bind.h"
#include "base/files/file_util.h"
#include "base/memory/ref_counted_memory.h"
#include "base/test/bind_test_util.h"
#include "base/test/scoped_feature_list.h"
#include "build/build_config.h"
#include "content/browser/storage_partition_impl.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/network_service_instance.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_ui_controller.h"
#include "content/public/browser/web_ui_controller_factory.h"
#include "content/public/common/content_features.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/network_service_util.h"
#include "content/public/common/service_manager_connection.h"
#include "content/public/common/service_names.mojom.h"
#include "content/public/common/url_utils.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/content_browser_test.h"
#include "content/public/test/content_browser_test_utils.h"
#include "content/public/test/simple_url_loader_test_helper.h"
#include "content/public/test/test_utils.h"
#include "content/shell/browser/shell.h"
#include "content/test/content_browser_test_utils_internal.h"
#include "net/dns/mock_host_resolver.h"
#include "net/http/http_response_headers.h"
#include "net/test/embedded_test_server/default_handlers.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "services/network/public/cpp/features.h"
#include "services/network/public/cpp/network_switches.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/network_service_test.mojom.h"
#include "services/service_manager/public/cpp/connector.h"

#if defined(OS_ANDROID)
#include "base/android/application_status_listener.h"
#endif

namespace content {

namespace {

class WebUITestWebUIControllerFactory : public WebUIControllerFactory {
 public:
  std::unique_ptr<WebUIController> CreateWebUIControllerForURL(
      WebUI* web_ui,
      const GURL& url) const override {
    std::string foo(url.path());
    if (url.path() == "/nobinding/")
      web_ui->SetBindings(0);
    return HasWebUIScheme(url) ? std::make_unique<WebUIController>(web_ui)
                               : nullptr;
  }
  WebUI::TypeID GetWebUIType(BrowserContext* browser_context,
                             const GURL& url) const override {
    return HasWebUIScheme(url) ? reinterpret_cast<WebUI::TypeID>(1) : nullptr;
  }
  bool UseWebUIForURL(BrowserContext* browser_context,
                      const GURL& url) const override {
    return HasWebUIScheme(url);
  }
  bool UseWebUIBindingsForURL(BrowserContext* browser_context,
                              const GURL& url) const override {
    return HasWebUIScheme(url);
  }
};

class TestWebUIDataSource : public URLDataSource {
 public:
  TestWebUIDataSource() {}
  ~TestWebUIDataSource() override {}

  std::string GetSource() override { return "webui"; }

  void StartDataRequest(
      const std::string& path,
      const ResourceRequestInfo::WebContentsGetter& wc_getter,
      const URLDataSource::GotDataCallback& callback) override {
    std::string dummy_html = "<html><body>Foo</body></html>";
    scoped_refptr<base::RefCountedString> response =
        base::RefCountedString::TakeString(&dummy_html);
    callback.Run(response.get());
  }

  std::string GetMimeType(const std::string& path) override {
    return "text/html";
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(TestWebUIDataSource);
};

class NetworkServiceBrowserTest : public ContentBrowserTest {
 public:
  NetworkServiceBrowserTest() {
    scoped_feature_list_.InitAndEnableFeature(
        network::features::kNetworkService);
    EXPECT_TRUE(embedded_test_server()->Start());
    EXPECT_TRUE(temp_dir_.CreateUniqueTempDir());

    WebUIControllerFactory::RegisterFactory(&factory_);
  }

  bool ExecuteScript(const std::string& script) {
    bool xhr_result = false;
    // The JS call will fail if disallowed because the process will be killed.
    bool execute_result =
        ExecuteScriptAndExtractBool(shell(), script, &xhr_result);
    return xhr_result && execute_result;
  }

  bool FetchResource(const GURL& url, bool synchronous = false) {
    if (!url.is_valid())
      return false;
    std::string script = JsReplace(
        "var xhr = new XMLHttpRequest();"
        "xhr.open('GET', $1, $2);"
        "xhr.onload = function (e) {"
        "  if (xhr.readyState === 4) {"
        "    window.domAutomationController.send(xhr.status === 200);"
        "  }"
        "};"
        "xhr.onerror = function () {"
        "  window.domAutomationController.send(false);"
        "};"
        "try {"
        "  xhr.send(null);"
        "} catch (error) {"
        "  window.domAutomationController.send(false);"
        "}",
        url, !synchronous);
    return ExecuteScript(script);
  }

  bool CheckCanLoadHttp() {
    return FetchResource(embedded_test_server()->GetURL("/echo"));
  }

  void SetUpOnMainThread() override {
    URLDataSource::Add(shell()->web_contents()->GetBrowserContext(),
                       std::make_unique<TestWebUIDataSource>());
  }

  void SetUpCommandLine(base::CommandLine* command_line) override {
    // Since we assume exploited renderer process, it can bypass the same origin
    // policy at will. Simulate that by passing the disable-web-security flag.
    command_line->AppendSwitch(switches::kDisableWebSecurity);
    IsolateAllSitesForTesting(command_line);
  }

  base::FilePath GetCacheDirectory() { return temp_dir_.GetPath(); }

  base::FilePath GetCacheIndexDirectory() {
    return GetCacheDirectory().AppendASCII("index-dir");
  }

  void LoadURL(const GURL& url,
               network::mojom::URLLoaderFactory* loader_factory) {
    std::unique_ptr<network::ResourceRequest> request =
        std::make_unique<network::ResourceRequest>();
    request->url = url;
    SimpleURLLoaderTestHelper simple_loader_helper;
    std::unique_ptr<network::SimpleURLLoader> simple_loader =
        network::SimpleURLLoader::Create(std::move(request),
                                         TRAFFIC_ANNOTATION_FOR_TESTS);

    simple_loader->DownloadToStringOfUnboundedSizeUntilCrashAndDie(
        loader_factory, simple_loader_helper.GetCallback());
    simple_loader_helper.WaitForCallback();
    ASSERT_TRUE(simple_loader_helper.response_body());
  }

 private:
  WebUITestWebUIControllerFactory factory_;
  base::test::ScopedFeatureList scoped_feature_list_;
  base::ScopedTempDir temp_dir_;

  DISALLOW_COPY_AND_ASSIGN(NetworkServiceBrowserTest);
};

// Verifies that WebUI pages with WebUI bindings can't make network requests.
IN_PROC_BROWSER_TEST_F(NetworkServiceBrowserTest, WebUIBindingsNoHttp) {
  GURL test_url(GetWebUIURL("webui/"));
  NavigateToURL(shell(), test_url);
  RenderProcessHostKillWaiter kill_waiter(
      shell()->web_contents()->GetMainFrame()->GetProcess());
  ASSERT_FALSE(CheckCanLoadHttp());
  EXPECT_EQ(bad_message::WEBUI_BAD_SCHEME_ACCESS, kill_waiter.Wait());
}

// Verifies that WebUI pages without WebUI bindings can make network requests.
IN_PROC_BROWSER_TEST_F(NetworkServiceBrowserTest, NoWebUIBindingsHttp) {
  GURL test_url(GetWebUIURL("webui/nobinding/"));
  NavigateToURL(shell(), test_url);
  ASSERT_TRUE(CheckCanLoadHttp());
}

// Verifies the filesystem URLLoaderFactory's check, using
// ChildProcessSecurityPolicyImpl::CanRequestURL is properly rejected.
IN_PROC_BROWSER_TEST_F(NetworkServiceBrowserTest,
                       FileSystemBindingsCorrectOrigin) {
  GURL test_url(GetWebUIURL("webui/nobinding/"));
  NavigateToURL(shell(), test_url);

  // Note: must be filesystem scheme (obviously).
  //       file: is not a safe web scheme (see IsWebSafeScheme),
  //       and /etc/passwd fails the CanCommitURL check.
  GURL file_url("filesystem:file:///etc/passwd");
  EXPECT_FALSE(FetchResource(file_url));
}

IN_PROC_BROWSER_TEST_F(NetworkServiceBrowserTest,
                       SimpleUrlLoader_NoAuthWhenNoWebContents) {
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = embedded_test_server()->GetURL("/auth-basic?password=");
  auto loader = network::SimpleURLLoader::Create(std::move(request),
                                                 TRAFFIC_ANNOTATION_FOR_TESTS);
  auto loader_factory = BrowserContext::GetDefaultStoragePartition(
                            shell()->web_contents()->GetBrowserContext())
                            ->GetURLLoaderFactoryForBrowserProcess();
  scoped_refptr<net::HttpResponseHeaders> headers;
  base::RunLoop loop;
  loader->DownloadHeadersOnly(
      loader_factory.get(),
      base::BindOnce(
          [](base::OnceClosure quit_closure,
             scoped_refptr<net::HttpResponseHeaders>* rh_out,
             scoped_refptr<net::HttpResponseHeaders> rh_in) {
            *rh_out = rh_in;
            std::move(quit_closure).Run();
          },
          loop.QuitClosure(), &headers));
  loop.Run();
  ASSERT_TRUE(headers.get());
  ASSERT_EQ(headers->response_code(), 401);
}

#if defined(OS_ANDROID)
IN_PROC_BROWSER_TEST_F(NetworkServiceBrowserTest,
                       HttpCacheWrittenToDiskOnApplicationStateChange) {
  base::ScopedAllowBlockingForTesting allow_blocking;

  // Create network context with cache pointing to the temp cache dir.
  network::mojom::NetworkContextPtr network_context;
  network::mojom::NetworkContextParamsPtr context_params =
      network::mojom::NetworkContextParams::New();
  context_params->http_cache_path = GetCacheDirectory();
  GetNetworkService()->CreateNetworkContext(mojo::MakeRequest(&network_context),
                                            std::move(context_params));

  network::mojom::URLLoaderFactoryParamsPtr params =
      network::mojom::URLLoaderFactoryParams::New();
  params->process_id = network::mojom::kBrowserProcessId;
  params->is_corb_enabled = false;
  network::mojom::URLLoaderFactoryPtr loader_factory;
  network_context->CreateURLLoaderFactory(mojo::MakeRequest(&loader_factory),
                                          std::move(params));

  // Load a URL and check the cache index size.
  LoadURL(embedded_test_server()->GetURL("/cachetime"), loader_factory.get());
  int64_t directory_size = base::ComputeDirectorySize(GetCacheIndexDirectory());

  // Load another URL, cache index should not be written to disk yet.
  LoadURL(embedded_test_server()->GetURL("/cachetime?foo"),
          loader_factory.get());
  EXPECT_EQ(directory_size,
            base::ComputeDirectorySize(GetCacheIndexDirectory()));

  // After application state changes, cache index should be written to disk.
  base::android::ApplicationStatusListener::NotifyApplicationStateChange(
      base::android::APPLICATION_STATE_HAS_STOPPED_ACTIVITIES);
  base::RunLoop().RunUntilIdle();
  FlushNetworkServiceInstanceForTesting();
  disk_cache::FlushCacheThreadForTesting();

  EXPECT_GT(base::ComputeDirectorySize(GetCacheIndexDirectory()),
            directory_size);
}

class NetworkConnectionObserver
    : public network::NetworkConnectionTracker::NetworkConnectionObserver {
 public:
  NetworkConnectionObserver() {
    content::GetNetworkConnectionTracker()->AddNetworkConnectionObserver(this);
    content::GetNetworkConnectionTracker()->GetConnectionType(
        &last_connection_type_,
        base::BindOnce(&NetworkConnectionObserver::OnConnectionChanged,
                       base::Unretained(this)));
  }

  ~NetworkConnectionObserver() override {
    content::GetNetworkConnectionTracker()->RemoveNetworkConnectionObserver(
        this);
  }

  void WaitForConnectionType(network::mojom::ConnectionType type) {
    type_to_wait_for_ = type;
    if (last_connection_type_ == type_to_wait_for_)
      return;

    run_loop_ = std::make_unique<base::RunLoop>();
    run_loop_->Run();
  }

  // network::NetworkConnectionTracker::NetworkConnectionObserver:
  void OnConnectionChanged(network::mojom::ConnectionType type) override {
    last_connection_type_ = type;
    if (run_loop_ && type_to_wait_for_ == type)
      run_loop_->Quit();
  }

 private:
  network::mojom::ConnectionType type_to_wait_for_ =
      network::mojom::ConnectionType::CONNECTION_UNKNOWN;
  network::mojom::ConnectionType last_connection_type_ =
      network::mojom::ConnectionType::CONNECTION_UNKNOWN;
  std::unique_ptr<base::RunLoop> run_loop_;
};

IN_PROC_BROWSER_TEST_F(NetworkServiceBrowserTest,
                       ConnectionTypeChangeSyncedToNetworkProcess) {
  NetworkConnectionObserver observer;
  net::NetworkChangeNotifier::NotifyObserversOfConnectionTypeChangeForTests(
      net::NetworkChangeNotifier::CONNECTION_WIFI);
  observer.WaitForConnectionType(
      network::mojom::ConnectionType::CONNECTION_WIFI);

  net::NetworkChangeNotifier::NotifyObserversOfConnectionTypeChangeForTests(
      net::NetworkChangeNotifier::CONNECTION_ETHERNET);
  observer.WaitForConnectionType(
      network::mojom::ConnectionType::CONNECTION_ETHERNET);
}
#endif

IN_PROC_BROWSER_TEST_F(NetworkServiceBrowserTest,
                       MemoryPressureSentToNetworkProcess) {
  if (IsInProcessNetworkService())
    return;

  network::mojom::NetworkServiceTestPtr network_service_test;
  ServiceManagerConnection::GetForProcess()->GetConnector()->BindInterface(
      mojom::kNetworkServiceName, &network_service_test);
  // TODO(crbug.com/901026): Make sure the network process is started to avoid a
  // deadlock on Android.
  network_service_test.FlushForTesting();

  mojo::ScopedAllowSyncCallForTesting allow_sync_call;
  base::MemoryPressureListener::MemoryPressureLevel memory_pressure_level =
      base::MemoryPressureListener::MEMORY_PRESSURE_LEVEL_NONE;
  network_service_test->GetLatestMemoryPressureLevel(&memory_pressure_level);
  EXPECT_EQ(memory_pressure_level,
            base::MemoryPressureListener::MEMORY_PRESSURE_LEVEL_NONE);

  base::MemoryPressureListener::NotifyMemoryPressure(
      base::MemoryPressureListener::MEMORY_PRESSURE_LEVEL_CRITICAL);
  base::RunLoop().RunUntilIdle();
  FlushNetworkServiceInstanceForTesting();

  network_service_test->GetLatestMemoryPressureLevel(&memory_pressure_level);
  EXPECT_EQ(memory_pressure_level,
            base::MemoryPressureListener::MEMORY_PRESSURE_LEVEL_CRITICAL);
}

// Verifies that sync XHRs don't hang if the network service crashes.
IN_PROC_BROWSER_TEST_F(NetworkServiceBrowserTest, SyncXHROnCrash) {
  if (IsInProcessNetworkService())
    return;

  network::mojom::NetworkServiceTestPtr network_service_test;
  ServiceManagerConnection::GetForProcess()->GetConnector()->BindInterface(
      mojom::kNetworkServiceName, &network_service_test);
  network::mojom::NetworkServiceTestPtrInfo network_service_test_info =
      network_service_test.PassInterface();

  net::EmbeddedTestServer http_server;
  net::test_server::RegisterDefaultHandlers(&http_server);
  http_server.RegisterRequestMonitor(base::BindLambdaForTesting(
      [&](const net::test_server::HttpRequest& request) {
        if (request.relative_url == "/hung") {
          network::mojom::NetworkServiceTestPtr network_service_test2(
              std::move(network_service_test_info));
          network_service_test2->SimulateCrash();
        }
      }));
  EXPECT_TRUE(http_server.Start());

  NavigateToURL(shell(), http_server.GetURL("/empty.html"));

  FetchResource(http_server.GetURL("/hung"), true);
  // If the renderer is hung the test will hang.
}

// Verifies that sync cookie calls don't hang if the network service crashes.
IN_PROC_BROWSER_TEST_F(NetworkServiceBrowserTest, SyncCookieGetOnCrash) {
  if (IsInProcessNetworkService())
    return;

  network::mojom::NetworkServiceTestPtr network_service_test;
  ServiceManagerConnection::GetForProcess()->GetConnector()->BindInterface(
      mojom::kNetworkServiceName, &network_service_test);
  network_service_test->CrashOnGetCookieList();

  NavigateToURL(shell(), embedded_test_server()->GetURL("/empty.html"));

  ASSERT_TRUE(
      content::ExecuteScript(shell()->web_contents(), "document.cookie"));
  // If the renderer is hung the test will hang.
}

class NetworkServiceInProcessBrowserTest : public ContentBrowserTest {
 public:
  NetworkServiceInProcessBrowserTest() {
    std::vector<base::Feature> features;
    features.push_back(network::features::kNetworkService);
    features.push_back(features::kNetworkServiceInProcess);
    scoped_feature_list_.InitWithFeatures(features,
                                          std::vector<base::Feature>());
  }

  void SetUpOnMainThread() override {
    host_resolver()->AddRule("*", "127.0.0.1");
    EXPECT_TRUE(embedded_test_server()->Start());
  }

 private:
  base::test::ScopedFeatureList scoped_feature_list_;

  DISALLOW_COPY_AND_ASSIGN(NetworkServiceInProcessBrowserTest);
};

// Verifies that in-process network service works.
IN_PROC_BROWSER_TEST_F(NetworkServiceInProcessBrowserTest, Basic) {
  GURL test_url = embedded_test_server()->GetURL("foo.com", "/echo");
  StoragePartitionImpl* partition = static_cast<StoragePartitionImpl*>(
      BrowserContext::GetDefaultStoragePartition(
          shell()->web_contents()->GetBrowserContext()));
  NavigateToURL(shell(), test_url);
  ASSERT_EQ(net::OK,
            LoadBasicRequest(partition->GetNetworkContext(), test_url));
}

class NetworkServiceInvalidLogBrowserTest : public ContentBrowserTest {
 public:
  NetworkServiceInvalidLogBrowserTest() {
    scoped_feature_list_.InitAndEnableFeature(
        network::features::kNetworkService);
  }

  void SetUpCommandLine(base::CommandLine* command_line) override {
    command_line->AppendSwitchASCII(network::switches::kLogNetLog, "/abc/def");
  }

  void SetUpOnMainThread() override {
    host_resolver()->AddRule("*", "127.0.0.1");
    EXPECT_TRUE(embedded_test_server()->Start());
  }

 private:
  base::test::ScopedFeatureList scoped_feature_list_;

  DISALLOW_COPY_AND_ASSIGN(NetworkServiceInvalidLogBrowserTest);
};

// Verifies that an invalid --log-net-log flag won't crash the browser.
IN_PROC_BROWSER_TEST_F(NetworkServiceInvalidLogBrowserTest, Basic) {
  GURL test_url = embedded_test_server()->GetURL("foo.com", "/echo");
  StoragePartitionImpl* partition = static_cast<StoragePartitionImpl*>(
      BrowserContext::GetDefaultStoragePartition(
          shell()->web_contents()->GetBrowserContext()));
  NavigateToURL(shell(), test_url);
  ASSERT_EQ(net::OK,
            LoadBasicRequest(partition->GetNetworkContext(), test_url));
}

}  // namespace

}  // namespace content
