// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/push_messaging/push_messaging_manager.h"

#include <string>
#include <utility>
#include <vector>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/command_line.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/metrics/histogram_macros.h"
#include "base/optional.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/post_task.h"
#include "content/browser/permissions/permission_controller_impl.h"
#include "content/browser/renderer_host/render_process_host_impl.h"
#include "content/browser/service_worker/service_worker_context_core.h"
#include "content/browser/service_worker/service_worker_context_wrapper.h"
#include "content/browser/service_worker/service_worker_storage.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/permission_type.h"
#include "content/public/browser/push_messaging_service.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/child_process_host.h"
#include "content/public/common/content_switches.h"
#include "third_party/blink/public/mojom/devtools/console_message.mojom.h"
#include "third_party/blink/public/mojom/permissions/permission_status.mojom.h"
#include "third_party/blink/public/mojom/push_messaging/push_messaging.mojom.h"
#include "third_party/blink/public/mojom/push_messaging/push_messaging_status.mojom.h"

namespace content {

// Service Worker database keys. If a registration ID is stored, the stored
// sender ID must be the one used to register. Unfortunately, this isn't always
// true of pre-InstanceID registrations previously stored in the database, but
// fortunately it's less important for their sender ID to be accurate.
const char kPushSenderIdServiceWorkerKey[] = "push_sender_id";
const char kPushRegistrationIdServiceWorkerKey[] = "push_registration_id";

namespace {

// Chrome currently does not support the Push API in incognito.
const char kIncognitoPushUnsupportedMessage[] =
    "Chrome currently does not support the Push API in incognito mode "
    "(https://crbug.com/401439). There is deliberately no way to "
    "feature-detect this, since incognito mode needs to be undetectable by "
    "websites.";

// These UMA methods are called from the IO and/or UI threads. Racey but ok, see
// https://groups.google.com/a/chromium.org/d/msg/chromium-dev/FNzZRJtN2aw/Aw0CWAXJJ1kJ
void RecordRegistrationStatus(blink::mojom::PushRegistrationStatus status) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO) ||
         BrowserThread::CurrentlyOn(BrowserThread::UI));
  UMA_HISTOGRAM_ENUMERATION("PushMessaging.RegistrationStatus", status);
}
void RecordUnregistrationStatus(blink::mojom::PushUnregistrationStatus status) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  UMA_HISTOGRAM_ENUMERATION("PushMessaging.UnregistrationStatus", status);
}
void RecordGetRegistrationStatus(
    blink::mojom::PushGetRegistrationStatus status) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO) ||
         BrowserThread::CurrentlyOn(BrowserThread::UI));
  UMA_HISTOGRAM_ENUMERATION("PushMessaging.GetRegistrationStatus", status);
}

const char* PushUnregistrationStatusToString(
    blink::mojom::PushUnregistrationStatus status) {
  switch (status) {
    case blink::mojom::PushUnregistrationStatus::SUCCESS_UNREGISTERED:
      return "Unregistration successful - from push service";

    case blink::mojom::PushUnregistrationStatus::SUCCESS_WAS_NOT_REGISTERED:
      return "Unregistration successful - was not registered";

    case blink::mojom::PushUnregistrationStatus::PENDING_NETWORK_ERROR:
      return "Unregistration pending - a network error occurred, but it will "
             "be retried until it succeeds";

    case blink::mojom::PushUnregistrationStatus::NO_SERVICE_WORKER:
      return "Unregistration failed - no Service Worker";

    case blink::mojom::PushUnregistrationStatus::SERVICE_NOT_AVAILABLE:
      return "Unregistration failed - push service not available";

    case blink::mojom::PushUnregistrationStatus::PENDING_SERVICE_ERROR:
      return "Unregistration pending - a push service error occurred, but it "
             "will be retried until it succeeds";

    case blink::mojom::PushUnregistrationStatus::STORAGE_ERROR:
      return "Unregistration failed - storage error";

    case blink::mojom::PushUnregistrationStatus::NETWORK_ERROR:
      return "Unregistration failed - could not connect to push server";
  }
  NOTREACHED();
  return "";
}

// Returns whether |application_server_key| contains a valid application server
// key, that is, a NIST P-256 public key in uncompressed format.
bool IsApplicationServerKey(
    const std::vector<uint8_t>& application_server_key) {
  return application_server_key.size() == 65 &&
         application_server_key[0] == 0x04;
}

// Returns application_server_key if non-empty, otherwise checks if
// stored_sender_id may be used as a fallback and if so, returns
// stored_sender_id instead.
//
// This is in order to support the legacy way of subscribing from a service
// worker (first subscribe from the document using a gcm_sender_id set in the
// manifest, and then subscribe from the service worker with no key).
//
// An empty string will be returned if application_server_key is empty and the
// fallback is not a numeric gcm sender id.
std::string FixSenderInfo(const std::string& application_server_key,
                          const std::string& stored_sender_id) {
  if (!application_server_key.empty())
    return application_server_key;
  if (base::ContainsOnlyChars(stored_sender_id, "0123456789"))
    return stored_sender_id;
  return std::string();
}

