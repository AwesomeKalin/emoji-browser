// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/session/session_controller_impl.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include "ash/metrics/user_metrics_recorder.h"
#include "ash/public/cpp/session/session_activation_observer.h"
#include "ash/public/cpp/session/session_controller_client.h"
#include "ash/public/cpp/session/user_info.h"
#include "ash/session/multiprofiles_intro_dialog.h"
#include "ash/session/session_aborted_dialog.h"
#include "ash/session/session_observer.h"
#include "ash/session/teleport_warning_dialog.h"
#include "ash/shell.h"
#include "ash/system/power/power_event_observer.h"
#include "ash/system/screen_security/screen_switch_check_controller.h"
#include "ash/wm/lock_state_controller.h"
#include "ash/wm/mru_window_tracker.h"
#include "ash/wm/overview/overview_controller.h"
#include "ash/wm/window_state.h"
#include "ash/wm/window_util.h"
#include "ash/wm/wm_event.h"
#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/command_line.h"
#include "base/logging.h"
#include "components/account_id/account_id.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"
#include "components/user_manager/user_type.h"
#include "ui/message_center/message_center.h"

using session_manager::SessionState;

namespace ash {

SessionControllerImpl::SessionControllerImpl() = default;

SessionControllerImpl::~SessionControllerImpl() {
  // Abort pending start lock request.
  if (!start_lock_callback_.is_null())
    std::move(start_lock_callback_).Run(false /* locked */);
}

int SessionControllerImpl::NumberOfLoggedInUsers() const {
  return static_cast<int>(user_sessions_.size());
}

AccountId SessionControllerImpl::GetActiveAccountId() const {
  return user_sessions_.empty() ? AccountId()
                                : user_sessions_[0]->user_info.account_id;
}

AddUserSessionPolicy SessionControllerImpl::GetAddUserPolicy() const {
  return add_user_session_policy_;
}

bool SessionControllerImpl::IsActiveUserSessionStarted() const {
  return !user_sessions_.empty();
}

bool SessionControllerImpl::CanLockScreen() const {
  return IsActiveUserSessionStarted() && can_lock_;
}

bool SessionControllerImpl::IsScreenLocked() const {
  return state_ == SessionState::LOCKED;
}

bool SessionControllerImpl::ShouldLockScreenAutomatically() const {
  return should_lock_screen_automatically_;
}

bool SessionControllerImpl::IsRunningInAppMode() const {
  return is_running_in_app_mode_;
}

bool SessionControllerImpl::IsDemoSession() const {
  return is_demo_session_;
}

bool SessionControllerImpl::IsUserSessionBlocked() const {
  // User sessions are blocked when session state is not ACTIVE, with two
  // exceptions:
  // - LOGGED_IN_NOT_ACTIVE state. This is needed so that browser windows
  //   created by session restore (or a default new browser window) are properly
  //   activated before session state changes to ACTIVE.
  // - LOCKED state with a running unlocking animation. This is needed because
  //   the unlocking animation hides the lock container at the end. During the
  //   unlock animation, IsUserSessionBlocked needs to return unblocked so that
  //   user windows are deemed activatable and ash correctly restores the active
  //   window before locking.
  return state_ != SessionState::ACTIVE &&
         state_ != SessionState::LOGGED_IN_NOT_ACTIVE &&
         !(state_ == SessionState::LOCKED && is_unlocking_);
}

bool SessionControllerImpl::IsInSecondaryLoginScreen() const {
  return state_ == SessionState::LOGIN_SECONDARY;
}

SessionState SessionControllerImpl::GetSessionState() const {
  return state_;
}

bool SessionControllerImpl::ShouldEnableSettings() const {
  // Settings opens a web UI window, so it is not available at the lock screen.
  if (!IsActiveUserSessionStarted() || IsScreenLocked() ||
      IsInSecondaryLoginScreen()) {
    return false;
  }

  return user_sessions_[0]->should_enable_settings;
}

bool SessionControllerImpl::ShouldShowNotificationTray() const {
  if (!IsActiveUserSessionStarted() || IsInSecondaryLoginScreen())
    return false;

  return user_sessions_[0]->should_show_notification_tray;
}

const SessionControllerImpl::UserSessions&
SessionControllerImpl::GetUserSessions() const {
  return user_sessions_;
}

const UserSession* SessionControllerImpl::GetUserSession(
    UserIndex index) const {
  if (index < 0 || index >= static_cast<UserIndex>(user_sessions_.size()))
    return nullptr;

  return user_sessions_[index].get();
}

const UserSession* SessionControllerImpl::GetPrimaryUserSession() const {
  auto it = std::find_if(user_sessions_.begin(), user_sessions_.end(),
                         [this](const std::unique_ptr<UserSession>& session) {
                           return session->session_id == primary_session_id_;
                         });
  if (it == user_sessions_.end())
    return nullptr;

  return (*it).get();
}

bool SessionControllerImpl::IsUserSupervised() const {
  if (!IsActiveUserSessionStarted())
    return false;

  user_manager::UserType active_user_type = GetUserSession(0)->user_info.type;
  return active_user_type == user_manager::USER_TYPE_SUPERVISED ||
         active_user_type == user_manager::USER_TYPE_CHILD;
}

bool SessionControllerImpl::IsUserLegacySupervised() const {
  if (!IsActiveUserSessionStarted())
    return false;

  user_manager::UserType active_user_type = GetUserSession(0)->user_info.type;
  return active_user_type == user_manager::USER_TYPE_SUPERVISED;
}

bool SessionControllerImpl::IsUserChild() const {
  if (!IsActiveUserSessionStarted())
    return false;

  user_manager::UserType active_user_type = GetUserSession(0)->user_info.type;
  return active_user_type == user_manager::USER_TYPE_CHILD;
}

bool SessionControllerImpl::IsUserPublicAccount() const {
  if (!IsActiveUserSessionStarted())
    return false;

  user_manager::UserType active_user_type = GetUserSession(0)->user_info.type;
  return active_user_type == user_manager::USER_TYPE_PUBLIC_ACCOUNT;
}

base::Optional<user_manager::UserType> SessionControllerImpl::GetUserType()
    const {
  if (!IsActiveUserSessionStarted())
    return base::nullopt;

  return base::make_optional(GetUserSession(0)->user_info.type);
}

bool SessionControllerImpl::IsUserPrimary() const {
  if (!IsActiveUserSessionStarted())
    return false;

  return GetUserSession(0)->session_id == primary_session_id_;
}

bool SessionControllerImpl::IsUserFirstLogin() const {
  if (!IsActiveUserSessionStarted())
    return false;

  return GetUserSession(0)->user_info.is_new_profile;
}

bool SessionControllerImpl::ShouldDisplayManagedUI() const {
  if (!IsActiveUserSessionStarted())
    return false;

  return GetUserSession(0)->user_info.should_display_managed_ui;
}

void SessionControllerImpl::LockScreen() {
  if (client_)
    client_->RequestLockScreen();
}

void SessionControllerImpl::RequestSignOut() {
  if (client_)
    client_->RequestSignOut();
}

void SessionControllerImpl::SwitchActiveUser(const AccountId& account_id) {
  if (client_)
    client_->SwitchActiveUser(account_id);
}

void SessionControllerImpl::CycleActiveUser(CycleUserDirection direction) {
  if (client_)
    client_->CycleActiveUser(direction);
}

void SessionControllerImpl::ShowMultiProfileLogin() {
  if (client_)
    client_->ShowMultiProfileLogin();
}

void SessionControllerImpl::EmitAshInitialized() {
  if (client_)
    client_->EmitAshInitialized();
}

PrefService* SessionControllerImpl::GetSigninScreenPrefService() const {
  return client_ ? client_->GetSigninScreenPrefService() : nullptr;
}

PrefService* SessionControllerImpl::GetUserPrefServiceForUser(
    const AccountId& account_id) const {
  return client_ ? client_->GetUserPrefService(account_id) : nullptr;
}

PrefService* SessionControllerImpl::GetPrimaryUserPrefService() const {
  const UserSession* session = GetPrimaryUserSession();
  return session ? GetUserPrefServiceForUser(session->user_info.account_id)
                 : nullptr;
}

PrefService* SessionControllerImpl::GetLastActiveUserPrefService() const {
  return last_active_user_prefs_;
}

PrefService* SessionControllerImpl::GetActivePrefService() const {
  // Use the active user prefs once they become available. Check the PrefService
  // object instead of session state because prefs load is async after login.
  if (last_active_user_prefs_)
    return last_active_user_prefs_;
  return GetSigninScreenPrefService();
}

void SessionControllerImpl::AddObserver(SessionObserver* observer) {
  observers_.AddObserver(observer);
}

void SessionControllerImpl::RemoveObserver(SessionObserver* observer) {
  observers_.RemoveObserver(observer);
}

void SessionControllerImpl::SetClient(SessionControllerClient* client) {
  client_ = client;
}

void SessionControllerImpl::SetSessionInfo(const SessionInfo& info) {
  can_lock_ = info.can_lock_screen;
  should_lock_screen_automatically_ = info.should_lock_screen_automatically;
  is_running_in_app_mode_ = info.is_running_in_app_mode;
  if (info.is_demo_session)
    SetIsDemoSession();
  add_user_session_policy_ = info.add_user_session_policy;
  SetSessionState(info.state);
}

void SessionControllerImpl::UpdateUserSession(const UserSession& user_session) {
  auto it = std::find_if(
      user_sessions_.begin(), user_sessions_.end(),
      [&user_session](const std::unique_ptr<UserSession>& session) {
        return session->session_id == user_session.session_id;
      });
  if (it == user_sessions_.end()) {
    AddUserSession(user_session);
    return;
  }

  *it = std::make_unique<UserSession>(user_session);
  for (auto& observer : observers_)
    observer.OnUserSessionUpdated((*it)->user_info.account_id);

  UpdateLoginStatus();
}

void SessionControllerImpl::SetUserSessionOrder(
    const std::vector<uint32_t>& user_session_order) {
  DCHECK_EQ(user_sessions_.size(), user_session_order.size());

  AccountId last_active_account_id;
  if (user_sessions_.size())
    last_active_account_id = user_sessions_[0]->user_info.account_id;

  // Adjusts |user_sessions_| to match the given order.
  std::vector<std::unique_ptr<UserSession>> sessions;
  for (const auto& session_id : user_session_order) {
    auto it =
        std::find_if(user_sessions_.begin(), user_sessions_.end(),
                     [session_id](const std::unique_ptr<UserSession>& session) {
                       return session && session->session_id == session_id;
                     });
    if (it == user_sessions_.end()) {
      LOG(ERROR) << "Unknown session id =" << session_id;
      continue;
    }

    sessions.push_back(std::move(*it));
  }

  user_sessions_.swap(sessions);

  // Check active user change and notifies observers.
  if (user_sessions_[0]->session_id != active_session_id_) {
    const bool is_first_session = active_session_id_ == 0u;
    active_session_id_ = user_sessions_[0]->session_id;

    if (is_first_session) {
      for (auto& observer : observers_)
        observer.OnFirstSessionStarted();
    }

    session_activation_observer_holder_.NotifyActiveSessionChanged(
        last_active_account_id, user_sessions_[0]->user_info.account_id);

    // When switching to a user for whose PrefService is not ready,
    // |last_active_user_prefs_| continues to point to the PrefService of the
    // most-recently active user with a loaded PrefService.
    PrefService* user_pref_service =
        GetUserPrefServiceForUser(user_sessions_[0]->user_info.account_id);
    if (user_pref_service)
      last_active_user_prefs_ = user_pref_service;

    for (auto& observer : observers_) {
      observer.OnActiveUserSessionChanged(
          user_sessions_[0]->user_info.account_id);
    }

    if (user_pref_service)
      MaybeNotifyOnActiveUserPrefServiceChanged();

    UpdateLoginStatus();
  }
}

void SessionControllerImpl::PrepareForLock(PrepareForLockCallback callback) {
  // If the active window is fullscreen, exit fullscreen to avoid the web page
  // or app mimicking the lock screen. Do not exit fullscreen if the shelf is
  // visible while in fullscreen because the shelf makes it harder for a web
  // page or app to mimick the lock screen.
  wm::WindowState* active_window_state = wm::GetActiveWindowState();
  if (active_window_state && active_window_state->IsFullscreen() &&
      active_window_state->GetHideShelfWhenFullscreen()) {
    const wm::WMEvent event(wm::WM_EVENT_TOGGLE_FULLSCREEN);
    active_window_state->OnWMEvent(&event);
  }

  std::move(callback).Run();
}

void SessionControllerImpl::StartLock(StartLockCallback callback) {
  DCHECK(start_lock_callback_.is_null());
  start_lock_callback_ = std::move(callback);

  LockStateController* const lock_state_controller =
      Shell::Get()->lock_state_controller();

  lock_state_controller->SetLockScreenDisplayedCallback(
      base::BindOnce(&SessionControllerImpl::OnLockAnimationFinished,
                     weak_ptr_factory_.GetWeakPtr()));
  lock_state_controller->OnStartingLock();
}

void SessionControllerImpl::NotifyChromeLockAnimationsComplete() {
  Shell::Get()->power_event_observer()->OnLockAnimationsComplete();
}

void SessionControllerImpl::RunUnlockAnimation(
    RunUnlockAnimationCallback callback) {
  is_unlocking_ = true;

  // Shell could have no instance in tests.
  if (Shell::HasInstance())
    Shell::Get()->lock_state_controller()->OnLockScreenHide(
        std::move(callback));
}

void SessionControllerImpl::NotifyChromeTerminating() {
  for (auto& observer : observers_)
    observer.OnChromeTerminating();
}

void SessionControllerImpl::SetSessionLengthLimit(base::TimeDelta length_limit,
                                                  base::TimeTicks start_time) {
  session_length_limit_ = length_limit;
  session_start_time_ = start_time;
  for (auto& observer : observers_)
    observer.OnSessionLengthLimitChanged();
}

void SessionControllerImpl::CanSwitchActiveUser(
    CanSwitchActiveUserCallback callback) {
  // Cancel overview mode when switching user profiles.
  Shell::Get()->overview_controller()->EndOverview();

  ash::Shell::Get()
      ->screen_switch_check_controller()
      ->CanSwitchAwayFromActiveUser(std::move(callback));
}

void SessionControllerImpl::ShowMultiprofilesIntroDialog(
    ShowMultiprofilesIntroDialogCallback callback) {
  MultiprofilesIntroDialog::Show(std::move(callback));
}

void SessionControllerImpl::ShowTeleportWarningDialog(
    ShowTeleportWarningDialogCallback callback) {
  TeleportWarningDialog::Show(std::move(callback));
}

void SessionControllerImpl::ShowMultiprofilesSessionAbortedDialog(
    const std::string& user_email) {
  SessionAbortedDialog::Show(user_email);
}

void SessionControllerImpl::AddSessionActivationObserverForAccountId(
    const AccountId& account_id,
    SessionActivationObserver* observer) {
  bool locked = state_ == SessionState::LOCKED;
  observer->OnLockStateChanged(locked);
  observer->OnSessionActivated(user_sessions_.size() &&
                               user_sessions_[0]->user_info.account_id ==
                                   account_id);
  session_activation_observer_holder_.AddForAccountId(account_id, observer);
}

void SessionControllerImpl::RemoveSessionActivationObserverForAccountId(
    const AccountId& account_id,
    SessionActivationObserver* observer) {
  session_activation_observer_holder_.RemoveForAccountId(account_id, observer);
}

void SessionControllerImpl::ClearUserSessionsForTest() {
  user_sessions_.clear();
  last_active_user_prefs_ = nullptr;
  active_session_id_ = 0u;
  primary_session_id_ = 0u;
}

void SessionControllerImpl::SetIsDemoSession() {
  if (is_demo_session_)
    return;

  is_demo_session_ = true;
  Shell::Get()->metrics()->StartDemoSessionMetricsRecording();
  // Notifications should be silenced during demo sessions.
  message_center::MessageCenter::Get()->SetQuietMode(true);
}

void SessionControllerImpl::SetSessionState(SessionState state) {
  if (state_ == state)
    return;

  const bool was_user_session_blocked = IsUserSessionBlocked();
  const bool was_locked = state_ == SessionState::LOCKED;
  state_ = state;
  for (auto& observer : observers_)
    observer.OnSessionStateChanged(state_);

  UpdateLoginStatus();

  const bool locked = state_ == SessionState::LOCKED;
  if (was_locked != locked) {
    if (!locked)
      is_unlocking_ = false;

    for (auto& observer : observers_)
      observer.OnLockStateChanged(locked);

    session_activation_observer_holder_.NotifyLockStateChanged(locked);
  }

  EnsureSigninScreenPrefService();

  if (was_user_session_blocked && !IsUserSessionBlocked())
    EnsureActiveWindowAfterUnblockingUserSession();
}

void SessionControllerImpl::AddUserSession(const UserSession& user_session) {
  const AccountId account_id(user_session.user_info.account_id);

  if (primary_session_id_ == 0u)
    primary_session_id_ = user_session.session_id;

  user_sessions_.push_back(std::make_unique<UserSession>(user_session));

  OnProfilePrefServiceInitialized(account_id,
                                  GetUserPrefServiceForUser(account_id));
  UpdateLoginStatus();
  for (auto& observer : observers_)
    observer.OnUserSessionAdded(account_id);
}

LoginStatus SessionControllerImpl::CalculateLoginStatus() const {
  // TODO(jamescook|xiyuan): There is not a 1:1 mapping of SessionState to
  // LoginStatus. Fix the cases that don't match. http://crbug.com/701193
  switch (state_) {
    case SessionState::UNKNOWN:
    case SessionState::OOBE:
    case SessionState::LOGIN_PRIMARY:
    case SessionState::LOGGED_IN_NOT_ACTIVE:
      return LoginStatus::NOT_LOGGED_IN;

    case SessionState::ACTIVE:
      return CalculateLoginStatusForActiveSession();

    case SessionState::LOCKED:
      return LoginStatus::LOCKED;

    case SessionState::LOGIN_SECONDARY:
      // TODO: There is no LoginStatus for this.
      return LoginStatus::USER;
  }
  NOTREACHED();
  return LoginStatus::NOT_LOGGED_IN;
}

LoginStatus SessionControllerImpl::CalculateLoginStatusForActiveSession()
    const {
  DCHECK(state_ == SessionState::ACTIVE);

  if (user_sessions_.empty())  // Can be empty in tests.
    return LoginStatus::USER;

  switch (user_sessions_[0]->user_info.type) {
    case user_manager::USER_TYPE_REGULAR:
      return LoginStatus::USER;
    case user_manager::USER_TYPE_GUEST:
      return LoginStatus::GUEST;
    case user_manager::USER_TYPE_PUBLIC_ACCOUNT:
      return LoginStatus::PUBLIC;
    case user_manager::USER_TYPE_SUPERVISED:
      return LoginStatus::SUPERVISED;
    case user_manager::USER_TYPE_KIOSK_APP:
      return LoginStatus::KIOSK_APP;
    case user_manager::USER_TYPE_CHILD:
      return LoginStatus::SUPERVISED;
    case user_manager::USER_TYPE_ARC_KIOSK_APP:
      return LoginStatus::ARC_KIOSK_APP;
    case user_manager::USER_TYPE_ACTIVE_DIRECTORY:
      // TODO: There is no LoginStatus for this.
      return LoginStatus::USER;
    case user_manager::NUM_USER_TYPES:
      // Avoid having a "default" case so the compiler catches new enum values.
      NOTREACHED();
      return LoginStatus::USER;
  }
  NOTREACHED();
  return LoginStatus::USER;
}

void SessionControllerImpl::UpdateLoginStatus() {
  const LoginStatus new_login_status = CalculateLoginStatus();
  if (new_login_status == login_status_)
    return;

  login_status_ = new_login_status;
  for (auto& observer : observers_)
    observer.OnLoginStatusChanged(login_status_);
}

void SessionControllerImpl::OnLockAnimationFinished() {
  if (!start_lock_callback_.is_null())
    std::move(start_lock_callback_).Run(true /* locked */);
}

void SessionControllerImpl::EnsureSigninScreenPrefService() {
  // Obtain and notify signin profile prefs only once.
  if (signin_screen_prefs_obtained_)
    return;

  PrefService* const signin_prefs = GetSigninScreenPrefService();
  if (!signin_prefs)
    return;

  OnSigninScreenPrefServiceInitialized(signin_prefs);
}

void SessionControllerImpl::OnSigninScreenPrefServiceInitialized(
    PrefService* pref_service) {
  DCHECK(pref_service);
  DCHECK(!signin_screen_prefs_obtained_);

  signin_screen_prefs_obtained_ = true;

  for (auto& observer : observers_)
    observer.OnSigninScreenPrefServiceInitialized(pref_service);

  if (on_active_user_prefs_changed_notify_deferred_) {
    // Notify obsevers with the deferred OnActiveUserPrefServiceChanged(). Do
    // this in a separate loop from the above since observers might depend on
    // each other and we want to avoid having inconsistent states.
    for (auto& observer : observers_)
      observer.OnActiveUserPrefServiceChanged(last_active_user_prefs_);
    on_active_user_prefs_changed_notify_deferred_ = false;
  }
}

void SessionControllerImpl::OnProfilePrefServiceInitialized(
    const AccountId& account_id,
    PrefService* pref_service) {
  // |pref_service| can be null in tests.
  if (!pref_service)
    return;

  DCHECK(!user_sessions_.empty());
  if (account_id == user_sessions_[0]->user_info.account_id) {
    last_active_user_prefs_ = pref_service;

    MaybeNotifyOnActiveUserPrefServiceChanged();
  }
}

void SessionControllerImpl::MaybeNotifyOnActiveUserPrefServiceChanged() {
  DCHECK(last_active_user_prefs_);

  if (!signin_screen_prefs_obtained_) {
    // We must guarantee that OnSigninScreenPrefServiceInitialized() is called
    // before OnActiveUserPrefServiceChanged(), so defer notifying the
    // observers until the sign in prefs are received.
    on_active_user_prefs_changed_notify_deferred_ = true;
    return;
  }

  for (auto& observer : observers_)
    observer.OnActiveUserPrefServiceChanged(last_active_user_prefs_);
}

void SessionControllerImpl::EnsureActiveWindowAfterUnblockingUserSession() {
  // This happens only in tests (See SessionControllerImplTest).
  if (!Shell::HasInstance())
    return;

  auto mru_list =
      Shell::Get()->mru_window_tracker()->BuildMruWindowList(kActiveDesk);
  if (!mru_list.empty())
    mru_list.front()->Focus();
}

}  // namespace ash
