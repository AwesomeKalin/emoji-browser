// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "headless/lib/browser/headless_content_browser_client.h"

#include <memory>
#include <unordered_set>

#include "base/base_switches.h"
#include "base/bind.h"
#include "base/callback.h"
#include "base/command_line.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "build/build_config.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/client_certificate_delegate.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/browser/resource_dispatcher_host.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/service_names.mojom.h"
#include "headless/app/headless_shell_switches.h"
#include "headless/lib/browser/headless_browser_context_impl.h"
#include "headless/lib/browser/headless_browser_impl.h"
#include "headless/lib/browser/headless_browser_main_parts.h"
#include "headless/lib/browser/headless_devtools_manager_delegate.h"
#include "headless/lib/browser/headless_overlay_manifests.h"
#include "headless/lib/browser/headless_quota_permission_context.h"
#include "headless/lib/headless_macros.h"
#include "net/base/url_util.h"
#include "net/ssl/client_cert_identity.h"
#include "printing/buildflags/buildflags.h"
#include "services/service_manager/sandbox/switches.h"
#include "storage/browser/quota/quota_settings.h"
#include "ui/base/ui_base_switches.h"
#include "ui/gfx/switches.h"

#if defined(HEADLESS_USE_BREAKPAD)
#include "base/debug/leak_annotations.h"
#include "components/crash/content/app/breakpad_linux.h"
#include "components/crash/content/browser/crash_handler_host_linux.h"
#include "content/public/common/content_descriptors.h"
#endif  // defined(HEADLESS_USE_BREAKPAD)