bool IsRequestFromDocument(int render_frame_id) {
  return render_frame_id != ChildProcessHost::kInvalidUniqueID;
}

}  // namespace

struct PushMessagingManager::RegisterData {
  RegisterData();
  RegisterData(RegisterData&& other) = default;

  GURL requesting_origin;
  int64_t service_worker_registration_id;
  base::Optional<std::string> existing_subscription_id;
  blink::mojom::PushSubscriptionOptionsPtr options;
  SubscribeCallback callback;

  // True if the call to register was made with a user gesture.
  bool user_gesture;
};

// Inner core of the PushMessagingManager which lives on the UI thread.
class PushMessagingManager::Core {
 public:
  Core(const base::WeakPtr<PushMessagingManager>& io_parent,
       int render_process_id,
       int render_frame_id);

  // Public Register methods on UI thread --------------------------------------

  // Called via PostTask from IO thread.
  void RegisterOnUI(RegisterData data);

  // Public Unregister methods on UI thread ------------------------------------

  // Called via PostTask from IO thread.
  void UnregisterFromService(UnsubscribeCallback callback,
                             int64_t service_worker_registration_id,
                             const GURL& requesting_origin,
                             const std::string& sender_id);

  // Public GetSubscription methods on UI thread -------------------------------

  // Callback called on UI thread.
  void GetSubscriptionDidGetInfoOnUI(GetSubscriptionCallback callback,
                                     const GURL& origin,
                                     int64_t service_worker_registration_id,
                                     const GURL& endpoint,
                                     const std::string& application_server_key,
                                     bool is_valid,
                                     const std::vector<uint8_t>& p256dh,
                                     const std::vector<uint8_t>& auth);

  // Callback called on UI thread.
  void GetSubscriptionDidUnsubscribe(
      GetSubscriptionCallback callback,
      blink::mojom::PushGetRegistrationStatus get_status,
      blink::mojom::PushUnregistrationStatus unsubscribe_status);

  // Public helper methods on UI thread ----------------------------------------

  // Called via PostTask from IO thread. |callback| will be run on UI thread.
  void GetSubscriptionInfoOnUI(
      const GURL& origin,
      int64_t service_worker_registration_id,
      const std::string& sender_id,
      const std::string& push_subscription_id,
      PushMessagingService::SubscriptionInfoCallback callback);

  // Called (directly) from both the UI and IO threads.
  bool is_incognito() const { return is_incognito_; }

  // Returns a push messaging service. May return null.
  PushMessagingService* service();

  // Returns a weak ptr. Must only be called on the UI thread (and hence can
  // only be called from the outer class's constructor).
  base::WeakPtr<Core> GetWeakPtrFromIOParentConstructor();

 private:
  friend struct BrowserThread::DeleteOnThread<BrowserThread::UI>;
  friend class base::DeleteHelper<Core>;

  ~Core();

  // Private Register methods on UI thread -------------------------------------

  void DidRequestPermissionInIncognito(RegisterData data,
                                       blink::mojom::PermissionStatus status);

  void DidRegister(RegisterData data,
                   const std::string& push_subscription_id,
                   const std::vector<uint8_t>& p256dh,
                   const std::vector<uint8_t>& auth,
                   blink::mojom::PushRegistrationStatus status);

  // Private Unregister methods on UI thread -----------------------------------

  void DidUnregisterFromService(
      UnsubscribeCallback callback,
      int64_t service_worker_registration_id,
      blink::mojom::PushUnregistrationStatus unregistration_status);

  // Outer part of the PushMessagingManager which lives on the IO thread.
  base::WeakPtr<PushMessagingManager> io_parent_;

  int render_process_id_;
  int render_frame_id_;

  bool is_incognito_;

  base::WeakPtrFactory<Core> weak_factory_ui_to_ui_;

  DISALLOW_COPY_AND_ASSIGN(Core);
};

PushMessagingManager::RegisterData::RegisterData()
    : service_worker_registration_id(0) {}

PushMessagingManager::Core::Core(
    const base::WeakPtr<PushMessagingManager>& io_parent,
    int render_process_id,
    int render_frame_id)
    : io_parent_(io_parent),
      render_process_id_(render_process_id),
      render_frame_id_(render_frame_id),
      weak_factory_ui_to_ui_(this) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  RenderProcessHost* process_host =
      RenderProcessHost::FromID(render_process_id_);  // Can't be null yet.
  is_incognito_ = process_host->GetBrowserContext()->IsOffTheRecord();
}

PushMessagingManager::Core::~Core() {}

