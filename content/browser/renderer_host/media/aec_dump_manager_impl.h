// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_RENDERER_HOST_MEDIA_AEC_DUMP_MANAGER_IMPL_H_
#define CONTENT_BROWSER_RENDERER_HOST_MEDIA_AEC_DUMP_MANAGER_IMPL_H_

#include <map>
#include <memory>

#include "base/process/process_handle.h"
#include "content/common/media/aec_dump.mojom.h"
#include "mojo/public/cpp/bindings/receiver_set.h"

namespace base {
class File;
class FilePath;
}  // namespace base

namespace content {

class AecDumpManagerImpl : public mojom::AecDumpManager {
 public:
  AecDumpManagerImpl();
  ~AecDumpManagerImpl() override;

  void AddRequest(mojo::InterfaceRequest<mojom::AecDumpManager> request);

  // Start generating AEC dumps using default settings.
  void AutoStart();

  // Start generating AEC dumps using a specific file path prefix.
  void Start(const base::FilePath& file_path);

  // Stop generating AEC dumps.
  void Stop();

  // mojom::AecDumpManager methods:
  void Add(mojo::PendingRemote<mojom::AecDumpAgent> agent) override;

  void set_pid(base::ProcessId pid) { pid_ = pid; }

 private:
  void CreateFileAndStartDump(const base::FilePath& file_path, int id);
  void StartDump(int id, base::File file);
  void OnAgentDisconnected(int id);

  base::ProcessId pid_ = 0;
  std::map<int /* id */, mojo::Remote<mojom::AecDumpAgent>> agents_;
  int id_counter_ = 0;
  mojo::ReceiverSet<mojom::AecDumpManager> receiver_set_;

  base::WeakPtrFactory<AecDumpManagerImpl> weak_factory_{this};

  DISALLOW_COPY_AND_ASSIGN(AecDumpManagerImpl);
};

}  // namespace content

#endif  // CONTENT_BROWSER_RENDERER_HOST_MEDIA_AEC_DUMP_MANAGER_IMPL_H_
