// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/web_applications/web_app_ui_service.h"

#include "base/barrier_closure.h"
#include "base/test/bind_test_util.h"
#include "base/threading/thread_task_runner_handle.h"
#include "chrome/browser/extensions/browsertest_util.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/browser_list_observer.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/web_applications/components/web_app_helpers.h"
#include "chrome/browser/web_applications/system_web_app_manager.h"
#include "chrome/browser/web_applications/web_app_provider.h"
#include "chrome/common/chrome_features.h"
#include "chrome/common/web_application_info.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "extensions/common/extension.h"
#include "url/gurl.h"

#if defined(OS_CHROMEOS)
#include "ash/public/cpp/app_list/internal_app_id_constants.h"
#include "chrome/browser/ui/app_list/app_list_model_updater.h"
#include "chrome/browser/ui/app_list/app_list_syncable_service.h"
#include "chrome/browser/ui/app_list/app_list_syncable_service_factory.h"
#include "chrome/browser/ui/app_list/test/chrome_app_list_test_support.h"
#endif

namespace web_app {

namespace {

// Waits for |browser| to be removed from BrowserList and then calls |callback|.
class BrowserRemovedWaiter final : public BrowserListObserver {
 public:
  explicit BrowserRemovedWaiter(Browser* browser) : browser_(browser) {}
  ~BrowserRemovedWaiter() override = default;

  void Wait() {
    BrowserList::AddObserver(this);
    run_loop_.Run();
  }

  // BrowserListObserver
  void OnBrowserRemoved(Browser* browser) override {
    if (browser != browser_)
      return;

    BrowserList::RemoveObserver(this);
    // Post a task to ensure the Remove event has been dispatched to all
    // observers.
    base::ThreadTaskRunnerHandle::Get()->PostTask(FROM_HERE,
                                                  run_loop_.QuitClosure());
  }

 private:
  Browser* browser_;
  base::RunLoop run_loop_;
};

void CloseAndWait(Browser* browser) {
  BrowserRemovedWaiter waiter(browser);
  browser->window()->Close();
  waiter.Wait();
}

}  // namespace

const GURL kFooUrl = GURL("https://foo.example");
const GURL kBarUrl = GURL("https://bar.example");

class WebAppUiServiceBrowserTest : public InProcessBrowserTest {
 protected:
  Profile* profile() { return browser()->profile(); }

  const extensions::Extension* InstallWebApp(const GURL& app_url) {
    WebApplicationInfo web_app_info;
    web_app_info.app_url = app_url;
    web_app_info.open_as_window = true;
    return extensions::browsertest_util::InstallBookmarkApp(profile(),
                                                            web_app_info);
  }

  Browser* LaunchApp(const extensions::Extension* app) {
    return extensions::browsertest_util::LaunchAppBrowser(profile(), app);
  }

  WebAppUiService* ui_service() { return WebAppUiService::Get(profile()); }
};

IN_PROC_BROWSER_TEST_F(WebAppUiServiceBrowserTest,
                       GetNumWindowsForApp_AppWindowsAdded) {
  // Should not crash.
  auto& ui_delegate = WebAppProvider::Get(browser()->profile())->ui_delegate();
  auto* ui_service = WebAppUiService::Get(browser()->profile());
  EXPECT_EQ(&ui_delegate, ui_service);

  // Zero apps on start:
  EXPECT_EQ(0u, ui_service->GetNumWindowsForApp(AppId()));

  const auto* foo_app = InstallWebApp(kFooUrl);
  LaunchApp(foo_app);
  EXPECT_EQ(1u, ui_service->GetNumWindowsForApp(foo_app->id()));

  LaunchApp(foo_app);
  EXPECT_EQ(2u, ui_service->GetNumWindowsForApp(foo_app->id()));
}

IN_PROC_BROWSER_TEST_F(WebAppUiServiceBrowserTest,
                       GetNumWindowsForApp_AppWindowsRemoved) {
  const auto* foo_app = InstallWebApp(kFooUrl);
  auto* foo_window1 = LaunchApp(foo_app);
  auto* foo_window2 = LaunchApp(foo_app);

  const auto* bar_app = InstallWebApp(kBarUrl);
  LaunchApp(bar_app);

  EXPECT_EQ(2u, ui_service()->GetNumWindowsForApp(foo_app->id()));
  EXPECT_EQ(1u, ui_service()->GetNumWindowsForApp(bar_app->id()));

  CloseAndWait(foo_window1);

  EXPECT_EQ(1u, ui_service()->GetNumWindowsForApp(foo_app->id()));
  EXPECT_EQ(1u, ui_service()->GetNumWindowsForApp(bar_app->id()));

  CloseAndWait(foo_window2);

  EXPECT_EQ(0u, ui_service()->GetNumWindowsForApp(foo_app->id()));
  EXPECT_EQ(1u, ui_service()->GetNumWindowsForApp(bar_app->id()));
}

IN_PROC_BROWSER_TEST_F(WebAppUiServiceBrowserTest,
                       NotifyOnAllAppWindowsClosed_NoOpenedWindows) {
  const auto* foo_app = InstallWebApp(kFooUrl);
  const auto* bar_app = InstallWebApp(kBarUrl);
  LaunchApp(bar_app);

  base::RunLoop run_loop;
  // Should return early; no windows for |foo_app|.
  ui_service()->NotifyOnAllAppWindowsClosed(foo_app->id(),
                                            run_loop.QuitClosure());
  run_loop.Run();
}

// Tests that the callback is correctly called when there is more than one
// app window.
IN_PROC_BROWSER_TEST_F(WebAppUiServiceBrowserTest,
                       NotifyOnAllAppWindowsClosed_MultipleOpenedWindows) {
  const auto* foo_app = InstallWebApp(kFooUrl);
  const auto* bar_app = InstallWebApp(kBarUrl);

  // Test that NotifyOnAllAppWindowsClosed can be called more than once for
  // the same app.
  for (int i = 0; i < 2; i++) {
    auto* foo_window1 = LaunchApp(foo_app);
    auto* foo_window2 = LaunchApp(foo_app);
    auto* bar_window = LaunchApp(bar_app);

    bool callback_ran = false;
    base::RunLoop run_loop;
    ui_service()->NotifyOnAllAppWindowsClosed(foo_app->id(),
                                              base::BindLambdaForTesting([&]() {
                                                callback_ran = true;
                                                run_loop.Quit();
                                              }));

    CloseAndWait(foo_window1);
    // The callback shouldn't have run yet because there is still one window
    // opened.
    EXPECT_FALSE(callback_ran);

    CloseAndWait(bar_window);
    EXPECT_FALSE(callback_ran);

    CloseAndWait(foo_window2);
    run_loop.Run();
    EXPECT_TRUE(callback_ran);
  }
}

#if defined(OS_CHROMEOS)
class WebAppUiServiceMigrationBrowserTest : public WebAppUiServiceBrowserTest {
 public:
  void SetUp() override {
    // Disable System Web Apps so that the Internal Apps are installed.
    scoped_feature_list_.InitAndDisableFeature(features::kSystemWebApps);
    WebAppUiServiceBrowserTest::SetUp();
  }