PushMessagingManager::PushMessagingManager(
    int render_process_id,
    int render_frame_id,
    ServiceWorkerContextWrapper* service_worker_context)
    : service_worker_context_(service_worker_context),
      render_frame_id_(render_frame_id),
      weak_factory_(this) {
  // Although this class is used only on the IO thread, it is constructed on UI.
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  // Normally, it would be unsafe to obtain a weak pointer from the UI thread,
  // but it's ok in the constructor since we can't be destroyed before our
  // constructor finishes.
  ui_core_.reset(new Core(weak_factory_.GetWeakPtr(), render_process_id,
                          render_frame_id_));
  ui_core_weak_ptr_ = ui_core_->GetWeakPtrFromIOParentConstructor();

  PushMessagingService* service = ui_core_->service();
  service_available_ = !!service;

  if (service_available_) {
    default_endpoint_ = service->GetEndpoint(false /* standard_protocol */);
    web_push_protocol_endpoint_ =
        service->GetEndpoint(true /* standard_protocol */);
  }
}

PushMessagingManager::~PushMessagingManager() {}

void PushMessagingManager::AddPushMessagingReceiver(
    mojo::PendingReceiver<blink::mojom::PushMessaging> receiver) {
  receivers_.Add(this, std::move(receiver));
}

void PushMessagingManager::BindRequest(
    blink::mojom::PushMessagingRequest request) {
  // Implicit conversion to mojo::PendingReceiver<blink::mojom::PushMessaging>.
  AddPushMessagingReceiver(std::move(request));
}

// Subscribe methods on both IO and UI threads, merged in order of use from
// PushMessagingManager and Core.
// -----------------------------------------------------------------------------

void PushMessagingManager::Subscribe(
    int64_t service_worker_registration_id,
    blink::mojom::PushSubscriptionOptionsPtr options,
    bool user_gesture,
    SubscribeCallback callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  DCHECK(options);

  // TODO(mvanouwerkerk): Validate arguments?
  RegisterData data;

  data.service_worker_registration_id = service_worker_registration_id;
  data.callback = std::move(callback);
  data.options = std::move(options);
  data.user_gesture = user_gesture;

  ServiceWorkerRegistration* service_worker_registration =
      service_worker_context_->GetLiveRegistration(
          data.service_worker_registration_id);
  if (!service_worker_registration ||
      !service_worker_registration->active_version()) {
    SendSubscriptionError(
        std::move(data),
        blink::mojom::PushRegistrationStatus::NO_SERVICE_WORKER);
    return;
  }
  data.requesting_origin = service_worker_registration->scope().GetOrigin();

  DCHECK(!(data.options->application_server_key.empty() &&
           IsRequestFromDocument(render_frame_id_)));

  int64_t registration_id = data.service_worker_registration_id;
  service_worker_context_->GetRegistrationUserData(
      registration_id,
      {kPushRegistrationIdServiceWorkerKey, kPushSenderIdServiceWorkerKey},
      base::BindOnce(&PushMessagingManager::DidCheckForExistingRegistration,
                     weak_factory_.GetWeakPtr(), std::move(data)));
}

void PushMessagingManager::DidCheckForExistingRegistration(
    RegisterData data,
    const std::vector<std::string>& subscription_id_and_sender_id,
    blink::ServiceWorkerStatusCode service_worker_status) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  // Validate the stored subscription against the subscription request made by
  // the developer. The authorized entity must match.
  if (service_worker_status == blink::ServiceWorkerStatusCode::kOk) {
    DCHECK_EQ(2u, subscription_id_and_sender_id.size());

    const std::string& subscription_id = subscription_id_and_sender_id[0];
    const std::string& stored_sender_id = subscription_id_and_sender_id[1];

    const std::string application_server_key_string(
        data.options->application_server_key.begin(),
        data.options->application_server_key.end());

    std::string fixed_sender_id(
        FixSenderInfo(application_server_key_string, stored_sender_id));
    if (fixed_sender_id.empty()) {
      SendSubscriptionError(std::move(data),
                            blink::mojom::PushRegistrationStatus::NO_SENDER_ID);
      return;
    }

    if (fixed_sender_id != stored_sender_id) {
      SendSubscriptionError(
          std::move(data),
          blink::mojom::PushRegistrationStatus::SENDER_ID_MISMATCH);
      return;
    }

    data.existing_subscription_id = subscription_id;
  }

  // TODO(peter): Handle failures other than
  // blink::ServiceWorkerStatusCode::kErrorNotFound by rejecting
  // the subscription algorithm instead of trying to subscribe.

  if (!data.options->application_server_key.empty()) {
    base::PostTaskWithTraits(
        FROM_HERE, {BrowserThread::UI},
        base::BindOnce(&Core::RegisterOnUI, base::Unretained(ui_core_.get()),
                       std::move(data)));
  } else {
    // No |application_server_key| was provided by the developer. Fall back to
    // checking whether a previous subscription did identify a sender.
    int64_t registration_id = data.service_worker_registration_id;
    service_worker_context_->GetRegistrationUserData(
        registration_id, {kPushSenderIdServiceWorkerKey},
        base::BindOnce(&PushMessagingManager::DidGetSenderIdFromStorage,
                       weak_factory_.GetWeakPtr(), std::move(data)));
  }
}

