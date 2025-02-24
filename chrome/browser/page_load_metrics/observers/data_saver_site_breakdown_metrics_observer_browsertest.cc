// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdint.h>
#include <memory>
#include <string>

#include "base/command_line.h"
#include "base/run_loop.h"
#include "base/strings/stringprintf.h"
#include "base/test/scoped_feature_list.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/data_reduction_proxy/data_reduction_proxy_chrome_settings.h"
#include "chrome/browser/data_reduction_proxy/data_reduction_proxy_chrome_settings_factory.h"
#include "chrome/browser/page_load_metrics/page_load_metrics_test_waiter.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/pref_names.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/data_reduction_proxy/core/browser/data_reduction_proxy_compression_stats.h"
#include "components/data_reduction_proxy/core/browser/data_reduction_proxy_service.h"
#include "components/data_reduction_proxy/core/browser/data_reduction_proxy_settings.h"
#include "components/data_reduction_proxy/core/common/data_reduction_proxy_features.h"
#include "components/data_reduction_proxy/core/common/data_reduction_proxy_pref_names.h"
#include "components/data_reduction_proxy/core/common/data_reduction_proxy_switches.h"
#include "components/data_reduction_proxy/proto/data_store.pb.h"
#include "components/prefs/pref_service.h"
#include "components/previews/core/previews_features.h"
#include "components/previews/core/previews_switches.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/content_features.h"
#include "content/public/test/browser_test_base.h"
#include "content/public/test/browser_test_utils.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_response.h"
#include "services/network/public/cpp/network_quality_tracker.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

// Return a plaintext response.
std::unique_ptr<net::test_server::HttpResponse>
HandleResourceRequestWithPlaintextMimeType(
    const net::test_server::HttpRequest& request) {
  std::unique_ptr<net::test_server::BasicHttpResponse> response =
      std::make_unique<net::test_server::BasicHttpResponse>();

  response->set_code(net::HttpStatusCode::HTTP_OK);
  response->set_content("Some non-HTML content.");
  response->set_content_type("text/plain");

  return response;
}

}  // namespace

class DataSaverSiteBreakdownMetricsObserverBrowserTest
    : public InProcessBrowserTest {
 protected:
  void SetUp() override {
    scoped_feature_list_.InitWithFeaturesAndParameters(
        {{features::kLazyImageLoading,
          {{"automatic-lazy-load-images-enabled", "true"}}}},
        {});
    InProcessBrowserTest::SetUp();
  }

  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();

    PrefService* prefs = browser()->profile()->GetPrefs();
    prefs->SetBoolean(data_reduction_proxy::prefs::kDataUsageReportingEnabled,
                      true);
  }

  void WaitForDBToInitialize() {
    base::RunLoop run_loop;
    DataReductionProxyChromeSettingsFactory::GetForBrowserContext(
        browser()->profile())
        ->data_reduction_proxy_service()
        ->GetDBTaskRunnerForTesting()
        ->PostTask(FROM_HERE, run_loop.QuitClosure());
    run_loop.Run();
  }

  void SetUpCommandLine(base::CommandLine* command_line) override {
    command_line->AppendSwitch(
        data_reduction_proxy::switches::kEnableDataReductionProxy);
    command_line->AppendSwitch(previews::switches::kIgnorePreviewsBlacklist);
  }

  // Gets the data usage recorded against the host the embedded server runs on.
  uint64_t GetDataUsage(const std::string& host) {
    const auto& data_usage_map =
        DataReductionProxyChromeSettingsFactory::GetForBrowserContext(
            browser()->profile())
            ->data_reduction_proxy_service()
            ->compression_stats()
            ->DataUsageMapForTesting();
    const auto& it = data_usage_map.find(host);
    if (it != data_usage_map.end())
      return it->second->data_used();
    return 0;
  }

  // Gets the data savings recorded against the host the embedded server runs
  // on.
  int64_t GetDataSavings(const std::string& host) {
    const auto& data_usage_map =
        DataReductionProxyChromeSettingsFactory::GetForBrowserContext(
            browser()->profile())
            ->data_reduction_proxy_service()
            ->compression_stats()
            ->DataUsageMapForTesting();
    const auto& it = data_usage_map.find(host);
    if (it != data_usage_map.end())
      return it->second->original_size() - it->second->data_used();
    return 0;
  }

  void ScrollToAndWaitForScroll(unsigned int scroll_offset) {
    ASSERT_TRUE(content::ExecuteScript(
        browser()->tab_strip_model()->GetActiveWebContents(),
        base::StringPrintf("window.scrollTo(0, %d);", scroll_offset)));
    content::RenderFrameSubmissionObserver observer(
        browser()->tab_strip_model()->GetActiveWebContents());
    observer.WaitForScrollOffset(gfx::Vector2dF(0, scroll_offset));
  }

  // Navigates to |url| waiting until |expected_resources| are received and then
  // returns the data savings. |expected_resources| should include main html,
  // subresources and favicon.
  int64_t NavigateAndGetDataSavings(const std::string& url,
                                    int expected_resources) {
    WaitForDBToInitialize();
    EXPECT_TRUE(embedded_test_server()->Start());

    GURL test_url(embedded_test_server()->GetURL(url));
    uint64_t data_savings_before_navigation =
        GetDataSavings(test_url.HostNoBrackets());

    auto waiter =
        std::make_unique<page_load_metrics::PageLoadMetricsTestWaiter>(
            browser()->tab_strip_model()->GetActiveWebContents());

    ui_test_utils::NavigateToURL(browser(), test_url);

    waiter->AddMinimumCompleteResourcesExpectation(expected_resources);
    waiter->Wait();

    // Navigate away to force the histogram recording.
    ui_test_utils::NavigateToURL(browser(), GURL(url::kAboutBlankURL));

    return GetDataSavings(test_url.HostNoBrackets()) -
           data_savings_before_navigation;
  }

  // Navigates to |url| waiting until |expected_initial_resources| are received.
  // Then scrolls down the page and waits until |expected_resources_post_scroll|
  // more resources are received. Finally returns the data savings. The resource
  // counts should include main html, subresources and favicon.
  int64_t NavigateAndGetDataSavingsAfterScroll(
      const std::string& url,
      size_t expected_initial_resources,
      size_t expected_resources_post_scroll) {
    WaitForDBToInitialize();
    EXPECT_TRUE(embedded_test_server()->Start());

    GURL test_url(embedded_test_server()->GetURL(url));
    uint64_t data_savings_before_navigation =
        GetDataSavings(test_url.HostNoBrackets());

    auto waiter =
        std::make_unique<page_load_metrics::PageLoadMetricsTestWaiter>(
            browser()->tab_strip_model()->GetActiveWebContents());

    ui_test_utils::NavigateToURL(browser(), test_url);
    waiter->AddMinimumCompleteResourcesExpectation(expected_initial_resources);
    waiter->Wait();

    // Scroll to remove data savings by loading the images.
    ScrollToAndWaitForScroll(10000);

    waiter->AddMinimumCompleteResourcesExpectation(
        expected_initial_resources + expected_resources_post_scroll);
    waiter->Wait();

    // Navigate away to force the histogram recording.
    ui_test_utils::NavigateToURL(browser(), GURL(url::kAboutBlankURL));

    return GetDataSavings(test_url.HostNoBrackets()) -
           data_savings_before_navigation;
  }

 private:
  base::test::ScopedFeatureList scoped_feature_list_;
};

