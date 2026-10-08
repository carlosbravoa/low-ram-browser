// Copyright 2026 The low-ram-browser Authors
// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/lrb_browser_main_parts.h"

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/ref_counted_memory.h"
#include "base/run_loop.h"
#include "components/performance_manager/embedder/graph_features.h"
#include "components/performance_manager/embedder/performance_manager_lifetime.h"
#include "device/bluetooth/bluetooth_adapter_factory.h"
#include "device/bluetooth/dbus/dbus_bluez_manager_wrapper_linux.h"
#include "lrb/browser/devtools_manager_delegate.h"
#include "lrb/browser/list_updater.h"
#include "lrb/browser/lrb_browser_context.h"
#include "lrb/browser/lrb_content_browser_client.h"
#include "lrb/browser/saved_windows.h"
#include "lrb/browser/shell.h"
#include "lrb/browser/ui/lrb_platform_delegate.h"
#include "lrb/browser/ui/settings_dialog.h"
#include "lrb/common/lrb_switches.h"
#include "lrb/coordinator/rules.h"
#include "net/base/module/net_module.h"
#include "net/grit/net_resources.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/linux/linux_ui.h"
#include "ui/linux/linux_ui_factory.h"
#include "url/gurl.h"
#include "url/url_constants.h"

namespace lrb {

namespace {

// The header of file:// directory listings.
scoped_refptr<base::RefCountedMemory> PlatformResourceProvider(int key) {
  if (key == IDR_DIR_HEADER_HTML) {
    return ui::ResourceBundle::GetSharedInstance().LoadDataResourceBytes(
        IDR_DIR_HEADER_HTML);
  }
  return nullptr;
}

}  // namespace

LrbBrowserMainParts::LrbBrowserMainParts(LrbContentBrowserClient& client)
    : client_(client) {}
LrbBrowserMainParts::~LrbBrowserMainParts() = default;

void LrbBrowserMainParts::PostCreateMainMessageLoop() {
  // Bluetooth over BlueZ (D-Bus): Web Bluetooth and WebAuthn's
  // phone-as-passkey (caBLE) use the adapter.
  bluez::DBusBluezManagerWrapperLinux::Initialize();
}

void LrbBrowserMainParts::ToolkitInitialized() {
  // The desktop's theme, fonts, cursor and input methods.
  ui::LinuxUi::SetInstance(ui::GetDefaultLinuxUi());
}

int LrbBrowserMainParts::PostCreateThreads() {
  performance_manager_lifetime_ =
      std::make_unique<performance_manager::PerformanceManagerLifetime>(
          performance_manager::GraphFeatures::WithMinimal(), base::DoNothing());
  return 0;
}

int LrbBrowserMainParts::PreMainMessageLoopRun() {
  browser_context_ = std::make_unique<LrbBrowserContext>(false);
  off_the_record_browser_context_ = std::make_unique<LrbBrowserContext>(true);
  // Persistent origin trials before the first request.
  browser_context_->GetOriginTrialsControllerDelegate();
  off_the_record_browser_context_->GetOriginTrialsControllerDelegate();

  Shell::Initialize(std::make_unique<LrbPlatformDelegate>());
  net::NetModule::SetResourceProvider(PlatformResourceProvider);
  // Only when asked for: a debugging server would let any local process
  // drive every window, a bank's included.
  LrbDevToolsManagerDelegate::StartRemoteDebuggingIfAsked(
      browser_context_.get());
  // Before the first window: that navigation's throttles run synchronously
  // and must already know whether a coordinator is there.
  client_->ConnectToCoordinator();
  OpenFirstWindow();
  return 0;
}

void LrbBrowserMainParts::OpenFirstWindow() {
  const base::CommandLine& command_line =
      *base::CommandLine::ForCurrentProcess();
  if (command_line.HasSwitch(switches::kSettingsWindow)) {
    ShowSettingsAlone(
        command_line.GetSwitchValueASCII(switches::kSettingsWindow) ==
            "first-start",
        base::BindOnce(&Shell::Shutdown));
    return;
  }
  if (command_line.HasSwitch(switches::kUpdateLists)) {
    // The filter-list updater: no window; exits when done.
    list_updater_ = std::make_unique<ListUpdater>(
        browser_context_.get(),
        command_line.GetSwitchValuePath(switches::kUpdateLists),
        command_line.GetSwitchValueASCII(switches::kAdblockSetting) == "lean",
        base::BindOnce(&Shell::Shutdown));
    list_updater_->Start();
    return;
  }
  const base::CommandLine::StringVector& args = command_line.GetArgs();
  GURL startup_url = args.empty() ? GURL() : GURL(args[0]);
  if (!startup_url.is_valid() || !startup_url.has_scheme()) {
    startup_url = GURL(url::kAboutBlankURL);
  }
  browser_context_->StartZoomLevels();
  // Replacing another site's window: open where it was.
  if (std::optional<coordinator::WindowBounds> bounds =
          coordinator::ParseBounds(
              command_line.GetSwitchValueASCII(switches::kWindowBounds))) {
    LrbPlatformDelegate::SetNextWindowBounds(
        gfx::Rect(bounds->x, bounds->y, bounds->width, bounds->height));
  }
  // Left from another site's page: Back on the first page returns there.
  LrbPlatformDelegate::SetNextWindowBackUrl(
      GURL(command_line.GetSwitchValueASCII(switches::kBackUrl)));
  OpenStartupWindows(browser_context_.get(), startup_url,
                     command_line.HasSwitch(switches::kRestoreLeft),
                     command_line.HasSwitch(switches::kResume));
}

void LrbBrowserMainParts::WillRunMainMessageLoop(
    std::unique_ptr<base::RunLoop>& run_loop) {
  Shell::SetMainMessageLoopQuitClosure(run_loop->QuitClosure());
}

void LrbBrowserMainParts::PostMainMessageLoopRun() {
  DCHECK_EQ(Shell::windows().size(), 0u);
  // Before the browser context it points to.
  list_updater_.reset();
  LrbDevToolsManagerDelegate::StopRemoteDebugging();
  browser_context_.reset();
  off_the_record_browser_context_.reset();
  ui::LinuxUi::SetInstance(nullptr);
  performance_manager_lifetime_.reset();
}

void LrbBrowserMainParts::PostDestroyThreads() {
  device::BluetoothAdapterFactory::Shutdown();
  bluez::DBusBluezManagerWrapperLinux::Shutdown();
}

}  // namespace lrb