void PushMessagingManager::DidGetSenderIdFromStorage(
    RegisterData data,
    const std::vector<std::string>& stored_sender_id,
    blink::ServiceWorkerStatusCode service_worker_status) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  if (service_worker_status != blink::ServiceWorkerStatusCode::kOk) {
    SendSubscriptionError(std::move(data),
                          blink::mojom::PushRegistrationStatus::NO_SENDER_ID);
    return;
  }
  DCHECK_EQ(1u, stored_sender_id.size());
  // We should only be here because no sender info was supplied to subscribe().
  DCHECK(data.options->application_server_key.empty());

  const std::string application_server_key_string(
      std::string(data.options->application_server_key.begin(),
                  data.options->application_server_key.end()));
  std::string fixed_sender_id(
      FixSenderInfo(application_server_key_string, stored_sender_id[0]));
  if (fixed_sender_id.empty()) {
    SendSubscriptionError(std::move(data),
                          blink::mojom::PushRegistrationStatus::NO_SENDER_ID);
    return;
  }
  data.options->application_server_key =
      std::vector<uint8_t>(fixed_sender_id.begin(), fixed_sender_id.end());
  base::PostTaskWithTraits(
      FROM_HERE, {BrowserThread::UI},
      base::BindOnce(&Core::RegisterOnUI, base::Unretained(ui_core_.get()),
                     std::move(data)));
}

void PushMessagingManager::Core::RegisterOnUI(
    PushMessagingManager::RegisterData data) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  PushMessagingService* push_service = service();
  if (!push_service) {
    if (!is_incognito()) {
      // This might happen if InstanceIDProfileService::IsInstanceIDEnabled
      // returns false because the Instance ID kill switch was enabled.
      base::PostTaskWithTraits(
          FROM_HERE, {BrowserThread::IO},
          base::BindOnce(
              &PushMessagingManager::SendSubscriptionError, io_parent_,
              std::move(data),
              blink::mojom::PushRegistrationStatus::SERVICE_NOT_AVAILABLE));
    } else {
      // Prevent websites from detecting incognito mode, by emulating what would
      // have happened if we had a PushMessagingService available.
      if (!IsRequestFromDocument(render_frame_id_) ||
          !data.options->user_visible_only) {
        // Throw a permission denied error under the same circumstances.
        base::PostTaskWithTraits(
            FROM_HERE, {BrowserThread::IO},
            base::BindOnce(&PushMessagingManager::SendSubscriptionError,
                           io_parent_, std::move(data),
                           blink::mojom::PushRegistrationStatus::
                               INCOGNITO_PERMISSION_DENIED));
      } else {
        RenderFrameHost* render_frame_host =
            RenderFrameHost::FromID(render_process_id_, render_frame_id_);
        WebContents* web_contents =
            WebContents::FromRenderFrameHost(render_frame_host);
        if (web_contents) {
          web_contents->GetMainFrame()->AddMessageToConsole(
              blink::mojom::ConsoleMessageLevel::kError,
              kIncognitoPushUnsupportedMessage);

          BrowserContext* browser_context = web_contents->GetBrowserContext();

          // Request notifications permission (which will fail, since
          // notifications aren't supported in incognito), so the website can't
          // detect whether incognito is active.
          PermissionControllerImpl::FromBrowserContext(browser_context)
              ->RequestPermission(
                  PermissionType::NOTIFICATIONS, render_frame_host,
                  data.requesting_origin, data.user_gesture,
                  base::Bind(&PushMessagingManager::Core::
                                 DidRequestPermissionInIncognito,
                             weak_factory_ui_to_ui_.GetWeakPtr(),
                             base::Passed(&data)));
        }
      }
    }
    return;
  }

  int64_t registration_id = data.service_worker_registration_id;
  GURL requesting_origin = data.requesting_origin;

  auto options = data.options->Clone();
  if (IsRequestFromDocument(render_frame_id_)) {
    push_service->SubscribeFromDocument(
        requesting_origin, registration_id, render_process_id_,
        render_frame_id_, std::move(options), data.user_gesture,
        base::Bind(&Core::DidRegister, weak_factory_ui_to_ui_.GetWeakPtr(),
                   base::Passed(&data)));
  } else {
    push_service->SubscribeFromWorker(
        requesting_origin, registration_id, std::move(options),
        base::Bind(&Core::DidRegister, weak_factory_ui_to_ui_.GetWeakPtr(),
                   base::Passed(&data)));
  }
}

void PushMessagingManager::Core::DidRequestPermissionInIncognito(
    RegisterData data,
    blink::mojom::PermissionStatus status) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  // Notification permission should always be denied in incognito.
  DCHECK_EQ(blink::mojom::PermissionStatus::DENIED, status);
  base::PostTaskWithTraits(
      FROM_HERE, {BrowserThread::IO},
      base::BindOnce(
          &PushMessagingManager::SendSubscriptionError, io_parent_,
          std::move(data),
          blink::mojom::PushRegistrationStatus::INCOGNITO_PERMISSION_DENIED));
}

