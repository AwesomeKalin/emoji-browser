// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_IDENTITY_PUBLIC_CPP_PRIMARY_ACCOUNT_MUTATOR_H_
#define SERVICES_IDENTITY_PUBLIC_CPP_PRIMARY_ACCOUNT_MUTATOR_H_

#include <string>

#include "base/callback_forward.h"
#include "components/signin/core/browser/signin_metrics.h"

namespace identity {

// PrimaryAccountMutator is the interface to set and clear the primary account
// (see IdentityManager for more information).
//
// It is a pure interface that has concrete implementation on platform that
// support changing the signed-in state during the lifetime of the application.
// On other platforms, there is no implementation, and no instance will be
// available at runtime (thus accessors may return null).
class PrimaryAccountMutator {
 public:
  // Represents the options for handling the accounts known to the
  // IdentityManager upon calling ClearPrimaryAccount().
  enum class ClearAccountsAction {
    kDefault,    // Default action based on internal policy.
    kKeepAll,    // Keep all accounts.
    kRemoveAll,  // Remove all accounts.
  };

  PrimaryAccountMutator() = default;
  virtual ~PrimaryAccountMutator() = default;

  // PrimaryAccountMutator is non-copyable, non-moveable.
  PrimaryAccountMutator(PrimaryAccountMutator&& other) = delete;
  PrimaryAccountMutator const& operator=(PrimaryAccountMutator&& other) =
      delete;

  PrimaryAccountMutator(const PrimaryAccountMutator& other) = delete;
  PrimaryAccountMutator const& operator=(const PrimaryAccountMutator& other) =
      delete;

#if !defined(OS_CHROMEOS)
  // Marks the account with |account_id| as the primary account, and returns
  // whether the operation succeeded or not. To succeed, this requires that:
  //    - setting the primary account is allowed,
  //    - the account username is allowed by policy,
  //    - there is not already a primary account set,
  //    - the account is known by the IdentityManager.
  virtual bool SetPrimaryAccount(const std::string& account_id) = 0;

  // Clears the primary account, and returns whether the operation
  // succeeded or not. Depending on |action|, the other accounts
  // known to the IdentityManager may be deleted.
  virtual bool ClearPrimaryAccount(
      ClearAccountsAction action,
      signin_metrics::ProfileSignout source_metric,
      signin_metrics::SignoutDelete delete_metric) = 0;
#else
  // Updates the info of the account corresponding to (|gaia_id|, |email|),
  // marks it as the primary account, and returns whether the operation
  // succeeded or not. Currently, this method is guaranteed to succeed.
  // NOTE: Unlike SetPrimaryAccount(), this method does not require that the
  // account is known by IdentityManager. The reason is that on ChromeOS the
  // primary account is in fact not guaranteed to be known by IdentityManager
  // when it is set. TODO(https://crbug.com/967605): Port callers to
  // SetPrimaryAccount() once https://crbug.com/867602 is fixed."
  virtual bool SetPrimaryAccountAndUpdateAccountInfo(
      const std::string& gaia_id,
      const std::string& email) = 0;
#endif
};

}  // namespace identity

#endif  // SERVICES_IDENTITY_PUBLIC_CPP_PRIMARY_ACCOUNT_MUTATOR_H_
