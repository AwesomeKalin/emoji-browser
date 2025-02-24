// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/extensions/browsertest_util.h"

#include <memory>

#include "base/feature_list.h"
#include "base/files/file_util.h"
#include "base/path_service.h"
#include "base/run_loop.h"
#include "base/test/bind_test_util.h"
#include "chrome/browser/extensions/launch_util.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/extensions/app_launch_params.h"
#include "chrome/browser/ui/extensions/application_launch.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/web_applications/components/install_manager.h"
#include "chrome/browser/web_applications/components/web_app_constants.h"
#include "chrome/browser/web_applications/components/web_app_helpers.h"
#include "chrome/browser/web_applications/components/web_app_provider_base.h"
#include "chrome/browser/web_applications/components/web_app_tab_helper_base.h"
#include "chrome/common/chrome_features.h"
#include "chrome/common/web_application_info.h"
#include "content/public/browser/notification_service.h"
#include "content/public/test/test_utils.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/extension_system.h"
#include "extensions/browser/notification_types.h"
#include "extensions/common/extension.h"
#include "testing/gtest/include/gtest/gtest.h"

#if defined(OS_CHROMEOS)
#include "chrome/browser/extensions/updater/local_extension_cache.h"
#include "chromeos/constants/chromeos_paths.h"
#endif

namespace extensions {
namespace browsertest_util {

void CreateAndInitializeLocalCache() {
#if defined(OS_CHROMEOS)
  base::FilePath extension_cache_dir;
  CHECK(base::PathService::Get(chromeos::DIR_DEVICE_EXTENSION_LOCAL_CACHE,
                               &extension_cache_dir));
  base::FilePath cache_init_file = extension_cache_dir.Append(
      extensions::LocalExtensionCache::kCacheReadyFlagFileName);
  EXPECT_EQ(base::WriteFile(cache_init_file, "", 0), 0);
#endif
}

const Extension* InstallBookmarkApp(Profile* profile, WebApplicationInfo info) {
  size_t num_extensions =
      ExtensionRegistry::Get(profile)->enabled_extensions().size();

  // TODO(crbug.com/915043): Erase windowed_observer code path.
  content::WindowedNotificationObserver windowed_observer(
      NOTIFICATION_CRX_INSTALLER_DONE,
      content::NotificationService::AllSources());

  base::RunLoop run_loop;
  web_app::AppId app_id;
  auto* provider = web_app::WebAppProviderBase::GetProviderBase(profile);
  DCHECK(provider);
  provider->install_manager().InstallWebAppForTesting(
      std::make_unique<WebApplicationInfo>(info),
      base::BindLambdaForTesting([&](const web_app::AppId& installed_app_id,
                                     web_app::InstallResultCode code) {
        DCHECK_EQ(web_app::InstallResultCode::kSuccess, code);
        app_id = installed_app_id;
        run_loop.Quit();
      }));

  const Extension* app = nullptr;
  // The legacy system doesn't support completion callback in
  // InstallWebAppForTesting. Use |windowed_observer| if
  // kDesktopPWAsUnifiedInstall disabled.
  if (base::FeatureList::IsEnabled(features::kDesktopPWAsUnifiedInstall)) {
    run_loop.Run();
    app = ExtensionRegistry::Get(profile)->enabled_extensions().GetByID(app_id);
  } else {
    windowed_observer.Wait();
    app = content::Details<const Extension>(windowed_observer.details()).ptr();
  }

  EXPECT_EQ(++num_extensions,
            ExtensionRegistry::Get(profile)->enabled_extensions().size());
  DCHECK(app);
  extensions::SetLaunchType(profile, app->id(),
                            info.open_as_window
                                ? extensions::LAUNCH_TYPE_WINDOW
                                : extensions::LAUNCH_TYPE_REGULAR);
  return app;
}

Browser* LaunchAppBrowser(Profile* profile, const Extension* extension_app) {
  EXPECT_TRUE(OpenApplication(
      AppLaunchParams(profile, extension_app->id(), LAUNCH_CONTAINER_WINDOW,
                      WindowOpenDisposition::CURRENT_TAB, SOURCE_TEST)));

  Browser* browser = chrome::FindLastActive();
  bool is_correct_app_browser =
      browser && web_app::GetAppIdFromApplicationName(browser->app_name()) ==
                     extension_app->id();
  EXPECT_TRUE(is_correct_app_browser);

  return is_correct_app_browser ? browser : nullptr;
}

Browser* LaunchBrowserForAppInTab(Profile* profile,
                                  const Extension* extension_app) {
  content::WebContents* web_contents = OpenApplication(
      AppLaunchParams(profile, extension_app->id(), LAUNCH_CONTAINER_TAB,
                      WindowOpenDisposition::NEW_FOREGROUND_TAB, SOURCE_TEST));
  DCHECK(web_contents);

  web_app::WebAppTabHelperBase* tab_helper =
      web_app::WebAppTabHelperBase::FromWebContents(web_contents);
  DCHECK(tab_helper);
  DCHECK_EQ(extension_app->id(), tab_helper->app_id());

  Browser* browser = chrome::FindBrowserWithWebContents(web_contents);
  DCHECK_EQ(browser, chrome::FindLastActive());
  DCHECK_EQ(web_contents, browser->tab_strip_model()->GetActiveWebContents());
  return browser;
}

}  // namespace browsertest_util
}  // namespace extensions
