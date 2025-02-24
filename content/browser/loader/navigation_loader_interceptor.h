// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_LOADER_NAVIGATION_LOADER_INTERCEPTOR_H_
#define CONTENT_BROWSER_LOADER_NAVIGATION_LOADER_INTERCEPTOR_H_

#include <memory>

#include "base/callback_forward.h"
#include "base/macros.h"
#include "base/optional.h"
#include "content/browser/loader/single_request_url_loader_factory.h"
#include "content/common/content_export.h"
#include "net/url_request/redirect_info.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"

namespace content {

class ResourceContext;
struct ResourceRequest;
struct SubresourceLoaderParams;
class ThrottlingURLLoader;

// NavigationLoaderInterceptor is given a chance to create a URLLoader and
// intercept a navigation request before the request is handed off to the
// default URLLoader, e.g. the one from the network service.
// NavigationLoaderInterceptor is a per-request object and kept around during
// the lifetime of a navigation request (including multiple redirect legs).
class CONTENT_EXPORT NavigationLoaderInterceptor {
 public:
  NavigationLoaderInterceptor() = default;
  virtual ~NavigationLoaderInterceptor() = default;

  using LoaderCallback =
      base::OnceCallback<void(SingleRequestURLLoaderFactory::RequestHandler)>;
  using FallbackCallback =
      base::OnceCallback<void(bool /* reset_subresource_loader_params */)>;

  // Asks this handler to handle this resource load request.
  // The handler must invoke |callback| eventually with either a non-null
  // RequestHandler indicating its willingness to handle the request, or a null
  // RequestHandler to indicate that someone else should handle the request.
  //
  // The |tentative_resource_request| passed to this function and the resource
  // request later passed to the RequestHandler given to |callback| may not be
  // exactly the same, because URLLoaderThrottles may rewrite the request
  // between the two calls. However the URL must remain constant between the
  // two, as any modifications on the URL done by URLLoaderThrottles must result
  // in an (internal) redirect, which must restart the request with a new
  // MaybeCreateLoader().
  //
  // This handler might initially elect to handle the request, but later decide
  // to fall back to the default behavior. In that case, it can invoke
  // |fallback_callback| to do so. An example of this is when a service worker
  // decides to handle the request because it is in-scope, but the service
  // worker JavaScript execution does not result in a response provided, so
  // fallback to network is required.
  //
  // If |fallback_callback| is called, it must be called prior to the
  // RequestHandler making any URLLoaderClient calls. The
  // |reset_subresource_loader_params| parameter to |fallback_callback|
  // indicates whether to discard the subresource loader params previously
  // returned by MaybeCreateSubresourceLoaderParams().
  virtual void MaybeCreateLoader(
      const network::ResourceRequest& tentative_resource_request,
      ResourceContext* resource_context,
      LoaderCallback callback,
      FallbackCallback fallback_callback) = 0;

  // Returns a SubresourceLoaderParams if any to be used for subsequent URL
  // requests going forward. Subclasses who want to set-up custom loader for
  // subresource requests may want to override this.
  //
  // Note that the handler can return a null callback to MaybeCreateLoader(),
  // and at the same time can return non-null SubresourceLoaderParams here if it
  // does NOT want to handle the specific request given to MaybeCreateLoader()
  // but wants to handle the subsequent resource requests or ensure other
  // interceptors are skipped.
  virtual base::Optional<SubresourceLoaderParams>
  MaybeCreateSubresourceLoaderParams();

  // Returns true if the handler creates a loader for the |response| passed.
  // |request| is the latest request whose request URL may include URL fragment.
  // An example of where this is used is AppCache, where the handler returns
  // fallback content for the response passed in.
  // The URLLoader interface pointer is returned in the |loader| parameter.
  // The interface request for the URLLoaderClient is returned in the
  // |client_request| parameter.
  // The |url_loader| points to the ThrottlingURLLoader that currently controls
  // the request. It can be optionally consumed to get the current
  // URLLoaderClient and URLLoader so that the implementation can rebind them to
  // intercept the inflight loading if necessary.  Note that the |url_loader|
  // will be reset after this method is called, which will also drop the
  // URLLoader held by |url_loader_| if it is not unbound yet.
  // |skip_other_interceptors| is set to true when this interceptor will
  // exclusively handle the navigation even after redirections. TODO(horo): This
  // flag was introduced to skip service worker after signed exchange redirect.
  // Remove this flag when we support service worker and signed exchange
  // integration. See crbug.com/894755#c1. Nullptr is not allowed.
  virtual bool MaybeCreateLoaderForResponse(
      const network::ResourceRequest& request,
      const network::ResourceResponseHead& response,
      network::mojom::URLLoaderPtr* loader,
      network::mojom::URLLoaderClientRequest* client_request,
      ThrottlingURLLoader* url_loader,
      bool* skip_other_interceptors);
};

}  // namespace content

#endif  // CONTENT_BROWSER_LOADER_NAVIGATION_LOADER_INTERCEPTOR_H_
