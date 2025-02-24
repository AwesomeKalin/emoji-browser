// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/identity/public/cpp/identity_test_utils.h"

#include <utility>
#include <vector>

#include "base/run_loop.h"
#include "base/strings/string_split.h"
#include "components/signin/core/browser/account_tracker_service.h"
#include "components/signin/core/browser/gaia_cookie_manager_service.h"
#include "components/signin/core/browser/list_accounts_test_utils.h"
#include "components/signin/core/browser/profile_oauth2_token_service.h"
#include "google_apis/gaia/gaia_auth_util.h"
#include "services/identity/public/cpp/identity_manager.h"
#include "services/identity/public/cpp/primary_account_mutator.h"
#include "services/identity/public/cpp/test_identity_manager_observer.h"

#if defined(OS_ANDROID)
#include "components/signin/core/browser/oauth2_token_service_delegate_android.h"
#endif

namespace identity {

namespace {

void WaitForLoadCredentialsToComplete(IdentityManager* identity_manager) {
  base::RunLoop run_loop;
  TestIdentityManagerObserver load_credentials_observer(identity_manager);
  load_credentials_observer.SetOnRefreshTokensLoadedCallback(
      run_loop.QuitClosure());

  if (identity_manager->AreRefreshTokensLoaded())
    return;

  // Do NOT explicitly load credentials here:
  // 1. It is not re-entrant and will DCHECK fail.
  // 2. It should have been called by IdentityManager during its initialization.

  run_loop.Run();
}

// Helper function that updates the refresh token for |account_id| to
// |new_token|. Before updating the refresh token, blocks until refresh tokens
// are loaded. After updating the token, blocks until the update is processed by
// |identity_manager|.
void UpdateRefreshTokenForAccount(
    ProfileOAuth2TokenService* token_service,
    AccountTrackerService* account_tracker_service,
    IdentityManager* identity_manager,
    const std::string& account_id,
    const std::string& new_token) {
  DCHECK_EQ(account_tracker_service->GetAccountInfo(account_id).account_id,
            account_id)
      << "To set the refresh token for an unknown account, use "
         "MakeAccountAvailable()";

  // Ensure that refresh tokens are loaded; some platforms enforce the invariant
  // that refresh token mutation cannot occur until refresh tokens are loaded,
  // and it is desired to eventually enforce that invariant across all
  // platforms.
  WaitForLoadCredentialsToComplete(identity_manager);

  base::RunLoop run_loop;
  TestIdentityManagerObserver token_updated_observer(identity_manager);
  token_updated_observer.SetOnRefreshTokenUpdatedCallback(
      run_loop.QuitClosure());

  token_service->UpdateCredentials(account_id, new_token);

  run_loop.Run();
}

}  // namespace

CoreAccountInfo SetPrimaryAccount(IdentityManager* identity_manager,
                                  const std::string& email) {
  DCHECK(!identity_manager->HasPrimaryAccount());
  PrimaryAccountManager* primary_account_manager =
      identity_manager->GetPrimaryAccountManager();
  DCHECK(!primary_account_manager->IsAuthenticated());

  AccountTrackerService* account_tracker_service =
      identity_manager->GetAccountTrackerService();
  AccountInfo account_info =
      account_tracker_service->FindAccountInfoByEmail(email);
  if (account_info.account_id.empty()) {
    std::string gaia_id = GetTestGaiaIdForEmail(email);
    account_tracker_service->SeedAccountInfo(gaia_id, email);
    account_info = account_tracker_service->FindAccountInfoByEmail(email);
  }

  std::string gaia_id = account_info.gaia;
  DCHECK(!gaia_id.empty());

  primary_account_manager->SignIn(email);

  DCHECK(primary_account_manager->IsAuthenticated());
  DCHECK(identity_manager->HasPrimaryAccount());
  return identity_manager->GetPrimaryAccountInfo();
}

void SetRefreshTokenForPrimaryAccount(IdentityManager* identity_manager,
                                      const std::string& token_value) {
  DCHECK(identity_manager->HasPrimaryAccount());
  std::string account_id = identity_manager->GetPrimaryAccountId();
  SetRefreshTokenForAccount(identity_manager, account_id, token_value);
}

void SetInvalidRefreshTokenForPrimaryAccount(
    IdentityManager* identity_manager) {
  DCHECK(identity_manager->HasPrimaryAccount());
  std::string account_id = identity_manager->GetPrimaryAccountId();

  SetInvalidRefreshTokenForAccount(identity_manager, account_id);
}

void RemoveRefreshTokenForPrimaryAccount(IdentityManager* identity_manager) {
  if (!identity_manager->HasPrimaryAccount())
    return;

  std::string account_id = identity_manager->GetPrimaryAccountId();

  RemoveRefreshTokenForAccount(identity_manager, account_id);
}

AccountInfo MakePrimaryAccountAvailable(IdentityManager* identity_manager,
                                        const std::string& email) {
  CoreAccountInfo account_info = SetPrimaryAccount(identity_manager, email);
  SetRefreshTokenForPrimaryAccount(identity_manager);
  base::Optional<AccountInfo> primary_account_info =
      identity_manager->FindAccountInfoForAccountWithRefreshTokenByAccountId(
          account_info.account_id);
  // Ensure that extended information for the account is available after setting
  // the refresh token.
  DCHECK(primary_account_info.has_value());
  return primary_account_info.value();
}

void ClearPrimaryAccount(IdentityManager* identity_manager,
                         ClearPrimaryAccountPolicy policy) {
#if defined(OS_CHROMEOS)
  // TODO(blundell): If we ever need this functionality on ChromeOS (which seems
  // unlikely), plumb this through to just clear the primary account info
  // synchronously with IdentityManager.
  NOTREACHED();
#else
  if (!identity_manager->HasPrimaryAccount())
    return;

  base::RunLoop run_loop;
  TestIdentityManagerObserver signout_observer(identity_manager);
  signout_observer.SetOnPrimaryAccountClearedCallback(run_loop.QuitClosure());

  PrimaryAccountManager* primary_account_manager =
      identity_manager->GetPrimaryAccountManager();
  signin_metrics::ProfileSignout signout_source_metric =
      signin_metrics::SIGNOUT_TEST;
  signin_metrics::SignoutDelete signout_delete_metric =
      signin_metrics::SignoutDelete::IGNORE_METRIC;

  switch (policy) {
    case ClearPrimaryAccountPolicy::DEFAULT:
      primary_account_manager->SignOut(signout_source_metric,
                                       signout_delete_metric);
      break;
    case ClearPrimaryAccountPolicy::KEEP_ALL_ACCOUNTS:
      primary_account_manager->SignOutAndKeepAllAccounts(signout_source_metric,
                                                         signout_delete_metric);
      break;
    case ClearPrimaryAccountPolicy::REMOVE_ALL_ACCOUNTS:
      primary_account_manager->SignOutAndRemoveAllAccounts(
          signout_source_metric, signout_delete_metric);
      break;
  }

  run_loop.Run();
#endif
}

AccountInfo MakeAccountAvailable(IdentityManager* identity_manager,
                                 const std::string& email) {
  AccountTrackerService* account_tracker_service =
      identity_manager->GetAccountTrackerService();

  DCHECK(account_tracker_service);
  DCHECK(account_tracker_service->FindAccountInfoByEmail(email).IsEmpty());

  // Wait until tokens are loaded, otherwise the account will be removed as soon
  // as tokens finish loading.
  WaitForLoadCredentialsToComplete(identity_manager);

  std::string gaia_id = GetTestGaiaIdForEmail(email);
  account_tracker_service->SeedAccountInfo(gaia_id, email);

  AccountInfo account_info =
      account_tracker_service->FindAccountInfoByEmail(email);
  DCHECK(!account_info.account_id.empty());

  SetRefreshTokenForAccount(identity_manager, account_info.account_id);

  return account_info;
}

void SetRefreshTokenForAccount(IdentityManager* identity_manager,
                               const std::string& account_id,
                               const std::string& token_value) {
  UpdateRefreshTokenForAccount(
      identity_manager->GetTokenService(),
      identity_manager->GetAccountTrackerService(), identity_manager,
      account_id,
      token_value.empty() ? "refresh_token_for_" + account_id : token_value);
}

void SetInvalidRefreshTokenForAccount(IdentityManager* identity_manager,
                                      const std::string& account_id) {
  UpdateRefreshTokenForAccount(
      identity_manager->GetTokenService(),

      identity_manager->GetAccountTrackerService(), identity_manager,
      account_id, OAuth2TokenServiceDelegate::kInvalidRefreshToken);
}

void RemoveRefreshTokenForAccount(IdentityManager* identity_manager,
                                  const std::string& account_id) {
  if (!identity_manager->HasAccountWithRefreshToken(account_id))
    return;

  base::RunLoop run_loop;
  TestIdentityManagerObserver token_updated_observer(identity_manager);
  token_updated_observer.SetOnRefreshTokenRemovedCallback(
      run_loop.QuitClosure());

  identity_manager->GetTokenService()->RevokeCredentials(account_id);

  run_loop.Run();
}

void SetCookieAccounts(IdentityManager* identity_manager,
                       network::TestURLLoaderFactory* test_url_loader_factory,
                       const std::vector<CookieParams>& cookie_accounts) {
  // Convert |cookie_accounts| to the format list_accounts_test_utils wants.
  std::vector<signin::CookieParams> gaia_cookie_accounts;
  for (const CookieParams& params : cookie_accounts) {
    gaia_cookie_accounts.push_back({params.email, params.gaia_id,
                                    /*valid=*/true, /*signed_out=*/false,
                                    /*verified=*/true});
  }

  base::RunLoop run_loop;
  TestIdentityManagerObserver cookie_observer(identity_manager);
  cookie_observer.SetOnAccountsInCookieUpdatedCallback(run_loop.QuitClosure());

  signin::SetListAccountsResponseWithParams(gaia_cookie_accounts,
                                            test_url_loader_factory);

  GaiaCookieManagerService* cookie_manager =
      identity_manager->GetGaiaCookieManagerService();
  cookie_manager->set_list_accounts_stale_for_testing(true);
  cookie_manager->ListAccounts(nullptr, nullptr);

  run_loop.Run();
}

void UpdateAccountInfoForAccount(IdentityManager* identity_manager,
                                 AccountInfo account_info) {
  // Make sure the account being updated is a known account.

  AccountTrackerService* account_tracker_service =
      identity_manager->GetAccountTrackerService();

  DCHECK(account_tracker_service);
  DCHECK(!account_tracker_service->GetAccountInfo(account_info.account_id)
              .account_id.empty());

  account_tracker_service->SeedAccountInfo(account_info);
}

void SetFreshnessOfAccountsInGaiaCookie(IdentityManager* identity_manager,
                                        bool accounts_are_fresh) {
  GaiaCookieManagerService* cookie_manager =
      identity_manager->GetGaiaCookieManagerService();
  cookie_manager->set_list_accounts_stale_for_testing(!accounts_are_fresh);
}

std::string GetTestGaiaIdForEmail(const std::string& email) {
  std::string gaia_id =
      std::string("gaia_id_for_") + gaia::CanonicalizeEmail(email);
  // Avoid character '@' in the gaia ID string as there is code in the codebase
  // that asserts that a gaia ID does not contain a "@" character.
  std::replace(gaia_id.begin(), gaia_id.end(), '@', '_');
  return gaia_id;
}

void UpdatePersistentErrorOfRefreshTokenForAccount(
    IdentityManager* identity_manager,
    const std::string& account_id,
    const GoogleServiceAuthError& auth_error) {
  DCHECK(identity_manager->HasAccountWithRefreshToken(account_id));
  identity_manager->GetTokenService()->GetDelegate()->UpdateAuthError(
      account_id, auth_error);
}

void DisableAccessTokenFetchRetries(IdentityManager* identity_manager) {
  identity_manager->GetTokenService()
      ->set_max_authorization_token_fetch_retries_for_testing(0);
}

#if defined(OS_ANDROID)
void DisableInteractionWithSystemAccounts() {
  OAuth2TokenServiceDelegateAndroid::
      set_disable_interaction_with_system_accounts();
}
#endif

void CancelAllOngoingGaiaCookieOperations(IdentityManager* identity_manager) {
  identity_manager->GetGaiaCookieManagerService()->CancelAll();
}

void SimulateSuccessfulFetchOfAccountInfo(IdentityManager* identity_manager,
                                          const std::string& account_id,
                                          const std::string& email,
                                          const std::string& gaia,
                                          const std::string& hosted_domain,
                                          const std::string& full_name,
                                          const std::string& given_name,
                                          const std::string& locale,
                                          const std::string& picture_url) {
  base::DictionaryValue user_info;
  user_info.SetString("id", gaia);
  user_info.SetString("email", email);
  user_info.SetString("hd", hosted_domain);
  user_info.SetString("name", full_name);
  user_info.SetString("given_name", given_name);
  user_info.SetString("locale", locale);
  user_info.SetString("picture", picture_url);

  AccountTrackerService* account_tracker_service =
      identity_manager->GetAccountTrackerService();
  account_tracker_service->SetAccountInfoFromUserInfo(account_id, &user_info);
}

}  // namespace identity
