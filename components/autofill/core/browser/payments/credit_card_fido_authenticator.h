// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_AUTOFILL_CORE_BROWSER_PAYMENTS_CREDIT_CARD_FIDO_AUTHENTICATOR_H_
#define COMPONENTS_AUTOFILL_CORE_BROWSER_PAYMENTS_CREDIT_CARD_FIDO_AUTHENTICATOR_H_

#include <memory>

#include "base/strings/string16.h"
#include "components/autofill/core/browser/autofill_client.h"
#include "components/autofill/core/browser/data_model/credit_card.h"
#include "components/autofill/core/browser/payments/full_card_request.h"
#include "components/autofill/core/browser/payments/payments_client.h"

namespace autofill {

// Authenticates credit card unmasking through FIDO authentication, using the
// WebAuthn specification, standardized by the FIDO alliance. The Webauthn
// specification defines an API to cryptographically bind a server and client,
// and verify that binding. More information can be found here:
// - https://www.w3.org/TR/webauthn-1/
// - https://fidoalliance.org/fido2/
class CreditCardFIDOAuthenticator {
 public:
  class Requester {
   public:
    virtual ~Requester() {}
    virtual void OnFIDOAuthenticationComplete(
        bool did_succeed,
        const CreditCard* card = nullptr) = 0;
  };
  explicit CreditCardFIDOAuthenticator(AutofillClient* client);
  virtual ~CreditCardFIDOAuthenticator();

  // Authentication
  void Authenticate(const CreditCard* card,
                    base::WeakPtr<Requester> requester,
                    base::Value request_options);

  // Returns true only if user has a verifying platform authenticator.
  // e.g. Touch/Face ID, Windows Hello, Android Fingerprint etc is available and
  // enabled.
  virtual bool IsUserVerifiable();

  // Returns true only if the user has opted-in to use WebAuthn for autofill.
  virtual bool IsUserOptedIn();

 private:
  friend class AutofillManagerTest;
  friend class CreditCardAccessManagerTest;
  friend class CreditCardFIDOAuthenticatorTest;

  // The associated autofill client. Weak reference.
  AutofillClient* const autofill_client_;

  // Payments client to make requests to Google Payments.
  payments::PaymentsClient* const payments_client_;

  // Responsible for getting the full card details, including the PAN and the
  // CVC.
  std::unique_ptr<payments::FullCardRequest> full_card_request_;

  // Weak pointer to object that is requesting authentication.
  base::WeakPtr<Requester> requester_;

  base::WeakPtrFactory<CreditCardFIDOAuthenticator> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN(CreditCardFIDOAuthenticator);
};

}  // namespace autofill

#endif  // COMPONENTS_AUTOFILL_CORE_BROWSER_PAYMENTS_CREDIT_CARD_FIDO_AUTHENTICATOR_H_