namespace headless {

namespace {

#if defined(HEADLESS_USE_BREAKPAD)
breakpad::CrashHandlerHostLinux* CreateCrashHandlerHost(
    const std::string& process_type,
    const HeadlessBrowser::Options& options) {
  base::FilePath dumps_path = options.crash_dumps_dir;
  if (dumps_path.empty()) {
    bool ok = base::PathService::Get(base::DIR_MODULE, &dumps_path);
    DCHECK(ok);
  }

  {
    ANNOTATE_SCOPED_MEMORY_LEAK;
#if defined(OFFICIAL_BUILD)
    // Upload crash dumps in official builds, unless we're running in unattended
    // mode (not to be confused with headless mode in general -- see
    // chrome/common/env_vars.cc).
    static const char kHeadless[] = "CHROME_HEADLESS";
    bool upload = (getenv(kHeadless) == nullptr);
#else
    bool upload = false;
#endif
    breakpad::CrashHandlerHostLinux* crash_handler =
        new breakpad::CrashHandlerHostLinux(process_type, dumps_path, upload);
    crash_handler->StartUploaderThread();
    return crash_handler;
  }
}

int GetCrashSignalFD(const base::CommandLine& command_line,
                     const HeadlessBrowser::Options& options) {
  if (!breakpad::IsCrashReporterEnabled())
    return -1;

  std::string process_type =
      command_line.GetSwitchValueASCII(::switches::kProcessType);

  if (process_type == ::switches::kRendererProcess) {
    static breakpad::CrashHandlerHostLinux* crash_handler =
        CreateCrashHandlerHost(process_type, options);
    return crash_handler->GetDeathSignalSocket();
  }

  if (process_type == ::switches::kPpapiPluginProcess) {
    static breakpad::CrashHandlerHostLinux* crash_handler =
        CreateCrashHandlerHost(process_type, options);
    return crash_handler->GetDeathSignalSocket();
  }

  if (process_type == ::switches::kGpuProcess) {
    static breakpad::CrashHandlerHostLinux* crash_handler =
        CreateCrashHandlerHost(process_type, options);
    return crash_handler->GetDeathSignalSocket();
  }

  return -1;
}
#endif  // defined(HEADLESS_USE_BREAKPAD)

}  // namespace

HeadlessContentBrowserClient::HeadlessContentBrowserClient(
    HeadlessBrowserImpl* browser)
    : browser_(browser),
      append_command_line_flags_callback_(
          browser_->options()->append_command_line_flags_callback) {}

HeadlessContentBrowserClient::~HeadlessContentBrowserClient() = default;

std::unique_ptr<content::BrowserMainParts>
HeadlessContentBrowserClient::CreateBrowserMainParts(
    const content::MainFunctionParams&) {
  auto browser_main_parts =
      std::make_unique<HeadlessBrowserMainParts>(browser_);

  browser_->set_browser_main_parts(browser_main_parts.get());

  return browser_main_parts;
}

void HeadlessContentBrowserClient::OverrideWebkitPrefs(
    content::RenderViewHost* render_view_host,
    content::WebPreferences* prefs) {
  auto* browser_context = HeadlessBrowserContextImpl::From(
      render_view_host->GetProcess()->GetBrowserContext());
  base::RepeatingCallback<void(WebPreferences*)> callback =
      browser_context->options()->override_web_preferences_callback();
  if (callback)
    callback.Run(prefs);
}

content::DevToolsManagerDelegate*
HeadlessContentBrowserClient::GetDevToolsManagerDelegate() {
  return new HeadlessDevToolsManagerDelegate(browser_->GetWeakPtr());
}

base::Optional<service_manager::Manifest>
HeadlessContentBrowserClient::GetServiceManifestOverlay(
    base::StringPiece name) {
  if (name == content::mojom::kBrowserServiceName)
    return GetHeadlessContentBrowserOverlayManifest();

  if (name == content::mojom::kPackagedServicesServiceName)
    return GetHeadlessContentPackagedServicesOverlayManifest();

  return base::nullopt;
}

scoped_refptr<content::QuotaPermissionContext>
HeadlessContentBrowserClient::CreateQuotaPermissionContext() {
  return new HeadlessQuotaPermissionContext();
}

void HeadlessContentBrowserClient::GetQuotaSettings(
    content::BrowserContext* context,
    content::StoragePartition* partition,
    ::storage::OptionalQuotaSettingsCallback callback) {
  ::storage::GetNominalDynamicSettings(
      partition->GetPath(), context->IsOffTheRecord(),
      ::storage::GetDefaultDiskInfoHelper(), std::move(callback));
}

content::GeneratedCodeCacheSettings
HeadlessContentBrowserClient::GetGeneratedCodeCacheSettings(
    content::BrowserContext* context) {
  // If we pass 0 for size, disk_cache will pick a default size using the
  // heuristics based on available disk size. These are implemented in
  // disk_cache::PreferredCacheSize in net/disk_cache/cache_util.cc.
  return content::GeneratedCodeCacheSettings(true, 0, context->GetPath());
}

#if defined(OS_POSIX) && !defined(OS_MACOSX)
void HeadlessContentBrowserClient::GetAdditionalMappedFilesForChildProcess(
    const base::CommandLine& command_line,
    int child_process_id,
    content::PosixFileDescriptorInfo* mappings) {
#if defined(HEADLESS_USE_BREAKPAD)
  int crash_signal_fd = GetCrashSignalFD(command_line, *browser_->options());
  if (crash_signal_fd >= 0)
    mappings->Share(service_manager::kCrashDumpSignal, crash_signal_fd);
#endif  // defined(HEADLESS_USE_BREAKPAD)
}
#endif  // defined(OS_POSIX) && !defined(OS_MACOSX)

void HeadlessContentBrowserClient::AppendExtraCommandLineSwitches(
    base::CommandLine* command_line,
    int child_process_id) {
  // NOTE: We may be called on the UI or IO thread. If called on the IO thread,
  // |browser_| may have already been destroyed.

  command_line->AppendSwitch(::switches::kHeadless);
  const base::CommandLine& old_command_line(
      *base::CommandLine::ForCurrentProcess());
  if (old_command_line.HasSwitch(switches::kUserAgent)) {
    command_line->AppendSwitchNative(
        switches::kUserAgent,
        old_command_line.GetSwitchValueNative(switches::kUserAgent));
  }
#if defined(HEADLESS_USE_BREAKPAD)
  // This flag tells child processes to also turn on crash reporting.
  if (breakpad::IsCrashReporterEnabled())
    command_line->AppendSwitch(::switches::kEnableCrashReporter);
#endif  // defined(HEADLESS_USE_BREAKPAD)

  // If we're spawning a renderer, then override the language switch.
  std::string process_type =
      command_line->GetSwitchValueASCII(::switches::kProcessType);
  if (process_type == ::switches::kRendererProcess) {
    // Renderer processes are initialized on the UI thread, so this is safe.
    content::RenderProcessHost* render_process_host =
        content::RenderProcessHost::FromID(child_process_id);
    if (render_process_host) {
      HeadlessBrowserContextImpl* headless_browser_context_impl =
          HeadlessBrowserContextImpl::From(
              render_process_host->GetBrowserContext());

      std::vector<base::StringPiece> languages = base::SplitStringPiece(
          headless_browser_context_impl->options()->accept_language(), ",",
          base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
      if (!languages.empty()) {
        command_line->AppendSwitchASCII(::switches::kLang,
                                        languages[0].as_string());
      }
    }
  }

  if (append_command_line_flags_callback_) {
    HeadlessBrowserContextImpl* headless_browser_context_impl = nullptr;
    if (process_type == ::switches::kRendererProcess) {
      // Renderer processes are initialized on the UI thread, so this is safe.
      content::RenderProcessHost* render_process_host =
          content::RenderProcessHost::FromID(child_process_id);
      if (render_process_host) {
        headless_browser_context_impl = HeadlessBrowserContextImpl::From(
            render_process_host->GetBrowserContext());
      }
    }
    append_command_line_flags_callback_.Run(command_line,
                                            headless_browser_context_impl,
                                            process_type, child_process_id);
  }

#if defined(OS_LINUX)
  // Processes may only query perf_event_open with the BPF sandbox disabled.
  if (old_command_line.HasSwitch(::switches::kEnableThreadInstructionCount) &&
      old_command_line.HasSwitch(service_manager::switches::kNoSandbox)) {
    command_line->AppendSwitch(::switches::kEnableThreadInstructionCount);
  }
#endif
}

std::string HeadlessContentBrowserClient::GetAcceptLangs(
    content::BrowserContext* context) {
  return browser_->options()->accept_language;
}

void HeadlessContentBrowserClient::AllowCertificateError(
    content::WebContents* web_contents,
    int cert_error,
    const net::SSLInfo& ssl_info,
    const GURL& request_url,
    bool is_main_frame_request,
    bool strict_enforcement,
    bool expired_previous_decision,
    const base::Callback<void(content::CertificateRequestResultType)>&
        callback) {
  if (!callback.is_null()) {
    // If --allow-insecure-localhost is specified, and the request
    // was for localhost, then the error was not fatal.
    bool allow_localhost = base::CommandLine::ForCurrentProcess()->HasSwitch(
        ::switches::kAllowInsecureLocalhost);
    if (allow_localhost && net::IsLocalhost(request_url)) {
      callback.Run(content::CERTIFICATE_REQUEST_RESULT_TYPE_CONTINUE);
      return;
    }

    callback.Run(content::CERTIFICATE_REQUEST_RESULT_TYPE_DENY);
  }
}

void HeadlessContentBrowserClient::SelectClientCertificate(
    content::WebContents* web_contents,
    net::SSLCertRequestInfo* cert_request_info,
    net::ClientCertIdentityList client_certs,
    std::unique_ptr<content::ClientCertificateDelegate> delegate) {
  delegate->ContinueWithCertificate(nullptr, nullptr);
}

void HeadlessContentBrowserClient::ResourceDispatcherHostCreated() {
  resource_dispatcher_host_delegate_.reset(
      new HeadlessResourceDispatcherHostDelegate);
  content::ResourceDispatcherHost::Get()->SetDelegate(
      resource_dispatcher_host_delegate_.get());
}

bool HeadlessContentBrowserClient::ShouldEnableStrictSiteIsolation() {
  // TODO(lukasza): https://crbug.com/869494: Instead of overriding
  // ShouldEnableStrictSiteIsolation, //headless should inherit the default
  // site-per-process setting from //content - this way tools (tests, but also
  // production cases like screenshot or pdf generation) based on //headless
  // will use a mode that is actually shipping in Chrome.
  return browser_->options()->site_per_process;
}

::network::mojom::NetworkContextPtr
HeadlessContentBrowserClient::CreateNetworkContext(
    content::BrowserContext* context,
    bool in_memory,
    const base::FilePath& relative_partition_path) {
  return HeadlessBrowserContextImpl::From(context)->CreateNetworkContext(
      in_memory, relative_partition_path);
}

std::string HeadlessContentBrowserClient::GetProduct() {
  return browser_->options()->product_name_and_version;
}

std::string HeadlessContentBrowserClient::GetUserAgent() {
  return browser_->options()->user_agent;
}

}  // namespace headless
