// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/public/browser/site_isolation_policy.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>

#include "base/bind.h"
#include "base/command_line.h"
#include "base/feature_list.h"
#include "base/macros.h"
#include "base/metrics/field_trial_params.h"
#include "base/metrics/histogram_macros.h"
#include "base/no_destructor.h"
#include "base/strings/string_split.h"
#include "base/timer/timer.h"
#include "build/build_config.h"
#include "content/public/browser/child_process_security_policy.h"
#include "content/public/browser/content_browser_client.h"
#include "content/public/common/content_client.h"
#include "content/public/common/content_features.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/resource_type.h"
#include "services/network/public/mojom/network_service.mojom.h"
#include "url/gurl.h"

namespace content {

namespace {

bool IsSiteIsolationDisabled() {
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kDisableSiteIsolation)) {
    return true;
  }

#if defined(OS_ANDROID)
  // Desktop platforms no longer support disabling Site Isolation by policy.
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kDisableSiteIsolationForPolicy)) {
    return true;
  }
#endif

  return GetContentClient() &&
         GetContentClient()->browser()->ShouldDisableSiteIsolation();
}

}  // namespace

// static
bool SiteIsolationPolicy::UseDedicatedProcessesForAllSites() {
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kSitePerProcess)) {
    return true;
  }

  if (IsSiteIsolationDisabled())
    return false;

  // The switches above needs to be checked first, because if the
  // ContentBrowserClient consults a base::Feature, then it will activate the
  // field trial and assigns the client either to a control or an experiment
  // group - such assignment should be final.
  return GetContentClient() &&
         GetContentClient()->browser()->ShouldEnableStrictSiteIsolation();
}

// static
void SiteIsolationPolicy::PopulateURLLoaderFactoryParamsPtrForCORB(
    network::mojom::URLLoaderFactoryParams* params) {
  // --disable-web-security also disables Cross-Origin Read Blocking (CORB).
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kDisableWebSecurity)) {
    params->is_corb_enabled = false;
    return;
  }

  params->is_corb_enabled = true;
}

// static
bool SiteIsolationPolicy::AreIsolatedOriginsEnabled() {
  // NOTE: Because it is possible for --isolate-origins to be isolating origins
  // at a finer-than-site granularity, we do not suppress --isolate-origins when
  // --site-per-process is also enabled.
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kIsolateOrigins)) {
    return true;
  }

  if (IsSiteIsolationDisabled())
    return false;

  // The feature needs to be checked last, because checking the feature
  // activates the field trial and assigns the client either to a control or an
  // experiment group - such assignment should be final.
  return base::FeatureList::IsEnabled(features::kIsolateOrigins);
}

// static
bool SiteIsolationPolicy::IsStrictOriginIsolationEnabled() {
  // TODO(wjmaclean): Figure out what should happen when this feature is
  // combined with --isolate-origins.
  if (IsSiteIsolationDisabled())
    return false;

  // The feature needs to be checked last, because checking the feature
  // activates the field trial and assigns the client either to a control or an
  // experiment group - such assignment should be final.
  return base::FeatureList::IsEnabled(features::kStrictOriginIsolation);
}

// static
bool SiteIsolationPolicy::IsErrorPageIsolationEnabled(bool in_main_frame) {
  return GetContentClient()->browser()->ShouldIsolateErrorPage(in_main_frame);
}

// static
bool SiteIsolationPolicy::ShouldPdfCompositorBeEnabledForOopifs() {
  // TODO(weili): We only create pdf compositor client and use pdf compositor
  // service when site-per-process or isolate-origins flag/feature is enabled,
  // or top-document-isolation feature is enabled. This may not cover all cases
  // where OOPIF is used such as isolate-extensions, but should be good for
  // feature testing purpose. Eventually, we will remove this check and use pdf
  // compositor service by default for printing.
  return AreIsolatedOriginsEnabled() || UseDedicatedProcessesForAllSites();
}

// static
bool SiteIsolationPolicy::AreDynamicIsolatedOriginsEnabled() {
  return !IsSiteIsolationDisabled();
}

