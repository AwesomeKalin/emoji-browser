// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/signin/core/browser/account_info_fetcher.h"

#include <utility>

#include "base/trace_event/trace_event.h"
#include "components/signin/core/browser/account_fetcher_service.h"
#include "google_apis/gaia/gaia_constants.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"

AccountInfoFetcher::AccountInfoFetcher(
    OAuth2TokenService* token_service,
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
    AccountFetcherService* service,
    const CoreAccountId& account_id)
    : OAuth2TokenService::Consumer("gaia_account_tracker"),
      token_service_(token_service),
      url_loader_factory_(std::move(url_loader_factory)),
      service_(service),
      account_id_(account_id) {
  TRACE_EVENT_ASYNC_BEGIN1("AccountFetcherService", "AccountIdFetcher", this,
                           "account_id", account_id.id);
}

AccountInfoFetcher::~AccountInfoFetcher() {
  TRACE_EVENT_ASYNC_END0("AccountFetcherService", "AccountIdFetcher", this);
}

void AccountInfoFetcher::Start() {
  OAuth2TokenService::ScopeSet scopes;
  scopes.insert(GaiaConstants::kGoogleUserInfoEmail);
  scopes.insert(GaiaConstants::kGoogleUserInfoProfile);
  login_token_request_ =
      token_service_->StartRequest(account_id_, scopes, this);
}

void AccountInfoFetcher::OnGetTokenSuccess(
    const OAuth2TokenService::Request* request,
    const OAuth2AccessTokenConsumer::TokenResponse& token_response) {
  TRACE_EVENT_ASYNC_STEP_PAST0("AccountFetcherService", "AccountIdFetcher",
                               this, "OnGetTokenSuccess");
  DCHECK_EQ(request, login_token_request_.get());

  gaia_oauth_client_.reset(new gaia::GaiaOAuthClient(url_loader_factory_));
  const int kMaxRetries = 3;
  gaia_oauth_client_->GetUserInfo(token_response.access_token, kMaxRetries,
                                  this);
}

void AccountInfoFetcher::OnGetTokenFailure(
    const OAuth2TokenService::Request* request,
    const GoogleServiceAuthError& error) {
  TRACE_EVENT_ASYNC_STEP_PAST1("AccountFetcherService", "AccountIdFetcher",
                               this, "OnGetTokenFailure",
                               "google_service_auth_error", error.ToString());
  LOG(ERROR) << "OnGetTokenFailure: " << error.ToString();
  DCHECK_EQ(request, login_token_request_.get());
  service_->OnUserInfoFetchFailure(account_id_);
}

void AccountInfoFetcher::OnGetUserInfoResponse(
    std::unique_ptr<base::DictionaryValue> user_info) {
  TRACE_EVENT_ASYNC_STEP_PAST1("AccountFetcherService", "AccountIdFetcher",
                               this, "OnGetUserInfoResponse", "account_id",
                               account_id_.id);
  service_->OnUserInfoFetchSuccess(account_id_, std::move(user_info));
}

void AccountInfoFetcher::OnOAuthError() {
  TRACE_EVENT_ASYNC_STEP_PAST0("AccountFetcherService", "AccountIdFetcher",
                               this, "OnOAuthError");
  LOG(ERROR) << "OnOAuthError";
  service_->OnUserInfoFetchFailure(account_id_);
}

void AccountInfoFetcher::OnNetworkError(int response_code) {
  TRACE_EVENT_ASYNC_STEP_PAST1("AccountFetcherService", "AccountIdFetcher",
                               this, "OnNetworkError", "response_code",
                               response_code);
  LOG(ERROR) << "OnNetworkError " << response_code;
  service_->OnUserInfoFetchFailure(account_id_);
}
