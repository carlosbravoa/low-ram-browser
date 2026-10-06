// Copyright 2026 The low-ram-browser Authors
// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/shell.h"

#include <utility>

#include "base/command_line.h"
#include "base/compiler_specific.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/run_loop.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/document_picture_in_picture_window_controller.h"
#include "content/public/browser/file_select_listener.h"
#include "content/public/browser/javascript_dialog_manager.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/picture_in_picture_window_controller.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/renderer_preferences_util.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/content_switches.h"
#include "lrb/browser/lrb_content_browser_client.h"
#include "lrb/browser/ui/lrb_platform_delegate.h"
#include "third_party/blink/public/common/peerconnection/webrtc_ip_handling_policy.h"
#include "third_party/blink/public/common/renderer_preferences/renderer_preferences.h"
#include "third_party/blink/public/mojom/input/pointer_lock_result.mojom.h"
#include "third_party/blink/public/mojom/window_features/window_features.mojom.h"

namespace lrb {

namespace {

// Null until the main message loop runs.
base::OnceClosure& GetMainMessageLoopQuitClosure() {
  static base::NoDestructor<base::OnceClosure> closure;
  return *closure;
}

constexpr int kDefaultWindowWidthDip = 800;
constexpr int kDefaultWindowHeightDip = 600;

// The harness sizes windows with content_shell's switch.
constexpr char kHostWindowSize[] = "content-shell-host-window-size";

// Owned: acquired in Initialize(), released in Shutdown(). (A global
// unique_ptr would need a static destructor.)
LrbPlatformDelegate* g_platform = nullptr;

}  // namespace

// static
std::vector<Shell*>& Shell::windows() {
  static base::NoDestructor<std::vector<Shell*>> windows;
  return *windows;
}

// static
bool Shell::ShouldHideToolbar() {
  return base::CommandLine::ForCurrentProcess()->HasSwitch(
      "content-shell-hide-toolbar");
}

Shell::Shell(std::unique_ptr<content::WebContents> web_contents)
    : WebContentsObserver(web_contents.get()),
      web_contents_(std::move(web_contents)) {
  web_contents_->SetDelegate(this);
  blink::RendererPreferences* prefs = web_contents_->GetMutableRendererPrefs();
  content::UpdateFontRendererPreferencesFromSystemSettings(prefs);
  // navigator.languages, as the Accept-Language header.
  prefs->accept_languages = LrbContentBrowserClient::Get()->GetAcceptLangs(
      web_contents_->GetBrowserContext());
  windows().push_back(this);
}

Shell::~Shell() {
  g_platform->CleanUp(this);
  std::erase(windows(), this);
  web_contents_->SetDelegate(nullptr);
  web_contents_.reset();
  if (windows().empty()) {
    g_platform->DidCloseLastWindow();
  }
}

// static
Shell* Shell::CreateShell(std::unique_ptr<content::WebContents> web_contents,
                          const gfx::Size& initial_size) {
  content::WebContents* raw_web_contents = web_contents.get();
  Shell* shell = new Shell(std::move(web_contents));
  g_platform->CreatePlatformWindow(shell, initial_size);

  // Not RenderFrameHost or RenderViewHost state here: it would be lost after
  // a cross-process navigation.
  const base::CommandLine& command_line =
      *base::CommandLine::ForCurrentProcess();
  if (command_line.HasSwitch(::switches::kForceWebRtcIPHandlingPolicy)) {
    raw_web_contents->GetMutableRendererPrefs()->webrtc_ip_handling_policy =
        blink::ToWebRTCIPHandlingPolicy(command_line.GetSwitchValueASCII(
            ::switches::kForceWebRtcIPHandlingPolicy));
  }

  g_platform->SetContents(shell);
  return shell;
}

// static
void Shell::SetMainMessageLoopQuitClosure(base::OnceClosure quit_closure) {
  GetMainMessageLoopQuitClosure() = std::move(quit_closure);
}

// static
Shell* Shell::FromWebContents(content::WebContents* web_contents) {
  for (Shell* window : windows()) {
    if (window->web_contents() && window->web_contents() == web_contents) {
      return window;
    }
  }
  return nullptr;
}

// static
void Shell::Initialize(std::unique_ptr<LrbPlatformDelegate> platform) {
  DCHECK(!g_platform);
  g_platform = platform.release();
  g_platform->Initialize();
}

// static
void Shell::Shutdown() {
  if (!g_platform) {
    return;  // Already shut down.
  }
  content::DevToolsAgentHost::DetachAllClients();
  while (!windows().empty()) {
    windows().back()->Close();
  }
  delete g_platform;
  g_platform = nullptr;

  for (auto it = content::RenderProcessHost::AllHostsIterator(); !it.IsAtEnd();
       it.Advance()) {
    it.GetCurrentValue()->DisableRefCounts();
  }
  if (auto& quit_loop = GetMainMessageLoopQuitClosure()) {
    std::move(quit_loop).Run();
  }
  // Lets window teardown tasks run.
  base::RunLoop().RunUntilIdle();
}

// static
gfx::Size Shell::AdjustWindowSize(const gfx::Size& initial_size) {
  return initial_size.IsEmpty() ? GetShellDefaultSize() : initial_size;
}

// static
Shell* Shell::CreateNewWindow(
    content::BrowserContext* browser_context,
    const GURL& url,
    const scoped_refptr<content::SiteInstance>& site_instance,
    const gfx::Size& initial_size) {
  content::WebContents::CreateParams create_params(browser_context,
                                                   site_instance);
  Shell* shell = CreateShell(content::WebContents::Create(create_params),
                             AdjustWindowSize(initial_size));
  if (!url.is_empty()) {
    shell->LoadURL(url);
  }
  return shell;
}

void Shell::LoadURL(const GURL& url) {
  content::NavigationController::LoadURLParams params(url);
  params.transition_type = ui::PageTransitionFromInt(
      ui::PAGE_TRANSITION_TYPED | ui::PAGE_TRANSITION_FROM_ADDRESS_BAR);
  web_contents_->GetController().LoadURLWithParams(params);
}

content::WebContents* Shell::AddNewContents(
    content::WebContents* source,
    std::unique_ptr<content::WebContents> new_contents,
    const GURL& target_url,
    WindowOpenDisposition disposition,
    const blink::mojom::WindowFeatures& window_features,
    bool user_gesture,
    bool* was_blocked) {
  // A document picture-in-picture window tells its controller.
  if (disposition == WindowOpenDisposition::NEW_PICTURE_IN_PICTURE) {
    content::DocumentPictureInPictureWindowController* controller =
        content::PictureInPictureWindowController::
            GetOrCreateDocumentPictureInPictureController(source);
    controller->Close(/*should_pause_video=*/false);
    controller->SetChildWebContents(new_contents.get());
    controller->Show();
  }
  content::WebContents* result = new_contents.get();
  CreateShell(std::move(new_contents),
              AdjustWindowSize(window_features.bounds.size()));
  return result;
}

void Shell::GoBackOrForward(int offset) {
  web_contents_->GetController().GoToOffset(offset);
}

void Shell::Reload() {
  web_contents_->GetController().Reload(content::ReloadType::NORMAL, false);
}

void Shell::ReloadBypassingCache() {
  web_contents_->GetController().Reload(content::ReloadType::BYPASSING_CACHE,
                                        false);
}

void Shell::Stop() {
  web_contents_->Stop();
}

void Shell::UpdateNavigationControls(bool should_show_loading_ui) {
  const int current_index =
      web_contents_->GetController().GetCurrentEntryIndex();
  const int max_index = web_contents_->GetController().GetEntryCount() - 1;
  g_platform->EnableUIControl(this, LrbPlatformDelegate::BACK_BUTTON,
                              current_index > 0);
  g_platform->EnableUIControl(this, LrbPlatformDelegate::FORWARD_BUTTON,
                              current_index < max_index);
  g_platform->EnableUIControl(
      this, LrbPlatformDelegate::STOP_BUTTON,
      should_show_loading_ui && web_contents_->IsLoading());
}

gfx::NativeWindow Shell::window() {
  return g_platform->GetNativeWindow(this);
}

content::WebContents* Shell::OpenURLFromTab(
    content::WebContents* source,
    const content::OpenURLParams& params,
    base::OnceCallback<void(content::NavigationHandle&)>
        navigation_handle_callback) {
  // lrb's own handling first: links opened in the background.
  if (g_platform->OpenURLFromTab(this, source, params)) {
    return nullptr;
  }
  content::WebContents* target = nullptr;
  switch (params.disposition) {
    case WindowOpenDisposition::CURRENT_TAB:
      target = source;
      break;
    case WindowOpenDisposition::NEW_POPUP:
    case WindowOpenDisposition::NEW_WINDOW:
    case WindowOpenDisposition::NEW_BACKGROUND_TAB:
    case WindowOpenDisposition::NEW_FOREGROUND_TAB: {
      // A new page (the platform delegate decides: a tab or a window).
      Shell* new_window = CreateNewWindow(source->GetBrowserContext(),
                                          GURL(), params.source_site_instance,
                                          gfx::Size());
      target = new_window->web_contents();
      break;
    }
    default:  // SINGLETON_TAB, OFF_THE_RECORD, SAVE_TO_DISK, IGNORE_ACTION
      return nullptr;
  }

  base::WeakPtr<content::NavigationHandle> navigation_handle =
      target->GetController().LoadURLWithParams(
          content::NavigationController::LoadURLParams(params));
  if (navigation_handle_callback && navigation_handle) {
    std::move(navigation_handle_callback).Run(*navigation_handle);
  }
  return target;
}

void Shell::LoadingStateChanged(content::WebContents* source,
                                bool should_show_loading_ui) {
  UpdateNavigationControls(should_show_loading_ui);
  g_platform->SetIsLoading(this, source->IsLoading());
}

void Shell::EnterFullscreenModeForTab(
    content::RenderFrameHost* requesting_frame,
    const blink::mojom::FullscreenOptions& options) {
  ToggleFullscreenModeForTab(
      content::WebContents::FromRenderFrameHost(requesting_frame), true);
}

void Shell::ExitFullscreenModeForTab(content::WebContents* web_contents) {
  ToggleFullscreenModeForTab(web_contents, false);
}

void Shell::ToggleFullscreenModeForTab(content::WebContents* web_contents,
                                       bool enter_fullscreen) {
  if (is_fullscreen_ != enter_fullscreen) {
    is_fullscreen_ = enter_fullscreen;
    web_contents->GetPrimaryMainFrame()
        ->GetRenderViewHost()
        ->GetWidget()
        ->SynchronizeVisualProperties();
  }
}

bool Shell::IsFullscreenForTabOrPending(
    const content::WebContents* web_contents) {
  return is_fullscreen_;
}

blink::mojom::DisplayMode Shell::GetDisplayMode(
    const content::WebContents* web_contents) {
  return IsFullscreenForTabOrPending(web_contents)
             ? blink::mojom::DisplayMode::kFullscreen
             : blink::mojom::DisplayMode::kBrowser;
}

void Shell::RequestPointerLock(content::WebContents* web_contents,
                               bool user_gesture,
                               bool last_unlocked_by_target) {
  web_contents->GotResponseToPointerLockRequest(
      blink::mojom::PointerLockResult::kSuccess);
}

void Shell::Close() {
  // The platform may co-opt destruction (e.g. tear the window down first).
  if (!g_platform->DestroyShell(this)) {
    delete this;
  }
}

void Shell::CloseContents(content::WebContents* source) {
  Close();
}

void Shell::BeforeUnloadFired(content::WebContents* tab,
                              bool proceed,
                              bool* proceed_to_fire_unload) {
  *proceed_to_fire_unload = proceed;
}

bool Shell::CanOverscrollContent() {
  return true;
}

void Shell::NavigationStateChanged(content::WebContents* source,
                                   content::InvalidateTypes changed_flags) {
  if (changed_flags & content::INVALIDATE_TYPE_URL) {
    g_platform->SetAddressBarURL(this, source->GetVisibleURL());
  }
}

content::JavaScriptDialogManager* Shell::GetJavaScriptDialogManager(
    content::WebContents* source) {
  if (!dialog_manager_) {
    dialog_manager_ = g_platform->CreateJavaScriptDialogManager(this);
  }
  return dialog_manager_.get();
}

void Shell::FindReply(content::WebContents* web_contents,
                      int request_id,
                      int number_of_matches,
                      const gfx::Rect& selection_rect,
                      int active_match_ordinal,
                      bool final_update) {
  g_platform->FindReply(this, request_id, number_of_matches,
                        active_match_ordinal, final_update);
}

void Shell::RequestMediaAccessPermission(
    content::WebContents* web_contents,
    const content::MediaStreamRequest& request,
    content::MediaResponseCallback callback) {
  g_platform->RequestMediaAccessPermission(this, web_contents, request,
                                           std::move(callback));
}

bool Shell::CheckMediaAccessPermission(
    content::RenderFrameHost* render_frame_host,
    const url::Origin& security_origin,
    blink::mojom::MediaStreamType type) {
  return g_platform->CheckMediaAccessPermission(this, render_frame_host,
                                                security_origin, type);
}

void Shell::RendererUnresponsive(
    content::WebContents* source,
    content::RenderWidgetHost* render_widget_host,
    base::RepeatingClosure hang_monitor_restarter) {
  LOG(WARNING) << "renderer unresponsive";
}

void Shell::ActivateContents(content::WebContents* contents) {
  g_platform->ActivateContents(this, contents);
}

void Shell::RunFileChooser(content::RenderFrameHost* render_frame_host,
                           scoped_refptr<content::FileSelectListener> listener,
                           const blink::mojom::FileChooserParams& params) {
  g_platform->RunFileChooser(render_frame_host, std::move(listener), params);
}

void Shell::EnumerateDirectory(
    content::WebContents* web_contents,
    scoped_refptr<content::FileSelectListener> listener,
    const base::FilePath& path) {
  listener->FileSelectionCanceled();
}

bool Shell::IsBackForwardCacheSupported(content::WebContents& web_contents) {
  return true;
}

content::PreloadingEligibility Shell::IsPrerender2Supported(
    content::WebContents& web_contents,
    content::PreloadingTriggerType trigger_type) {
  // No prerendering: a whole second page in memory on a guess.
  return content::PreloadingEligibility::kPreloadingDisabled;
}

// static
gfx::Size Shell::GetShellDefaultSize() {
  static gfx::Size default_size;  // Computed once.
  if (!default_size.IsEmpty()) {
    return default_size;
  }
  const base::CommandLine& command_line =
      *base::CommandLine::ForCurrentProcess();
  if (command_line.HasSwitch(kHostWindowSize)) {
    const std::string size_str =
        command_line.GetSwitchValueASCII(kHostWindowSize);
    int width, height;
    if (UNSAFE_TODO(sscanf(size_str.c_str(), "%dx%d", &width, &height)) == 2) {
      default_size = gfx::Size(width, height);
    } else {
      LOG(ERROR) << "Invalid size \"" << size_str << "\" given to --"
                 << kHostWindowSize;
    }
  }
  if (default_size.IsEmpty()) {
    default_size = gfx::Size(kDefaultWindowWidthDip, kDefaultWindowHeightDip);
  }
  return default_size;
}

void Shell::TitleWasSet(content::NavigationEntry* entry) {
  if (entry) {
    g_platform->SetTitle(this, entry->GetTitle());
  }
}

}  // namespace lrb
