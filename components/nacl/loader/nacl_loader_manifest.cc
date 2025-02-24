// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/nacl/loader/nacl_loader_manifest.h"

#include <set>

#include "base/no_destructor.h"
#include "components/nacl/common/nacl_constants.h"
#include "services/service_manager/public/cpp/manifest_builder.h"

const service_manager::Manifest& GetNaClLoaderManifest() {
  static base::NoDestructor<service_manager::Manifest> manifest{
      service_manager::ManifestBuilder()
          .WithServiceName(nacl::kNaClLoaderServiceName)
          .WithDisplayName("NaCl loader")
          // NOTE: The interfaces below are not implemented in the nacl_loader
          // service, but they are requested from all child processes by common
          // browser-side code. We list them here to make the Service Manager
          // happy.
          .ExposeCapability("browser",
                            std::set<const char*>{
                                "IPC.mojom.ChannelBootstrap",
                                "content.mojom.Child",
                                "content.mojom.ChildControl",
                                "content.mojom.ChildHistogramFetcherFactory",
                                "content.mojom.ResourceUsageReporter",
                            })
          .Build()};
  return *manifest;
}
