// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_DOWNLOAD_PUBLIC_COMMON_DOWNLOAD_FEATURES_H_
#define COMPONENTS_DOWNLOAD_PUBLIC_COMMON_DOWNLOAD_FEATURES_H_

#include "base/feature_list.h"
#include "build/build_config.h"
#include "components/download/public/common/download_export.h"

namespace download {
namespace features {

// Whether offline content provider should be used for the downloads UI..
COMPONENTS_DOWNLOAD_EXPORT extern const base::Feature
    kUseDownloadOfflineContentProvider;

// Whether download auto-resumptions are enabled in native.
COMPONENTS_DOWNLOAD_EXPORT extern const base::Feature
    kDownloadAutoResumptionNative;

// Whether a download can be handled by parallel jobs.
COMPONENTS_DOWNLOAD_EXPORT extern const base::Feature kParallelDownloading;

// Whether metadata for new in-progress downloads will be be stored in download
// DB, rather than history DB.
COMPONENTS_DOWNLOAD_EXPORT extern const base::Feature
    kDownloadDBForNewDownloads;

#if defined(OS_ANDROID)
// Whether download expiration date will be refreshed on resumption.
COMPONENTS_DOWNLOAD_EXPORT extern const base::Feature kRefreshExpirationDate;
#endif

// Whether concurrent downloads to the same target path should be allowed.
COMPONENTS_DOWNLOAD_EXPORT extern const base::Feature
    kPreventDownloadsWithSamePath;

// Whether in-progress download manager will be used to initialize download
// service.
COMPONENTS_DOWNLOAD_EXPORT extern const base::Feature
    kUseInProgressDownloadManagerForDownloadService;

// Whether download resumption is allowed when there are no strong validators.
COMPONENTS_DOWNLOAD_EXPORT extern const base::Feature
    kAllowDownloadResumptionWithoutStrongValidators;

// Whether download resumption is allowed when there are no strong validators.
COMPONENTS_DOWNLOAD_EXPORT extern const base::Feature
    kUseParallelRequestsForUnknwonRangeSupport;
}  // namespace features

}  // namespace download

#endif  // COMPONENTS_DOWNLOAD_PUBLIC_COMMON_DOWNLOAD_FEATURES_H_