// static
std::vector<url::Origin>
SiteIsolationPolicy::GetIsolatedOriginsFromCommandLine() {
  std::string cmdline_arg =
      base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
          switches::kIsolateOrigins);
  std::vector<url::Origin> origins;
  // Note that --isolate-origins trumps the opt-out flag, so the command-line
  // origins are returned regardless of IsSiteIsolationDisabled().
  if (!cmdline_arg.empty()) {
    origins = ParseIsolatedOrigins(cmdline_arg);
    UMA_HISTOGRAM_COUNTS_1000("SiteIsolation.IsolateOrigins.Size",
                              origins.size());
  }
  return origins;
}

std::vector<url::Origin>
SiteIsolationPolicy::GetIsolatedOriginsFromFieldTrial() {
  std::vector<url::Origin> origins;

  // Check if site isolation modes are turned off (e.g., due to an opt-out
  // flag).
  if (IsSiteIsolationDisabled())
    return origins;

  // The feature needs to be checked after the opt-out, because checking the
  // feature activates the field trial and assigns the client either to a
  // control or an experiment group - such assignment should be final.
  if (base::FeatureList::IsEnabled(features::kIsolateOrigins)) {
    std::string field_trial_arg = base::GetFieldTrialParamValueByFeature(
        features::kIsolateOrigins,
        features::kIsolateOriginsFieldTrialParamName);
    origins = ParseIsolatedOrigins(field_trial_arg);
  }

  return origins;
}

void SiteIsolationPolicy::ApplyGlobalIsolatedOrigins() {
  ChildProcessSecurityPolicy* policy =
      ChildProcessSecurityPolicy::GetInstance();

  std::vector<url::Origin> from_cmdline = GetIsolatedOriginsFromCommandLine();
  policy->AddIsolatedOrigins(
      from_cmdline,
      ChildProcessSecurityPolicy::IsolatedOriginSource::COMMAND_LINE);

  std::vector<url::Origin> from_trial = GetIsolatedOriginsFromFieldTrial();
  policy->AddIsolatedOrigins(
      from_trial,
      ChildProcessSecurityPolicy::IsolatedOriginSource::FIELD_TRIAL);

  std::vector<url::Origin> from_embedder =
      GetContentClient()->browser()->GetOriginsRequiringDedicatedProcess();
  policy->AddIsolatedOrigins(
      from_embedder,
      ChildProcessSecurityPolicy::IsolatedOriginSource::BUILT_IN);
}

// static
std::vector<url::Origin> SiteIsolationPolicy::ParseIsolatedOrigins(
    base::StringPiece arg) {
  std::vector<base::StringPiece> origin_strings = base::SplitStringPiece(
      arg, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);

  std::vector<url::Origin> origins;
  origins.reserve(origin_strings.size());
  for (const base::StringPiece& origin_string : origin_strings) {
    url::Origin origin = url::Origin::Create(GURL(origin_string));
    if (!origin.opaque())
      origins.push_back(origin);
  }
  return origins;
}

// static
void SiteIsolationPolicy::StartRecordingSiteIsolationFlagUsage() {
  RecordSiteIsolationFlagUsage();
  // Record the flag usage metrics every 24 hours.  Even though site isolation
  // flags can't change dynamically at runtime, collecting these stats daily
  // helps determine the overall population of users who run with a given flag
  // on any given day.
  static base::NoDestructor<base::RepeatingTimer> update_stats_timer;
  update_stats_timer->Start(
      FROM_HERE, base::TimeDelta::FromHours(24),
      base::BindRepeating(&SiteIsolationPolicy::RecordSiteIsolationFlagUsage));
}

// static
void SiteIsolationPolicy::RecordSiteIsolationFlagUsage() {
  // For --site-per-process and --isolate-origins, include flags specified on
  // command-line, in chrome://flags, and via enterprise policy (i.e., include
  // switches::kSitePerProcess and switches::kIsolateOrigins).  Exclude these
  // modes being set through field trials (i.e., exclude
  // features::kSitePerProcess and features::IsolateOrigins).
  UMA_HISTOGRAM_BOOLEAN("SiteIsolation.Flags.IsolateOrigins",
                        base::CommandLine::ForCurrentProcess()->HasSwitch(
                            switches::kIsolateOrigins));

  UMA_HISTOGRAM_BOOLEAN("SiteIsolation.Flags.SitePerProcess",
                        base::CommandLine::ForCurrentProcess()->HasSwitch(
                            switches::kSitePerProcess));
}

}  // namespace content
