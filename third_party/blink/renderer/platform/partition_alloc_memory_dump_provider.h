// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_PARTITION_ALLOC_MEMORY_DUMP_PROVIDER_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_PARTITION_ALLOC_MEMORY_DUMP_PROVIDER_H_

#include "base/macros.h"
#include "base/trace_event/memory_dump_provider.h"
#include "third_party/blink/renderer/platform/platform_export.h"

namespace blink {

class PLATFORM_EXPORT PartitionAllocMemoryDumpProvider final
    : public base::trace_event::MemoryDumpProvider {
  // TODO(tasak): PartitionAllocMemoryDumpProvider should be
  // USING_FAST_MALLOC. c.f. crbug.com/584196

 public:
  static PartitionAllocMemoryDumpProvider* Instance();
  ~PartitionAllocMemoryDumpProvider() override;

  // MemoryDumpProvider implementation.
  bool OnMemoryDump(const base::trace_event::MemoryDumpArgs&,
                    base::trace_event::ProcessMemoryDump*) override;

 private:
  PartitionAllocMemoryDumpProvider();

  DISALLOW_COPY_AND_ASSIGN(PartitionAllocMemoryDumpProvider);
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_PLATFORM_PARTITION_ALLOC_MEMORY_DUMP_PROVIDER_H_
