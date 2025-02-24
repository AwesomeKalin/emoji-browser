// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/data_reduction_proxy/core/browser/data_reduction_proxy_util.h"

#include <stdint.h>

#include "base/stl_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/time/time.h"
#include "base/version.h"
#include "components/data_reduction_proxy/core/browser/data_reduction_proxy_data.h"
#include "components/data_reduction_proxy/core/common/data_reduction_proxy_headers.h"
#include "components/data_reduction_proxy/core/common/version.h"
#include "net/base/net_errors.h"
#include "net/base/url_util.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_status_code.h"
#include "net/http/http_util.h"
#include "net/proxy_resolution/proxy_config.h"
#include "net/proxy_resolution/proxy_info.h"

#if defined(USE_GOOGLE_API_KEYS)
#include "google_apis/google_api_keys.h"
#endif

namespace data_reduction_proxy {

namespace {

#if defined(USE_GOOGLE_API_KEYS)
// Used in all Data Reduction Proxy URLs to specify API Key.
const char kApiKeyName[] = "key";
#endif

// Hostname used for the other bucket which consists of chrome-services traffic.
// This should be in sync with the same in DataReductionSiteBreakdownView.java
const char kOtherHostName[] = "Other";

}  // namespace

namespace util {

const char* ChromiumVersion() {
  // Assert at compile time that the Chromium version is at least somewhat
  // properly formed, e.g. the version string is at least as long as "0.0.0.0",
  // and starts and ends with numeric digits. This is to prevent another
  // regression like http://crbug.com/595471.
  static_assert(base::size(PRODUCT_VERSION) >= base::size("0.0.0.0") &&
                    '0' <= PRODUCT_VERSION[0] && PRODUCT_VERSION[0] <= '9' &&
                    '0' <= PRODUCT_VERSION[base::size(PRODUCT_VERSION) - 2] &&
                    PRODUCT_VERSION[base::size(PRODUCT_VERSION) - 2] <= '9',
                "PRODUCT_VERSION must be a string of the form "
                "'MAJOR.MINOR.BUILD.PATCH', e.g. '1.2.3.4'. "
                "PRODUCT_VERSION='" PRODUCT_VERSION "' is badly formed.");

  return PRODUCT_VERSION;
}

void GetChromiumBuildAndPatch(const std::string& version_string,
                              std::string* build,
                              std::string* patch) {
  uint32_t build_number;
  uint32_t patch_number;
  GetChromiumBuildAndPatchAsInts(version_string, &build_number, &patch_number);
  *build = base::NumberToString(build_number);
  *patch = base::NumberToString(patch_number);
}

void GetChromiumBuildAndPatchAsInts(const std::string& version_string,
                                    uint32_t* build,
                                    uint32_t* patch) {
  base::Version version(version_string);
  DCHECK(version.IsValid());
  DCHECK_EQ(4U, version.components().size());
  *build = version.components()[2];
  *patch = version.components()[3];
}

const char* GetStringForClient(Client client) {
  switch (client) {
    case Client::UNKNOWN:
      return "";
    case Client::CRONET_ANDROID:
      return "cronet";
    case Client::WEBVIEW_ANDROID:
      return "webview";
    case Client::CHROME_ANDROID:
      return "android";
    case Client::CHROME_IOS:
      return "ios";
    case Client::CHROME_MAC:
      return "mac";
    case Client::CHROME_CHROMEOS:
      return "chromeos";
    case Client::CHROME_LINUX:
      return "linux";
    case Client::CHROME_WINDOWS:
      return "win";
    case Client::CHROME_FREEBSD:
      return "freebsd";
    case Client::CHROME_OPENBSD:
      return "openbsd";
    case Client::CHROME_SOLARIS:
      return "solaris";
    case Client::CHROME_QNX:
      return "qnx";
    default:
      NOTREACHED();
      return "";
  }
}

GURL AddApiKeyToUrl(const GURL& url) {
  GURL new_url = url;
#if defined(USE_GOOGLE_API_KEYS)
  std::string api_key = google_apis::GetAPIKey();
  if (google_apis::HasAPIKeyConfigured() && !api_key.empty()) {
    new_url = net::AppendOrReplaceQueryParameter(url, kApiKeyName, api_key);
  }
#endif
  return net::AppendOrReplaceQueryParameter(new_url, "alt", "proto");
}

bool EligibleForDataReductionProxy(const net::ProxyInfo& proxy_info,
                                   const GURL& url,
                                   const std::string& method) {
  return proxy_info.is_direct() && proxy_info.proxy_list().size() == 1 &&
         !url.SchemeIsWSOrWSS() && net::HttpUtil::IsMethodIdempotent(method);
}

bool ApplyProxyConfigToProxyInfo(const net::ProxyConfig& proxy_config,
                                 const net::ProxyRetryInfoMap& proxy_retry_info,
                                 const GURL& url,
                                 net::ProxyInfo* data_reduction_proxy_info) {
  DCHECK(data_reduction_proxy_info);
  if (proxy_config.proxy_rules().empty())
    return false;
  proxy_config.proxy_rules().Apply(url, data_reduction_proxy_info);
  data_reduction_proxy_info->DeprioritizeBadProxies(proxy_retry_info);
  return !data_reduction_proxy_info->is_empty() &&
         !data_reduction_proxy_info->proxy_server().is_direct();
}

const char* GetSiteBreakdownOtherHostName() {
  return kOtherHostName;
}

}  // namespace util

namespace protobuf_parser {

PageloadMetrics_EffectiveConnectionType
ProtoEffectiveConnectionTypeFromEffectiveConnectionType(
    net::EffectiveConnectionType effective_connection_type) {
  switch (effective_connection_type) {
    case net::EFFECTIVE_CONNECTION_TYPE_UNKNOWN:
      return PageloadMetrics_EffectiveConnectionType_EFFECTIVE_CONNECTION_TYPE_UNKNOWN;
    case net::EFFECTIVE_CONNECTION_TYPE_OFFLINE:
      return PageloadMetrics_EffectiveConnectionType_EFFECTIVE_CONNECTION_TYPE_OFFLINE;
    case net::EFFECTIVE_CONNECTION_TYPE_SLOW_2G:
      return PageloadMetrics_EffectiveConnectionType_EFFECTIVE_CONNECTION_TYPE_SLOW_2G;
    case net::EFFECTIVE_CONNECTION_TYPE_2G:
      return PageloadMetrics_EffectiveConnectionType_EFFECTIVE_CONNECTION_TYPE_2G;
    case net::EFFECTIVE_CONNECTION_TYPE_3G:
      return PageloadMetrics_EffectiveConnectionType_EFFECTIVE_CONNECTION_TYPE_3G;
    case net::EFFECTIVE_CONNECTION_TYPE_4G:
      return PageloadMetrics_EffectiveConnectionType_EFFECTIVE_CONNECTION_TYPE_4G;
    default:
      NOTREACHED();
      return PageloadMetrics_EffectiveConnectionType_EFFECTIVE_CONNECTION_TYPE_UNKNOWN;
  }
}

PageloadMetrics_ConnectionType ProtoConnectionTypeFromConnectionType(
    net::NetworkChangeNotifier::ConnectionType connection_type) {
  switch (connection_type) {
    case net::NetworkChangeNotifier::CONNECTION_UNKNOWN:
      return PageloadMetrics_ConnectionType_CONNECTION_UNKNOWN;
    case net::NetworkChangeNotifier::CONNECTION_ETHERNET:
      return PageloadMetrics_ConnectionType_CONNECTION_ETHERNET;
    case net::NetworkChangeNotifier::CONNECTION_WIFI:
      return PageloadMetrics_ConnectionType_CONNECTION_WIFI;
    case net::NetworkChangeNotifier::CONNECTION_2G:
      return PageloadMetrics_ConnectionType_CONNECTION_2G;
    case net::NetworkChangeNotifier::CONNECTION_3G:
      return PageloadMetrics_ConnectionType_CONNECTION_3G;
    case net::NetworkChangeNotifier::CONNECTION_4G:
      return PageloadMetrics_ConnectionType_CONNECTION_4G;
    case net::NetworkChangeNotifier::CONNECTION_NONE:
      return PageloadMetrics_ConnectionType_CONNECTION_NONE;
    case net::NetworkChangeNotifier::CONNECTION_BLUETOOTH:
      return PageloadMetrics_ConnectionType_CONNECTION_BLUETOOTH;
  }
}

net::ProxyServer::Scheme SchemeFromProxyScheme(
    ProxyServer_ProxyScheme proxy_scheme) {
  switch (proxy_scheme) {
    case ProxyServer_ProxyScheme_HTTP:
      return net::ProxyServer::SCHEME_HTTP;
    case ProxyServer_ProxyScheme_HTTPS:
      return net::ProxyServer::SCHEME_HTTPS;
    default:
      return net::ProxyServer::SCHEME_INVALID;
  }
}

ProxyServer_ProxyScheme ProxySchemeFromScheme(net::ProxyServer::Scheme scheme) {
  switch (scheme) {
    case net::ProxyServer::SCHEME_HTTP:
      return ProxyServer_ProxyScheme_HTTP;
    case net::ProxyServer::SCHEME_HTTPS:
      return ProxyServer_ProxyScheme_HTTPS;
    default:
      return ProxyServer_ProxyScheme_UNSPECIFIED;
  }
}


void TimeDeltaToDuration(const base::TimeDelta& time_delta,
                         Duration* duration) {
  duration->set_seconds(time_delta.InSeconds());
  base::TimeDelta partial_seconds =
      time_delta - base::TimeDelta::FromSeconds(time_delta.InSeconds());
  duration->set_nanos(partial_seconds.InMicroseconds() *
                      base::Time::kNanosecondsPerMicrosecond);
}

base::TimeDelta DurationToTimeDelta(const Duration& duration) {
  return base::TimeDelta::FromSeconds(duration.seconds()) +
         base::TimeDelta::FromMicroseconds(
             duration.nanos() / base::Time::kNanosecondsPerMicrosecond);
}

void TimeToTimestamp(const base::Time& time, Timestamp* timestamp) {
  timestamp->set_seconds((time - base::Time::UnixEpoch()).InSeconds());
  timestamp->set_nanos(((time - base::Time::UnixEpoch()).InMicroseconds() %
                        base::Time::kMicrosecondsPerSecond) *
                       base::Time::kNanosecondsPerMicrosecond);
}

base::Time TimestampToTime(const Timestamp& timestamp) {
  base::Time t = base::Time::UnixEpoch();
  t += base::TimeDelta::FromSeconds(timestamp.seconds());
  t += base::TimeDelta::FromMicroseconds(
      timestamp.nanos() / base::Time::kNanosecondsPerMicrosecond);
  return t;
}

std::unique_ptr<Duration> CreateDurationFromTimeDelta(
    const base::TimeDelta& time_delta) {
  std::unique_ptr<Duration> duration(new Duration);
  TimeDeltaToDuration(time_delta, duration.get());
  return duration;
}

std::unique_ptr<Timestamp> CreateTimestampFromTime(const base::Time& time) {
  std::unique_ptr<Timestamp> timestamp(new Timestamp);
  TimeToTimestamp(time, timestamp.get());
  return timestamp;
}

}  // namespace protobuf_parser

}  // namespace data_reduction_proxy
