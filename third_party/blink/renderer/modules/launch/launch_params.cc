// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file

#include "third_party/blink/renderer/modules/launch/launch_params.h"

#include "third_party/blink/renderer/modules/native_file_system/native_file_system_handle.h"
#include "third_party/blink/renderer/platform/heap/visitor.h"

namespace blink {

LaunchParams::LaunchParams(String cause,
                           HeapVector<Member<NativeFileSystemHandle>> files)
    : cause_(cause), files_(files) {}

LaunchParams::~LaunchParams() = default;

void LaunchParams::Trace(blink::Visitor* visitor) {
  visitor->Trace(files_);
  ScriptWrappable::Trace(visitor);
}

}  // namespace blink