void PushMessagingManager::Core::DidRegister(
    RegisterData data,
    const std::string& push_subscription_id,
    const std::vector<uint8_t>& p256dh,
    const std::vector<uint8_t>& auth,
    blink::mojom::PushRegistrationStatus status) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  // TODO(crbug.com/646721): Handle the case where |push_subscription_id| and
  // |data.existing_subscription_id| are not the same. Right now we just
  // override the old subscription ID and encryption information.
  const bool subscription_changed =
      data.existing_subscription_id.has_value() &&
      data.existing_subscription_id.value() != push_subscription_id;

  if (status ==
      blink::mojom::PushRegistrationStatus::SUCCESS_FROM_PUSH_SERVICE) {
    base::PostTaskWithTraits(
        FROM_HERE, {BrowserThread::IO},
        base::BindOnce(&PushMessagingManager::PersistRegistrationOnIO,
                       io_parent_, std::move(data), push_subscription_id,
                       p256dh, auth,
                       subscription_changed
                           ? blink::mojom::PushRegistrationStatus::
                                 SUCCESS_NEW_SUBSCRIPTION_FROM_PUSH_SERVICE
                           : blink::mojom::PushRegistrationStatus::
                                 SUCCESS_FROM_PUSH_SERVICE));
  } else {
    base::PostTaskWithTraits(
        FROM_HERE, {BrowserThread::IO},
        base::BindOnce(&PushMessagingManager::SendSubscriptionError, io_parent_,
                       std::move(data), status));
  }
}

void PushMessagingManager::PersistRegistrationOnIO(
    RegisterData data,
    const std::string& push_subscription_id,
    const std::vector<uint8_t>& p256dh,
    const std::vector<uint8_t>& auth,
    blink::mojom::PushRegistrationStatus status) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  GURL requesting_origin = data.requesting_origin;
  int64_t registration_id = data.service_worker_registration_id;
  std::string application_server_key(
      std::string(data.options->application_server_key.begin(),
                  data.options->application_server_key.end()));

  service_worker_context_->StoreRegistrationUserData(
      registration_id, requesting_origin,
      {{kPushRegistrationIdServiceWorkerKey, push_subscription_id},
       {kPushSenderIdServiceWorkerKey, application_server_key}},
      base::BindOnce(&PushMessagingManager::DidPersistRegistrationOnIO,
                     weak_factory_.GetWeakPtr(), std::move(data),
                     push_subscription_id, p256dh, auth, status));
}

void PushMessagingManager::DidPersistRegistrationOnIO(
    RegisterData data,
    const std::string& push_subscription_id,
    const std::vector<uint8_t>& p256dh,
    const std::vector<uint8_t>& auth,
    blink::mojom::PushRegistrationStatus push_registration_status,
    blink::ServiceWorkerStatusCode service_worker_status) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  if (service_worker_status == blink::ServiceWorkerStatusCode::kOk) {
    SendSubscriptionSuccess(std::move(data), push_registration_status,
                            push_subscription_id, p256dh, auth);
  } else {
    // TODO(johnme): Unregister, so PushMessagingServiceImpl can decrease count.
    SendSubscriptionError(std::move(data),
                          blink::mojom::PushRegistrationStatus::STORAGE_ERROR);
  }
}

void PushMessagingManager::SendSubscriptionError(
    RegisterData data,
    blink::mojom::PushRegistrationStatus status) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  std::move(data.callback)
      .Run(status, base::nullopt /* endpoint */, nullptr /* options */,
           base::nullopt /* p256dh */, base::nullopt /* auth */);
  RecordRegistrationStatus(status);
}

void PushMessagingManager::SendSubscriptionSuccess(
    RegisterData data,
    blink::mojom::PushRegistrationStatus status,
    const std::string& push_subscription_id,
    const std::vector<uint8_t>& p256dh,
    const std::vector<uint8_t>& auth) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  if (!service_available_) {
    // This shouldn't be possible in incognito mode, since we've already checked
    // that we have an existing registration. Hence it's ok to throw an error.
    DCHECK(!ui_core_->is_incognito());
    SendSubscriptionError(
        std::move(data),
        blink::mojom::PushRegistrationStatus::SERVICE_NOT_AVAILABLE);
    return;
  }

  const GURL endpoint = CreateEndpoint(
      IsApplicationServerKey(data.options->application_server_key),
      push_subscription_id);

  std::move(data.callback)
      .Run(status, endpoint, std::move(data.options), p256dh, auth);

  RecordRegistrationStatus(status);
}

// Unsubscribe methods on both IO and UI threads, merged in order of use from
// PushMessagingManager and Core.
// -----------------------------------------------------------------------------