IN_PROC_BROWSER_TEST_F(DataSaverSiteBreakdownMetricsObserverBrowserTest,
                       NavigateToSimplePage) {
  const struct {
    std::string url;
    size_t expected_min_page_size;
    size_t expected_max_page_size;
  } tests[] = {
      // The range of the pages is calculated approximately from the html size
      // and the size of the subresources it includes.
      {"/google/google.html", 5000, 20000},
      {"/simple.html", 100, 1000},
      {"/media/youtube.html", 5000, 20000},
  };
  ASSERT_TRUE(embedded_test_server()->Start());

  for (const auto& test : tests) {
    GURL test_url(embedded_test_server()->GetURL(test.url));
    uint64_t data_usage_before_navigation =
        GetDataUsage(test_url.HostNoBrackets());
    ui_test_utils::NavigateToURL(browser(),
                                 embedded_test_server()->GetURL(test.url));

    base::RunLoop().RunUntilIdle();
    // Navigate away to force the histogram recording.
    ui_test_utils::NavigateToURL(browser(), GURL(url::kAboutBlankURL));

    EXPECT_LE(
        test.expected_min_page_size,
        GetDataUsage(test_url.HostNoBrackets()) - data_usage_before_navigation);
    EXPECT_GE(
        test.expected_max_page_size,
        GetDataUsage(test_url.HostNoBrackets()) - data_usage_before_navigation);
  }
}

IN_PROC_BROWSER_TEST_F(DataSaverSiteBreakdownMetricsObserverBrowserTest,
                       NavigateToPlaintext) {
  std::unique_ptr<net::EmbeddedTestServer> plaintext_server =
      std::make_unique<net::EmbeddedTestServer>(
          net::EmbeddedTestServer::TYPE_HTTPS);
  plaintext_server->RegisterRequestHandler(
      base::BindRepeating(&HandleResourceRequestWithPlaintextMimeType));
  ASSERT_TRUE(plaintext_server->Start());

  GURL test_url(plaintext_server->GetURL("/page"));

  uint64_t data_usage_before_navigation =
      GetDataUsage(test_url.HostNoBrackets());

  ui_test_utils::NavigateToURL(browser(), test_url);
  base::RunLoop().RunUntilIdle();

  // Navigate away to force the histogram recording.
  ui_test_utils::NavigateToURL(browser(), GURL(url::kAboutBlankURL));

  // Choose reasonable minimum (10 is the content length).
  EXPECT_LE(10u, GetDataUsage(test_url.HostNoBrackets()) -
                     data_usage_before_navigation);
  // Choose reasonable maximum (500 is the most we expect from headers).
  EXPECT_GE(500u, GetDataUsage(test_url.HostNoBrackets()) -
                      data_usage_before_navigation);
}

