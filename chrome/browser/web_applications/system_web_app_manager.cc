// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/web_applications/system_web_app_manager.h"

#include <string>
#include <utility>
#include <vector>

#include "base/bind.h"
#include "base/stl_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/version.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/web_applications/components/web_app_constants.h"
#include "chrome/browser/web_applications/components/web_app_ui_delegate.h"
#include "chrome/common/chrome_features.h"
#include "chrome/common/pref_names.h"
#include "chrome/common/webui_url_constants.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/pref_service.h"
#include "components/version_info/version_info.h"
#include "content/public/common/content_switches.h"

#if defined(OS_CHROMEOS)
#include "ash/public/cpp/app_list/internal_app_id_constants.h"
#include "chromeos/constants/chromeos_features.h"
#endif  // defined(OS_CHROMEOS)

namespace web_app {

namespace {

base::flat_map<SystemAppType, SystemAppInfo> CreateSystemWebApps() {
  base::flat_map<SystemAppType, SystemAppInfo> infos;
// TODO(calamity): Split this into per-platform functions.
#if defined(OS_CHROMEOS)
  if (base::FeatureList::IsEnabled(chromeos::features::kDiscoverApp))
    infos[SystemAppType::DISCOVER] = {GURL(chrome::kChromeUIDiscoverURL)};

  if (base::FeatureList::IsEnabled(chromeos::features::kSplitSettings)) {
    constexpr char kChromeSettingsPWAURL[] = "chrome://os-settings/pwa.html";
    infos[SystemAppType::SETTINGS] = {GURL(kChromeSettingsPWAURL),
                                      app_list::kInternalAppIdSettings};
  } else {
    constexpr char kChromeSettingsPWAURL[] = "chrome://settings/pwa.html";
    infos[SystemAppType::SETTINGS] = {GURL(kChromeSettingsPWAURL),
                                      app_list::kInternalAppIdSettings};
  }
#endif  // OS_CHROMEOS

  return infos;
}

InstallOptions CreateInstallOptionsForSystemApp(const SystemAppInfo& info) {
  DCHECK_EQ(content::kChromeUIScheme, info.install_url.scheme());

  web_app::InstallOptions install_options(info.install_url,
                                          LaunchContainer::kWindow,
                                          InstallSource::kSystemInstalled);
  install_options.add_to_applications_menu = false;
  install_options.add_to_desktop = false;
  install_options.add_to_quick_launch_bar = false;
  install_options.bypass_service_worker_check = true;
  install_options.always_update = true;
  return install_options;
}

}  // namespace

SystemWebAppManager::SystemWebAppManager(Profile* profile,
                                         PendingAppManager* pending_app_manager)
    : on_apps_synchronized_(new base::OneShotEvent()),
      pref_service_(profile->GetPrefs()),
      pending_app_manager_(pending_app_manager),
      weak_ptr_factory_(this) {
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          ::switches::kTestType)) {
    // Always update in tests, and return early to avoid populating with real
    // system apps.
    update_policy_ = UpdatePolicy::kAlwaysUpdate;
    return;
  }

#if defined(OFFICIAL_BUILD)
  // Official builds should trigger updates whenever the version number changes.
  update_policy_ = UpdatePolicy::kOnVersionChange;
#else
  // Dev builds should update every launch.
  update_policy_ = UpdatePolicy::kAlwaysUpdate;
#endif
  system_app_infos_ = CreateSystemWebApps();
}

SystemWebAppManager::~SystemWebAppManager() = default;