void PushMessagingManager::Unsubscribe(int64_t service_worker_registration_id,
                                       UnsubscribeCallback callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  ServiceWorkerRegistration* service_worker_registration =
      service_worker_context_->GetLiveRegistration(
          service_worker_registration_id);
  if (!service_worker_registration) {
    DidUnregister(std::move(callback),
                  blink::mojom::PushUnregistrationStatus::NO_SERVICE_WORKER);
    return;
  }

  service_worker_context_->GetRegistrationUserData(
      service_worker_registration_id, {kPushSenderIdServiceWorkerKey},
      base::BindOnce(&PushMessagingManager::UnsubscribeHavingGottenSenderId,
                     weak_factory_.GetWeakPtr(), std::move(callback),
                     service_worker_registration_id,
                     service_worker_registration->scope().GetOrigin()));
}

void PushMessagingManager::UnsubscribeHavingGottenSenderId(
    UnsubscribeCallback callback,
    int64_t service_worker_registration_id,
    const GURL& requesting_origin,
    const std::vector<std::string>& sender_ids,
    blink::ServiceWorkerStatusCode service_worker_status) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  std::string sender_id;
  if (service_worker_status == blink::ServiceWorkerStatusCode::kOk) {
    DCHECK_EQ(1u, sender_ids.size());
    sender_id = sender_ids[0];
  }
  base::PostTaskWithTraits(
      FROM_HERE, {BrowserThread::UI},
      base::BindOnce(&Core::UnregisterFromService,
                     base::Unretained(ui_core_.get()), std::move(callback),
                     service_worker_registration_id, requesting_origin,
                     sender_id));
}

void PushMessagingManager::Core::UnregisterFromService(
    UnsubscribeCallback callback,
    int64_t service_worker_registration_id,
    const GURL& requesting_origin,
    const std::string& sender_id) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  PushMessagingService* push_service = service();
  if (!push_service) {
    // This shouldn't be possible in incognito mode, since we've already checked
    // that we have an existing registration. Hence it's ok to throw an error.
    DCHECK(!is_incognito());
    base::PostTaskWithTraits(
        FROM_HERE, {BrowserThread::IO},
        base::BindOnce(
            &PushMessagingManager::DidUnregister, io_parent_,
            std::move(callback),
            blink::mojom::PushUnregistrationStatus::SERVICE_NOT_AVAILABLE));
    return;
  }

  push_service->Unsubscribe(
      blink::mojom::PushUnregistrationReason::JAVASCRIPT_API, requesting_origin,
      service_worker_registration_id, sender_id,
      base::Bind(&Core::DidUnregisterFromService,
                 weak_factory_ui_to_ui_.GetWeakPtr(), base::Passed(&callback),
                 service_worker_registration_id));
}

void PushMessagingManager::Core::DidUnregisterFromService(
    UnsubscribeCallback callback,
    int64_t service_worker_registration_id,
    blink::mojom::PushUnregistrationStatus unregistration_status) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  base::PostTaskWithTraits(
      FROM_HERE, {BrowserThread::IO},
      base::BindOnce(&PushMessagingManager::DidUnregister, io_parent_,
                     std::move(callback), unregistration_status));
}

void PushMessagingManager::DidUnregister(
    UnsubscribeCallback callback,
    blink::mojom::PushUnregistrationStatus unregistration_status) {
  // Only called from IO thread, but would be safe to call from UI thread.
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  switch (unregistration_status) {
    case blink::mojom::PushUnregistrationStatus::SUCCESS_UNREGISTERED:
    case blink::mojom::PushUnregistrationStatus::PENDING_NETWORK_ERROR:
    case blink::mojom::PushUnregistrationStatus::PENDING_SERVICE_ERROR:
      std::move(callback).Run(blink::mojom::PushErrorType::NONE,
                              true /* did_unsubscribe */,
                              base::nullopt /* error_message */);
      break;
    case blink::mojom::PushUnregistrationStatus::SUCCESS_WAS_NOT_REGISTERED:
      std::move(callback).Run(blink::mojom::PushErrorType::NONE,
                              false /* did_unsubscribe */,
                              base::nullopt /* error_message */);
      break;
    case blink::mojom::PushUnregistrationStatus::NO_SERVICE_WORKER:
    case blink::mojom::PushUnregistrationStatus::SERVICE_NOT_AVAILABLE:
    case blink::mojom::PushUnregistrationStatus::STORAGE_ERROR:
      std::move(callback).Run(blink::mojom::PushErrorType::ABORT, false,
                              std::string(PushUnregistrationStatusToString(
                                  unregistration_status)) /* error_message */);
      break;
    case blink::mojom::PushUnregistrationStatus::NETWORK_ERROR:
      NOTREACHED();
      break;
  }
  RecordUnregistrationStatus(unregistration_status);
}

// GetSubscription methods on both IO and UI threads, merged in order of use
// from PushMessagingManager and Core.
// -----------------------------------------------------------------------------

