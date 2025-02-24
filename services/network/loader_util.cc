// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/network/loader_util.h"

#include <string>

#include "base/command_line.h"
#include "base/logging.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "net/base/load_flags.h"
#include "net/base/mime_sniffer.h"
#include "net/http/http_raw_request_headers.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_util.h"
#include "net/url_request/url_request.h"
#include "services/network/public/cpp/http_raw_request_response_info.h"
#include "services/network/public/cpp/network_switches.h"
#include "services/network/public/cpp/resource_response.h"
#include "url/gurl.h"

namespace network {

const char kAcceptHeader[] = "Accept";
const char kFrameAcceptHeader[] =
    "text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,"
    "image/apng,*/*;q=0.8";
const char kDefaultAcceptHeader[] = "*/*";

// Headers that consumers are not trusted to set. All "Proxy-" prefixed messages
// are blocked inline. The"Authorization" auth header is deliberately not
// included, since OAuth requires websites be able to set it directly.
const char* kUnsafeHeaders[] = {
    // This is determined by the upload body and set by net/. A consumer
    // overriding that could allow for Bad Things.
    net::HttpRequestHeaders::kContentLength,

    // Disallow setting the Host header because it can conflict with specified
    // URL and logic related to isolating sites.
    net::HttpRequestHeaders::kHost,

    // Trailers are not supported.
    "Trailer",

    // Websockets use a different API.
    "Upgrade",

    // TODO(mmenke): Gather stats on the following headers before adding them:
    // Cookie, Cookie2, Referer, TE, Keep-Alive, Via.
};

// Headers that consumers are currently allowed to set, with the exception of
// certain values could cause problems.
// TODO(mmenke): Gather stats on these, and see if these headers can be banned
// outright instead.
const struct {
  const char* name;
  const char* value;
} kUnsafeHeaderValues[] = {
    // Websockets use a different API.
    {net::HttpRequestHeaders::kConnection, "Upgrade"},

    // Telling a server a non-chunked upload is chunked has similar implications
    // as sending the wrong Content-Length.
    {net::HttpRequestHeaders::kTransferEncoding, "Chunked"},
};

bool ShouldSniffContent(net::URLRequest* url_request,
                        ResourceResponse* response) {
  const std::string& mime_type = response->head.mime_type;

  std::string content_type_options;
  url_request->GetResponseHeaderByName("x-content-type-options",
                                       &content_type_options);

  bool sniffing_blocked =
      base::LowerCaseEqualsASCII(content_type_options, "nosniff");
  bool we_would_like_to_sniff =
      net::ShouldSniffMimeType(url_request->url(), mime_type);

  if (!sniffing_blocked && we_would_like_to_sniff) {
    // We're going to look at the data before deciding what the content type
    // is.  That means we need to delay sending the response started IPC.
    VLOG(1) << "To buffer: " << url_request->url().spec();
    return true;
  }

  return false;
}

scoped_refptr<HttpRawRequestResponseInfo> BuildRawRequestResponseInfo(
    const net::URLRequest& request,
    const net::HttpRawRequestHeaders& raw_request_headers,
    const net::HttpResponseHeaders* raw_response_headers) {
  scoped_refptr<HttpRawRequestResponseInfo> info =
      new HttpRawRequestResponseInfo();

  const net::HttpResponseInfo& response_info = request.response_info();
  // Unparsed headers only make sense if they were sent as text, i.e. HTTP 1.x.
  bool report_headers_text =
      !response_info.DidUseQuic() && !response_info.was_fetched_via_spdy;

  for (const auto& pair : raw_request_headers.headers())
    info->request_headers.push_back(pair);
  std::string request_line = raw_request_headers.request_line();
  if (report_headers_text && !request_line.empty()) {
    std::string text = std::move(request_line);
    for (const auto& pair : raw_request_headers.headers()) {
      if (!pair.second.empty()) {
        base::StringAppendF(&text, "%s: %s\r\n", pair.first.c_str(),
                            pair.second.c_str());
      } else {
        base::StringAppendF(&text, "%s:\r\n", pair.first.c_str());
      }
    }
    info->request_headers_text = std::move(text);
  }

  if (!raw_response_headers)
    raw_response_headers = request.response_headers();
  if (raw_response_headers) {
    info->http_status_code = raw_response_headers->response_code();
    info->http_status_text = raw_response_headers->GetStatusText();

    std::string name;
    std::string value;
    for (size_t it = 0;
         raw_response_headers->EnumerateHeaderLines(&it, &name, &value);) {
      info->response_headers.push_back(std::make_pair(name, value));
    }
    if (report_headers_text) {
      info->response_headers_text =
          net::HttpUtil::ConvertHeadersBackToHTTPResponse(
              raw_response_headers->raw_headers());
    }
  }
  return info;
}

std::string ComputeReferrer(const GURL& referrer) {
  if (!referrer.is_valid() || base::CommandLine::ForCurrentProcess()->HasSwitch(
                                  switches::kNoReferrers)) {
    return std::string();
  }

  return referrer.spec();
}

bool AreRequestHeadersSafe(const net::HttpRequestHeaders& request_headers) {
  net::HttpRequestHeaders::Iterator it(request_headers);

  while (it.GetNext()) {
    for (const auto* header : kUnsafeHeaders) {
      if (base::EqualsCaseInsensitiveASCII(header, it.name()))
        return false;
    }

    for (const auto& header : kUnsafeHeaderValues) {
      if (base::EqualsCaseInsensitiveASCII(header.name, it.name()) &&
          base::EqualsCaseInsensitiveASCII(header.value, it.value())) {
        return false;
      }
    }

    // Proxy headers are destined for the proxy, so shouldn't be set by callers.
    if (base::StartsWith(it.name(), "Proxy-",
                         base::CompareCase::INSENSITIVE_ASCII)) {
      return false;
    }
  }
  return true;
}

}  // namespace network