IN_PROC_BROWSER_TEST_F(DataSaverSiteBreakdownMetricsObserverBrowserTest,
                       LazyLoadImagesCSSBackgroundImage) {
  // 2 deferred images.
  EXPECT_EQ(10000 * 2,
            NavigateAndGetDataSavings("/lazyload/css-background-image.html",
                                      2 /* main html, favicon */));
}

IN_PROC_BROWSER_TEST_F(DataSaverSiteBreakdownMetricsObserverBrowserTest,
                       LazyLoadImagesCSSBackgroundImageScrollRemovesSavings) {
  // Scrolling should remove the savings.
  EXPECT_EQ(0u, NavigateAndGetDataSavingsAfterScroll(
                    "/lazyload/css-background-image.html", 2,
                    2 /* lazyloaded images */));
}

IN_PROC_BROWSER_TEST_F(DataSaverSiteBreakdownMetricsObserverBrowserTest,
                       LazyLoadImagesImgElement) {
  // Choose reasonable minimum, any savings is indicative of the mechanism
  // working.
  EXPECT_LE(
      10000,
      NavigateAndGetDataSavings(
          "/lazyload/img.html",
          5 /* main html, favicon and 2 placeholder images, 1 full image */));
}

IN_PROC_BROWSER_TEST_F(DataSaverSiteBreakdownMetricsObserverBrowserTest,
                       LazyLoadImagesImgElementScrollRemovesSavings) {
  // Choose reasonable minimum, any savings is indicative of the mechanism
  // working.
  // TODO(rajendrant): Check why sometimes data savings goes negative.
  EXPECT_GE(0, NavigateAndGetDataSavingsAfterScroll("/lazyload/img.html", 5,
                                                    1 /* lazyloaded image */));
}

IN_PROC_BROWSER_TEST_F(DataSaverSiteBreakdownMetricsObserverBrowserTest,
                       LazyLoadImagesImgWithDimension) {
  // 1 deferred image.
  EXPECT_EQ(10000,
            NavigateAndGetDataSavings("/lazyload/img-with-dimension.html",
                                      3 /* main html, favicon, full image */));
}

IN_PROC_BROWSER_TEST_F(DataSaverSiteBreakdownMetricsObserverBrowserTest,
                       LazyLoadImagesImgWithDimensionScrollRemovesSavings) {
  // Scrolling should remove the savings.
  EXPECT_EQ(0u, NavigateAndGetDataSavingsAfterScroll(
                    "/lazyload/img-with-dimension.html", 3,
                    1 /* lazyloaded image */));
}

IN_PROC_BROWSER_TEST_F(DataSaverSiteBreakdownMetricsObserverBrowserTest,
                       DISABLED_LoFiTest) {
  base::test::ScopedFeatureList scoped_feature_list;
  scoped_feature_list.InitWithFeatures(
      {previews::features::kPreviews,
       data_reduction_proxy::features::
           kDataReductionProxyEnabledWithNetworkService},
      {});

  ASSERT_TRUE(embedded_test_server()->Start());

  g_browser_process->network_quality_tracker()
      ->ReportEffectiveConnectionTypeForTesting(
          net::EFFECTIVE_CONNECTION_TYPE_2G);

  GURL test_url(
      embedded_test_server()->GetURL("/drag_and_drop/image_source.html"));

  // Check that LoFi has data savings.
  uint64_t data_usage_before_navigation =
      GetDataUsage(test_url.HostNoBrackets());

  ui_test_utils::NavigateToURL(browser(), test_url);
  base::RunLoop().RunUntilIdle();

  // Navigate away to force the histogram recording.
  ui_test_utils::NavigateToURL(browser(), GURL(url::kAboutBlankURL));

  // Choose reasonable minimum, any savings is indicative of the mechanism
  // working.
  EXPECT_LE(10u, GetDataSavings(test_url.HostNoBrackets()) -
                     data_usage_before_navigation);

  // Repeat the test, but this time reload the LoFi images.
  data_usage_before_navigation = GetDataUsage(test_url.HostNoBrackets());

  ui_test_utils::NavigateToURL(browser(), test_url);
  base::RunLoop().RunUntilIdle();

  browser()->tab_strip_model()->GetActiveWebContents()->ReloadLoFiImages();

  // Navigate away to force the histogram recording.
  ui_test_utils::NavigateToURL(browser(), GURL(url::kAboutBlankURL));

  EXPECT_LE(10u, data_usage_before_navigation -
                     GetDataSavings(test_url.HostNoBrackets()));
}
