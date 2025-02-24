// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeos/services/ime/decoder/decoder_engine.h"

#include "base/bind_helpers.h"
#include "base/files/file_path.h"
#include "build/buildflag.h"
#include "chromeos/services/ime/public/cpp/buildflags.h"

namespace chromeos {
namespace ime {

namespace {

// TODO(https://crbug.com/837156): Use define instead.
#if BUILDFLAG(ENABLE_CROS_IME_EXAMPLE_SO)
const char kDecoderLibName[] = "input_decoder_example";
#else
const char kDecoderLibName[] = "input_decoder_engine";
#endif

// A client delegate that makes calls on client side.
class ClientDelegate : public ImeClientDelegate {
 public:
  ClientDelegate(const std::string& ime_spec,
                 mojo::PendingRemote<mojom::InputChannel> remote)
      : ime_spec_(ime_spec), client_remote_(std::move(remote)) {
    client_remote_.set_disconnect_handler(base::BindOnce(
        &ClientDelegate::OnDisconnected, base::Unretained(this)));
  }

  ~ClientDelegate() override {}

  const char* ImeSpec() override { return ime_spec_.c_str(); }

  void Process(const uint8_t* data, size_t size) override {
    if (client_remote_ && client_remote_.is_bound()) {
      std::vector<uint8_t> msg(data, data + size);
      client_remote_->ProcessMessage(msg, base::DoNothing());
    }
  }

  void Destroy() override {}

 private:
  void OnDisconnected() {
    client_remote_.reset();
    LOG(ERROR) << "Client remote is disconnected." << ime_spec_;
  }

  // The ime specification which is unique in the scope of engine.
  std::string ime_spec_;

  // The InputChannel remote used to talk to the client.
  mojo::Remote<mojom::InputChannel> client_remote_;
};

}  // namespace

DecoderEngine::DecoderEngine(ImeCrosPlatform* platform) : platform_(platform) {
  // TODO(https://crbug.com/837156): Connect utility services for InputEngine
  // via connector(). Eg. take advantage of the file and network services to
  // download relevant language models required by the decocers to the device.

  // Load the decoder library.
  base::NativeLibraryLoadError load_error;
  base::FilePath lib_path(base::GetNativeLibraryName(kDecoderLibName));
  library_ = base::ScopedNativeLibrary(lib_path);

  if (!library_.is_valid()) {
    LOG(ERROR) << "Failed to load a decoder shared library, error: "
               << library_.GetError()->ToString();
    return;
  }

  ImeMainEntryCreateFn createMainEntryFn =
      reinterpret_cast<ImeMainEntryCreateFn>(
          library_.GetFunctionPointer(IME_MAIN_ENTRY_CREATE_FN_NAME));

  engine_main_entry_ = createMainEntryFn(platform_);
  LOG(ERROR) << "Loaded SO main_entry in DecoderEngine!";
}

DecoderEngine::~DecoderEngine() {}

bool DecoderEngine::BindRequest(
    const std::string& ime_spec,
    mojo::PendingReceiver<mojom::InputChannel> receiver,
    mojo::PendingRemote<mojom::InputChannel> remote,
    const std::vector<uint8_t>& extra) {
  // If the shared library supports this ime_spec.
  if (IsImeSupported(ime_spec)) {
    // Activates an IME engine via the shared library. Passing a
    // |ClientDelegate| for engine instance created by the shared library to
    // make safe calls on the client.
    if (engine_main_entry_->ActivateIme(
            ime_spec.c_str(),
            new ClientDelegate(ime_spec, std::move(remote)))) {
      channel_receivers_.Add(this, std::move(receiver));
      // TODO(https://crbug.com/837156): Registry connection error handler.
      return true;
    }
    return false;
  }

  // Otherwise, try the rule-based engine for this ime_spec.
  return InputEngine::BindRequest(ime_spec, std::move(receiver),
                                  std::move(remote), extra);
}

bool DecoderEngine::IsImeSupported(const std::string& ime_spec) {
  return engine_main_entry_ &&
         engine_main_entry_->IsImeSupported(ime_spec.c_str());
}

void DecoderEngine::ProcessMessage(const std::vector<uint8_t>& message,
                                   ProcessMessageCallback callback) {
  std::vector<uint8_t> result;

  // TODO(https://crbug.com/837156): Implement the functions below by calling
  // the corresponding functions of the shared library loaded.
  std::move(callback).Run(result);
}

}  // namespace ime
}  // namespace chromeos