void PushMessagingManager::GetSubscription(
    int64_t service_worker_registration_id,
    GetSubscriptionCallback callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  // TODO(johnme): Validate arguments?
  service_worker_context_->GetRegistrationUserData(
      service_worker_registration_id,
      {kPushRegistrationIdServiceWorkerKey, kPushSenderIdServiceWorkerKey},
      base::BindOnce(&PushMessagingManager::DidGetSubscription,
                     weak_factory_.GetWeakPtr(), std::move(callback),
                     service_worker_registration_id));
}

void PushMessagingManager::DidGetSubscription(
    GetSubscriptionCallback callback,
    int64_t service_worker_registration_id,
    const std::vector<std::string>&
        push_subscription_id_and_application_server_key,
    blink::ServiceWorkerStatusCode service_worker_status) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  blink::mojom::PushGetRegistrationStatus get_status =
      blink::mojom::PushGetRegistrationStatus::STORAGE_ERROR;
  switch (service_worker_status) {
    case blink::ServiceWorkerStatusCode::kOk: {
      DCHECK_EQ(2u, push_subscription_id_and_application_server_key.size());
      const std::string& push_subscription_id =
          push_subscription_id_and_application_server_key[0];
      const std::string& application_server_key =
          push_subscription_id_and_application_server_key[1];

      if (!service_available_) {
        // Return not found in incognito mode, so websites can't detect it.
        get_status = ui_core_->is_incognito()
                         ? blink::mojom::PushGetRegistrationStatus::
                               INCOGNITO_REGISTRATION_NOT_FOUND
                         : blink::mojom::PushGetRegistrationStatus::
                               SERVICE_NOT_AVAILABLE;
        break;
      }

      ServiceWorkerRegistration* registration =
          service_worker_context_->GetLiveRegistration(
              service_worker_registration_id);
      if (!registration) {
        get_status =
            blink::mojom::PushGetRegistrationStatus::NO_LIVE_SERVICE_WORKER;
        break;
      }

      const GURL origin = registration->scope().GetOrigin();

      const bool uses_standard_protocol =
          IsApplicationServerKey(std::vector<uint8_t>(
              application_server_key.begin(), application_server_key.end()));
      const GURL endpoint =
          CreateEndpoint(uses_standard_protocol, push_subscription_id);

      base::PostTaskWithTraits(
          FROM_HERE, {BrowserThread::UI},
          base::BindOnce(&Core::GetSubscriptionInfoOnUI,
                         base::Unretained(ui_core_.get()), origin,
                         service_worker_registration_id, application_server_key,
                         push_subscription_id,
                         base::Bind(&Core::GetSubscriptionDidGetInfoOnUI,
                                    ui_core_weak_ptr_, base::Passed(&callback),
                                    origin, service_worker_registration_id,
                                    endpoint, application_server_key)));

      return;
    }
    case blink::ServiceWorkerStatusCode::kErrorNotFound: {
      get_status =
          blink::mojom::PushGetRegistrationStatus::REGISTRATION_NOT_FOUND;
      break;
    }
    case blink::ServiceWorkerStatusCode::kErrorFailed: {
      get_status = blink::mojom::PushGetRegistrationStatus::STORAGE_ERROR;
      break;
    }
    case blink::ServiceWorkerStatusCode::kErrorAbort:
    case blink::ServiceWorkerStatusCode::kErrorStartWorkerFailed:
    case blink::ServiceWorkerStatusCode::kErrorProcessNotFound:
    case blink::ServiceWorkerStatusCode::kErrorExists:
    case blink::ServiceWorkerStatusCode::kErrorInstallWorkerFailed:
    case blink::ServiceWorkerStatusCode::kErrorActivateWorkerFailed:
    case blink::ServiceWorkerStatusCode::kErrorIpcFailed:
    case blink::ServiceWorkerStatusCode::kErrorNetwork:
    case blink::ServiceWorkerStatusCode::kErrorSecurity:
    case blink::ServiceWorkerStatusCode::kErrorEventWaitUntilRejected:
    case blink::ServiceWorkerStatusCode::kErrorState:
    case blink::ServiceWorkerStatusCode::kErrorTimeout:
    case blink::ServiceWorkerStatusCode::kErrorScriptEvaluateFailed:
    case blink::ServiceWorkerStatusCode::kErrorDiskCache:
    case blink::ServiceWorkerStatusCode::kErrorRedundant:
    case blink::ServiceWorkerStatusCode::kErrorDisallowed:
    case blink::ServiceWorkerStatusCode::kErrorInvalidArguments: {
      NOTREACHED() << "Got unexpected error code: "
                   << static_cast<uint32_t>(service_worker_status) << " "
                   << blink::ServiceWorkerStatusToString(service_worker_status);
      get_status = blink::mojom::PushGetRegistrationStatus::STORAGE_ERROR;
      break;
    }
  }
  std::move(callback).Run(get_status, base::nullopt /* endpoint */,
                          nullptr /* options */, base::nullopt /* p256dh */,
                          base::nullopt /* auth */);
  RecordGetRegistrationStatus(get_status);
}