 private:
  base::test::ScopedFeatureList scoped_feature_list_;
};

// Tests that the Settings app migrates the launcher and app list details from
// the Settings internal app.
IN_PROC_BROWSER_TEST_F(WebAppUiServiceMigrationBrowserTest,
                       SettingsSystemWebAppMigration) {
  base::test::ScopedFeatureList scoped_feature_list;
  scoped_feature_list.InitAndEnableFeature(features::kSystemWebApps);

  auto& system_web_app_manager =
      web_app::WebAppProvider::Get(browser()->profile())
          ->system_web_app_manager();

  auto* app_list_service =
      app_list::AppListSyncableServiceFactory::GetForProfile(
          browser()->profile());

  // Pin the Settings Internal App.
  syncer::StringOrdinal pin_position =
      syncer::StringOrdinal::CreateInitialOrdinal();
  pin_position = pin_position.CreateAfter().CreateAfter();
  app_list_service->SetPinPosition(app_list::kInternalAppIdSettings,
                                   pin_position);

  // Add the Settings Internal App to a folder.
  AppListModelUpdater* updater =
      test::GetModelUpdater(test::GetAppListClient());
  updater->MoveItemToFolder(app_list::kInternalAppIdSettings, "asdf");

  // Install the Settings System Web App, which should be immediately migrated
  // to the Settings Internal App's details.
  system_web_app_manager.InstallSystemAppsForTesting();
  std::string settings_system_web_app_id =
      *system_web_app_manager.GetAppIdForSystemApp(SystemAppType::SETTINGS);
  {
    const app_list::AppListSyncableService::SyncItem* web_app_item =
        app_list_service->GetSyncItem(settings_system_web_app_id);
    const app_list::AppListSyncableService::SyncItem* internal_app_item =
        app_list_service->GetSyncItem(app_list::kInternalAppIdSettings);

    EXPECT_TRUE(internal_app_item->item_pin_ordinal.Equals(
        web_app_item->item_pin_ordinal));
    EXPECT_TRUE(
        internal_app_item->item_ordinal.Equals(web_app_item->item_ordinal));
    EXPECT_EQ(internal_app_item->parent_id, web_app_item->parent_id);
  }

  // Change Settings System Web App properties.
  app_list_service->SetPinPosition(
      settings_system_web_app_id,
      syncer::StringOrdinal::CreateInitialOrdinal());
  updater->MoveItemToFolder(settings_system_web_app_id, std::string());

  // Do migration again with the already-installed app. Should be a no-op.
  system_web_app_manager.InstallSystemAppsForTesting();
  {
    const app_list::AppListSyncableService::SyncItem* web_app_item =
        app_list_service->GetSyncItem(settings_system_web_app_id);

    EXPECT_TRUE(syncer::StringOrdinal::CreateInitialOrdinal().Equals(
        web_app_item->item_pin_ordinal));
    EXPECT_EQ(std::string(), web_app_item->parent_id);
  }
}
#endif  // defined(OS_CHROMEOS)

}  // namespace web_app