void SystemWebAppManager::Start(WebAppUiDelegate* ui_delegate) {
  ui_delegate_ = ui_delegate;

  // Clear the last update pref here to force uninstall, and to ensure that when
  // the flag is enabled again, an update is triggered.
  if (!IsEnabled())
    pref_service_->ClearPref(prefs::kSystemWebAppLastUpdateVersion);

  if (!NeedsUpdate())
    return;

  std::vector<GURL> installed_apps = pending_app_manager_->GetInstalledAppUrls(
      InstallSource::kSystemInstalled);

  std::set<SystemAppType> installed_app_types;
  for (const auto& it : system_app_infos_) {
    if (std::find(installed_apps.begin(), installed_apps.end(),
                  it.second.install_url) != installed_apps.end())
      installed_app_types.insert(it.first);
  }

  std::vector<InstallOptions> install_options_list;
  if (IsEnabled()) {
    // Skipping this will uninstall all System Apps currently installed.
    for (const auto& app : system_app_infos_) {
      install_options_list.push_back(
          CreateInstallOptionsForSystemApp(app.second));
    }
  }

  pending_app_manager_->SynchronizeInstalledApps(
      std::move(install_options_list), InstallSource::kSystemInstalled,
      base::BindOnce(&SystemWebAppManager::OnAppsSynchronized,
                     weak_ptr_factory_.GetWeakPtr(), installed_app_types));
}

void SystemWebAppManager::InstallSystemAppsForTesting() {
  on_apps_synchronized_.reset(new base::OneShotEvent());
  system_app_infos_ = CreateSystemWebApps();
  Start(ui_delegate_);

  // Wait for the System Web Apps to install.
  base::RunLoop run_loop;
  on_apps_synchronized().Post(FROM_HERE, run_loop.QuitClosure());
  run_loop.Run();
}

base::Optional<AppId> SystemWebAppManager::GetAppIdForSystemApp(
    SystemAppType id) const {
  auto app_url_it = system_app_infos_.find(id);

  if (app_url_it == system_app_infos_.end())
    return base::Optional<AppId>();

  return pending_app_manager_->LookupAppId(app_url_it->second.install_url);
}

bool SystemWebAppManager::IsSystemWebApp(const AppId& app_id) const {
  return pending_app_manager_->HasAppIdWithInstallSource(
      app_id, InstallSource::kSystemInstalled);
}

void SystemWebAppManager::SetSystemAppsForTesting(
    base::flat_map<SystemAppType, SystemAppInfo> system_apps) {
  system_app_infos_ = std::move(system_apps);
}

void SystemWebAppManager::SetUpdatePolicyForTesting(UpdatePolicy policy) {
  update_policy_ = policy;
}

// static
bool SystemWebAppManager::IsEnabled() {
  return base::FeatureList::IsEnabled(features::kSystemWebApps);
}

// static
void SystemWebAppManager::RegisterProfilePrefs(
    user_prefs::PrefRegistrySyncable* registry) {
  registry->RegisterStringPref(prefs::kSystemWebAppLastUpdateVersion, "");
}

const base::Version& SystemWebAppManager::CurrentVersion() const {
  return version_info::GetVersion();
}

void SystemWebAppManager::OnAppsSynchronized(
    std::set<SystemAppType> already_installed,
    PendingAppManager::SynchronizeResult result) {
  if (IsEnabled()) {
    pref_service_->SetString(prefs::kSystemWebAppLastUpdateVersion,
                             CurrentVersion().GetString());
  }

  MigrateSystemWebApps(already_installed);

  // May be called more than once in tests.
  if (!on_apps_synchronized_->is_signaled())
    on_apps_synchronized_->Signal();
}

bool SystemWebAppManager::NeedsUpdate() const {
  if (update_policy_ == UpdatePolicy::kAlwaysUpdate)
    return true;

  base::Version last_update_version(
      pref_service_->GetString(prefs::kSystemWebAppLastUpdateVersion));
  // This also updates if the version rolls back for some reason to ensure that
  // the System Web Apps are always in sync with the Chrome version.
  return !last_update_version.IsValid() ||
         last_update_version != CurrentVersion();
}

void SystemWebAppManager::MigrateSystemWebApps(
    std::set<SystemAppType> already_installed) {
  DCHECK(ui_delegate_);

  for (const auto& type_and_info : system_app_infos_) {
    // Migrate if a migration source is specified and the app has been newly
    // installed.
    if (!type_and_info.second.migration_source.empty() &&
        !base::Contains(already_installed, type_and_info.first)) {
      ui_delegate_->MigrateOSAttributes(
          type_and_info.second.migration_source,
          *GetAppIdForSystemApp(type_and_info.first));
    }
  }
}

}  // namespace web_app
