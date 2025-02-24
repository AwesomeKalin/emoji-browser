// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>

#include "base/bind.h"
#include "base/macros.h"
#include "base/test/launcher/unit_test_launcher.h"
#include "base/test/scoped_task_environment.h"
#include "base/test/test_suite.h"
#include "build/build_config.h"

#if defined(OS_MACOSX) && !defined(OS_IOS)
#include "base/test/mock_chrome_application_mac.h"
#endif

#if defined(USE_OZONE)
#include "base/command_line.h"
#include "mojo/core/embedder/embedder.h"                  // nogncheck
#include "services/service_manager/public/cpp/service.h"  // nogncheck
#include "services/service_manager/public/cpp/service_binding.h"  // nogncheck
#include "services/service_manager/public/cpp/test/test_connector_factory.h"  // nogncheck
#include "services/viz/public/interfaces/constants.mojom.h"  // nogncheck
#include "ui/ozone/public/ozone_platform.h"
#endif

namespace {
#if defined(USE_OZONE)
class OzoneDrmTestService : public service_manager::Service {
 public:
  explicit OzoneDrmTestService(service_manager::mojom::ServiceRequest request)
      : service_binding_(this, std::move(request)) {}
  ~OzoneDrmTestService() override = default;

  service_manager::BinderRegistry* registry() { return &registry_; }

  void OnBindInterface(const service_manager::BindSourceInfo& source_info,
                       const std::string& interface_name,
                       mojo::ScopedMessagePipeHandle interface_pipe) override {
    registry_.BindInterface(interface_name, std::move(interface_pipe));
  }

 private:
  service_manager::ServiceBinding service_binding_;
  service_manager::BinderRegistry registry_;

  DISALLOW_COPY_AND_ASSIGN(OzoneDrmTestService);
};
#endif

class GlTestSuite : public base::TestSuite {
 public:
  GlTestSuite(int argc, char** argv) : base::TestSuite(argc, argv) {
  }

 protected:
  void Initialize() override {
    base::TestSuite::Initialize();

#if defined(OS_MACOSX) && !defined(OS_IOS)
    // This registers a custom NSApplication. It must be done before
    // ScopedTaskEnvironment registers a regular NSApplication.
    mock_cr_app::RegisterMockCrApp();
#endif

    scoped_task_environment_ =
        std::make_unique<base::test::ScopedTaskEnvironment>(
            base::test::ScopedTaskEnvironment::MainThreadType::UI);

#if defined(USE_OZONE)
    // OzonePlatform DRM implementation may attempt to connect to the viz
    // service to acquire interfaces.
    service_ = std::make_unique<OzoneDrmTestService>(
        connector_factory_.RegisterInstance(viz::mojom::kVizServiceName));

    // Make Ozone run in single-process mode, where it doesn't expect a GPU
    // process and it spawns and starts its own DRM thread. Note that this mode
    // still requires a mojo pipe for in-process communication between the host
    // and GPU components.
    ui::OzonePlatform::InitParams params;
    params.single_process = true;
    params.connector = connector_factory_.GetDefaultConnector();

    // This initialization must be done after ScopedTaskEnvironment has
    // initialized the UI thread.
    ui::OzonePlatform::InitializeForUI(params);
    ui::OzonePlatform::GetInstance()->AddInterfaces(service_->registry());
#endif
  }

  void Shutdown() override {
    base::TestSuite::Shutdown();
  }

 private:
  std::unique_ptr<base::test::ScopedTaskEnvironment> scoped_task_environment_;

#if defined(USE_OZONE)
  service_manager::TestConnectorFactory connector_factory_;
  std::unique_ptr<OzoneDrmTestService> service_;
#endif

  DISALLOW_COPY_AND_ASSIGN(GlTestSuite);
};

}  // namespace

int main(int argc, char** argv) {
#if defined(USE_OZONE)
  mojo::core::Init();
#endif

  GlTestSuite test_suite(argc, argv);

  return base::LaunchUnitTests(
      argc, argv,
      base::BindOnce(&GlTestSuite::Run, base::Unretained(&test_suite)));
}
