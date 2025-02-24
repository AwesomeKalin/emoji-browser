// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/signin/core/browser/primary_account_policy_manager_impl.h"

#include <string>

#include "base/bind.h"
#include "base/memory/weak_ptr.h"
#include "components/prefs/pref_change_registrar.h"
#include "components/prefs/pref_member.h"
#include "components/prefs/pref_service.h"
#include "components/signin/core/browser/identity_utils.h"
#include "components/signin/core/browser/primary_account_manager.h"
#include "components/signin/core/browser/signin_client.h"
#include "components/signin/core/browser/signin_metrics.h"
#include "components/signin/core/browser/signin_pref_names.h"

PrimaryAccountPolicyManagerImpl::PrimaryAccountPolicyManagerImpl(
    SigninClient* client)
    : client_(client), weak_pointer_factory_(this) {}

PrimaryAccountPolicyManagerImpl::~PrimaryAccountPolicyManagerImpl() {
  local_state_pref_registrar_.RemoveAll();
}

void PrimaryAccountPolicyManagerImpl::InitializePolicy(
    PrefService* local_state,
    PrimaryAccountManager* primary_account_manager) {
  // local_state can be null during unit tests.
  if (local_state) {
    local_state_pref_registrar_.Init(local_state);
    local_state_pref_registrar_.Add(
        prefs::kGoogleServicesUsernamePattern,
        base::Bind(&PrimaryAccountPolicyManagerImpl::
                       OnGoogleServicesUsernamePatternChanged,
                   weak_pointer_factory_.GetWeakPtr(),
                   primary_account_manager));
  }
  signin_allowed_.Init(
      prefs::kSigninAllowed, client_->GetPrefs(),
      base::Bind(&PrimaryAccountPolicyManagerImpl::OnSigninAllowedPrefChanged,
                 base::Unretained(this), primary_account_manager));

  AccountInfo account_info =
      primary_account_manager->GetAuthenticatedAccountInfo();
  if (!account_info.account_id.empty() &&
      (!IsAllowedUsername(account_info.email) || !IsSigninAllowed())) {
    // User is signed in, but the username is invalid or signin is no longer
    // allowed, so the user must be sign out.
    //
    // This may happen in the following cases:
    //   a. The user has toggled off signin allowed in settings.
    //   b. The administrator changed the policy since the last signin.
    //
    // Note: The token service has not yet loaded its credentials, so accounts
    // cannot be revoked here.
    //
    // On desktop, when PrimaryAccountManager is initializing, the profile was
    // not yet marked with sign out allowed. Therefore sign out is not allowed
    // and all calls to SignOut methods are no-op.
    //
    // TODO(msarda): SignOut methods do not guarantee that sign out can actually
    // be done (this depends on whether sign out is allowed). Add a check here
    // on desktop to make it clear that SignOut does not do anything.
    primary_account_manager->SignOutAndKeepAllAccounts(
        signin_metrics::SIGNIN_PREF_CHANGED_DURING_SIGNIN,
        signin_metrics::SignoutDelete::IGNORE_METRIC);
  }
}

void PrimaryAccountPolicyManagerImpl::OnGoogleServicesUsernamePatternChanged(
    PrimaryAccountManager* primary_account_manager) {
  if (primary_account_manager->IsAuthenticated() &&
      !IsAllowedUsername(
          primary_account_manager->GetAuthenticatedAccountInfo().email)) {
    // Signed in user is invalid according to the current policy so sign
    // the user out.
    primary_account_manager->SignOut(
        signin_metrics::GOOGLE_SERVICE_NAME_PATTERN_CHANGED,
        signin_metrics::SignoutDelete::IGNORE_METRIC);
  }
}

bool PrimaryAccountPolicyManagerImpl::IsSigninAllowed() const {
  return signin_allowed_.GetValue();
}

void PrimaryAccountPolicyManagerImpl::OnSigninAllowedPrefChanged(
    PrimaryAccountManager* primary_account_manager) {
  if (!IsSigninAllowed() && primary_account_manager->IsAuthenticated()) {
    VLOG(0) << "IsSigninAllowed() set to false, signing out the user";
    primary_account_manager->SignOut(
        signin_metrics::SIGNOUT_PREF_CHANGED,
        signin_metrics::SignoutDelete::IGNORE_METRIC);
  }
}

bool PrimaryAccountPolicyManagerImpl::IsAllowedUsername(
    const std::string& username) const {
  const PrefService* local_state = local_state_pref_registrar_.prefs();
  if (!local_state)
    return true;  // In a unit test with no local state - all names are allowed.

  std::string pattern =
      local_state->GetString(prefs::kGoogleServicesUsernamePattern);
  return identity::IsUsernameAllowedByPattern(username, pattern);
}