void PushMessagingManager::Core::GetSubscriptionDidGetInfoOnUI(
    GetSubscriptionCallback callback,
    const GURL& origin,
    int64_t service_worker_registration_id,
    const GURL& endpoint,
    const std::string& application_server_key,
    bool is_valid,
    const std::vector<uint8_t>& p256dh,
    const std::vector<uint8_t>& auth) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  if (is_valid) {
    auto options = blink::mojom::PushSubscriptionOptions::New();

    // Chrome rejects subscription requests with userVisibleOnly false, so it
    // must have been true. TODO(harkness): If Chrome starts accepting silent
    // push subscriptions with userVisibleOnly false, the bool will need to be
    // stored.
    options->user_visible_only = true;
    options->application_server_key = std::vector<uint8_t>(
        application_server_key.begin(), application_server_key.end());

    blink::mojom::PushGetRegistrationStatus status =
        blink::mojom::PushGetRegistrationStatus::SUCCESS;

    base::PostTaskWithTraits(
        FROM_HERE, {BrowserThread::IO},
        base::BindOnce(std::move(callback), status, endpoint,
                       std::move(options), p256dh, auth));

    RecordGetRegistrationStatus(status);
  } else {
    PushMessagingService* push_service = service();
    if (!push_service) {
      // Shouldn't be possible to have a stored push subscription in a profile
      // with no push service, but this case can occur when the renderer is
      // shutting down.
      base::PostTaskWithTraits(
          FROM_HERE, {BrowserThread::IO},
          base::BindOnce(
              std::move(callback),
              blink::mojom::PushGetRegistrationStatus::RENDERER_SHUTDOWN,
              base::nullopt /* endpoint */, nullptr /* options */,
              base::nullopt /* p256dh */, base::nullopt /* auth */));
      return;
    }

    // Uh-oh! Although there was a cached subscription in the Service Worker
    // database, it did not have matching counterparts in the
    // PushMessagingAppIdentifier map and/or GCM Store. Unsubscribe to fix this
    // inconsistency.
    blink::mojom::PushGetRegistrationStatus status =
        blink::mojom::PushGetRegistrationStatus::STORAGE_CORRUPT;

    push_service->Unsubscribe(blink::mojom::PushUnregistrationReason::
                                  GET_SUBSCRIPTION_STORAGE_CORRUPT,
                              origin, service_worker_registration_id,
                              application_server_key,
                              base::Bind(&Core::GetSubscriptionDidUnsubscribe,
                                         weak_factory_ui_to_ui_.GetWeakPtr(),
                                         base::Passed(&callback), status));

    RecordGetRegistrationStatus(status);
  }
}

void PushMessagingManager::Core::GetSubscriptionDidUnsubscribe(
    GetSubscriptionCallback callback,
    blink::mojom::PushGetRegistrationStatus get_status,
    blink::mojom::PushUnregistrationStatus unsubscribe_status) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  base::PostTaskWithTraits(
      FROM_HERE, {BrowserThread::IO},
      base::BindOnce(std::move(callback), get_status,
                     base::nullopt /* endpoint */, nullptr /* options */,
                     base::nullopt /* p256dh */, base::nullopt /* auth */));
}

// Helper methods on both IO and UI threads, merged from
// PushMessagingManager and Core.
// -----------------------------------------------------------------------------

void PushMessagingManager::Core::GetSubscriptionInfoOnUI(
    const GURL& origin,
    int64_t service_worker_registration_id,
    const std::string& sender_id,
    const std::string& push_subscription_id,
    PushMessagingService::SubscriptionInfoCallback callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  PushMessagingService* push_service = service();
  if (!push_service) {
    std::move(callback).Run(false /* is_valid */,
                            std::vector<uint8_t>() /* p256dh */,
                            std::vector<uint8_t>() /* auth */);
    return;
  }

  push_service->GetSubscriptionInfo(origin, service_worker_registration_id,
                                    sender_id, push_subscription_id,
                                    std::move(callback));
}

GURL PushMessagingManager::CreateEndpoint(
    bool standard_protocol,
    const std::string& subscription_id) const {
  const GURL& base =
      standard_protocol ? web_push_protocol_endpoint_ : default_endpoint_;

  return GURL(base.spec() + subscription_id);
}

PushMessagingService* PushMessagingManager::Core::service() {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  RenderProcessHost* process_host =
      RenderProcessHost::FromID(render_process_id_);
  return process_host
             ? process_host->GetBrowserContext()->GetPushMessagingService()
             : nullptr;
}

base::WeakPtr<PushMessagingManager::Core>
PushMessagingManager::Core::GetWeakPtrFromIOParentConstructor() {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  return weak_factory_ui_to_ui_.GetWeakPtr();
}

}  // namespace content
