// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/test/base/test_launcher_utils.h"

#include <memory>

#include "base/command_line.h"
#include "base/environment.h"
#include "base/feature_list.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "build/build_config.h"
#include "chrome/common/chrome_paths.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/url_constants.h"
#include "components/os_crypt/os_crypt_switches.h"
#include "content/public/common/content_switches.h"
#include "ui/display/display_switches.h"

#if defined(USE_AURA)
#include "ui/wm/core/wm_core_switches.h"
#endif

namespace test_launcher_utils {

void PrepareBrowserCommandLineForTests(base::CommandLine* command_line) {
  // Don't show the first run ui.
  command_line->AppendSwitch(switches::kNoFirstRun);

  // No default browser check, it would create an info-bar (if we are not the
  // default browser) that could conflicts with some tests expectations.
  command_line->AppendSwitch(switches::kNoDefaultBrowserCheck);

  // Enable info level logging to stderr by default so that we can see when bad
  // stuff happens, but honor the flags specified from the command line. Use the
  // default logging level (INFO) instead of explicitly passing
  // switches::kLoggingLevel. Passing the switch explicitly resulted in data
  // races in tests that start async operations (that use logging) prior to
  // initializing the browser: https://crbug.com/749066.
  if (!command_line->HasSwitch(switches::kEnableLogging))
    command_line->AppendSwitchASCII(switches::kEnableLogging, "stderr");

  // Don't install default apps.
  command_line->AppendSwitch(switches::kDisableDefaultApps);

#if defined(USE_AURA)
  // Disable window animations under Ash as the animations effect the
  // coordinates returned and result in flake.
  command_line->AppendSwitch(
      wm::switches::kWindowAnimationsDisabled);
#endif

#if defined(OS_POSIX) && !defined(OS_MACOSX) && !defined(OS_CHROMEOS)
  // Don't use the native password stores on Linux since they may
  // prompt for additional UI during tests and cause test failures or
  // timeouts.  Win, Mac and ChromeOS don't look at the kPasswordStore
  // switch.
  if (!command_line->HasSwitch(switches::kPasswordStore))
    command_line->AppendSwitchASCII(switches::kPasswordStore, "basic");
#endif

#if defined(OS_MACOSX)
  // Use mock keychain on mac to prevent blocking permissions dialogs.
  command_line->AppendSwitch(os_crypt::switches::kUseMockKeychain);
#endif

  command_line->AppendSwitch(switches::kDisableComponentUpdate);
}

void PrepareBrowserCommandLineForBrowserTests(base::CommandLine* command_line,
                                              bool open_about_blank_on_launch) {
  // This is a Browser test.
  command_line->AppendSwitchASCII(switches::kTestType, "browser");

  // Some browser tests produce pixel results and compare them. Use an sRGB
  // color profile to ensure that the machine's color profile does not affect
  // the results.
  command_line->AppendSwitchASCII(switches::kForceDisplayColorProfile, "srgb");

  if (open_about_blank_on_launch && command_line->GetArgs().empty())
    command_line->AppendArg(url::kAboutBlankURL);
}

void RemoveCommandLineSwitch(const base::CommandLine& in_command_line,
                             const std::string& switch_to_remove,
                             base::CommandLine* out_command_line) {
  const base::CommandLine::SwitchMap& switch_map =
      in_command_line.GetSwitches();
  for (auto i = switch_map.begin(); i != switch_map.end(); ++i) {
    const std::string& switch_name = i->first;
    if (switch_name == switch_to_remove)
      continue;

    out_command_line->AppendSwitchNative(switch_name, i->second);
  }
}

bool OverrideUserDataDir(const base::FilePath& user_data_dir) {
  bool success = true;

  // base::PathService::Override() is the best way to change the user data
  // directory. This matches what is done in ChromeMain().
  success = base::PathService::Override(chrome::DIR_USER_DATA, user_data_dir);

#if defined(OS_POSIX) && !defined(OS_MACOSX)
  // Make sure the cache directory is inside our clear profile. Otherwise
  // the cache may contain data from earlier tests that could break the
  // current test.
  //
  // Note: we use an environment variable here, because we have to pass the
  // value to the child process. This is the simplest way to do it.
  std::unique_ptr<base::Environment> env(base::Environment::Create());
  success = success && env->SetVar("XDG_CACHE_HOME", user_data_dir.value());

  // Also make sure that the machine policy directory is inside the clear
  // profile. Otherwise the machine's policies could affect tests.
  base::FilePath policy_files = user_data_dir.AppendASCII("policies");
  success = success &&
            base::PathService::Override(chrome::DIR_POLICY_FILES, policy_files);
#endif

  return success;
}

}  // namespace test_launcher_utils
