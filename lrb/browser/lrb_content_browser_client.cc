// Copyright 2026 The low-ram-browser Authors
// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/lrb_content_browser_client.h"

#include <memory>
#include <set>
#include <string_view>
#include <vector>

#include "base/command_line.h"
#include "base/containers/fixed_flat_set.h"
#include "base/i18n/rtl.h"
#include "base/no_destructor.h"
#include "base/strings/string_util.h"
#include "base/feature_list.h"
#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/files/scoped_file.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/memory_pressure_listener.h"
#include "base/memory/raw_ptr.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "components/embedder_support/user_agent_utils.h"
#include "components/network_hints/browser/simple_network_hints_handler_impl.h"
#include "components/performance_manager/embedder/binders.h"
#include "components/performance_manager/embedder/performance_manager_registry.h"
#include "components/performance_manager/public/graph/frame_node.h"
#include "components/performance_manager/public/performance_manager.h"
#include "components/version_info/version_info.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/client_certificate_delegate.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/navigation_throttle_registry.h"
#include "content/public/browser/posix_file_descriptor_info.h"
#include "content/public/browser/reload_type.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/spare_render_process_host_manager.h"
#include "content/public/browser/speech_recognition_manager_delegate.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/url_constants.h"
#include "lrb/browser/activity_tracker.h"
#include "lrb/browser/blocking_url_loader_factory.h"
#include "lrb/browser/bookmarks.h"
#include "lrb/browser/certificate_errors.h"
#include "lrb/browser/coordinator_client.h"
#include "lrb/browser/dark_pages.h"
#include "lrb/browser/external_links.h"
#include "lrb/browser/forms.h"
#include "lrb/browser/keyring.h"
#include "lrb/browser/devtools_manager_delegate.h"
#include "lrb/browser/list_updater.h"
#include "lrb/browser/lrb_browser_context.h"
#include "lrb/browser/lrb_browser_main_parts.h"
#include "lrb/browser/print.h"
#include "lrb/browser/saved_windows.h"
#include "lrb/browser/shell.h"
#include "lrb/browser/site.h"
#include "lrb/browser/site_launcher.h"
#include "lrb/browser/safety_throttle.h"
#include "lrb/browser/site_window_throttle.h"
#include "lrb/browser/ui/context_menu.h"
#include "lrb/browser/ui/file_picker.h"
#include "lrb/browser/ui/login_prompt.h"
#include "lrb/browser/ui/lrb_platform_delegate.h"
#include "lrb/browser/ui/snap_connections_dialog.h"
#include "lrb/common/content_blocker.h"
#include "lrb/common/lrb_descriptors.h"
#include "lrb/common/safety_list.h"
#include "lrb/common/lrb_switches.h"
#include "lrb/common/mode.h"
#include "lrb/coordinator/rules.h"
#include "net/base/features.h"
#include "net/dns/public/dns_over_https_config.h"
#include "net/dns/public/insecure_dns_mode.h"
#include "net/dns/public/secure_dns_mode.h"
#include "mojo/public/cpp/bindings/binder_map.h"
#include "net/ssl/client_cert_identity.h"
#include "net/ssl/ssl_private_key.h"
#include "sandbox/policy/switches.h"
#include "services/network/public/cpp/url_loader_factory_builder.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "services/network/public/mojom/network_service.mojom.h"
#include "third_party/blink/public/common/web_preferences/web_preferences.h"
#include "ui/gfx/geometry/size.h"
#include "url/gurl.h"
#include "url/url_constants.h"

namespace lrb {

namespace {

LrbContentBrowserClient* g_client = nullptr;

// content_shell's command line switches lrb keeps (OverrideWebPreferences).
constexpr char kForceDarkMode[] = "force-dark-mode";
constexpr char kForceHighContrast[] = "force-high-contrast";

// Speech recognition (webkitSpeechRecognition) listens to the microphone: it
// is refused, as no permission is granted without asking. (content_shell
// allowed it without asking; without a delegate a request would hang.)
class SpeechRecognitionManagerDelegate
    : public content::SpeechRecognitionManagerDelegate {
 public:
  void CheckRecognitionIsAllowed(
      int session_id,
      base::OnceCallback<void(bool ask_user, bool is_allowed)> callback)
      override {
    content::GetIOThreadTaskRunner({})->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback), /*ask_user=*/false,
                                  /*is_allowed=*/false));
  }
  content::SpeechRecognitionEventListener* GetEventListener() override {
    return nullptr;
  }
  void BindSpeechRecognitionContext(
      mojo::PendingReceiver<media::mojom::SpeechRecognitionContext> receiver,
      const std::string& language,
      const content::GlobalRenderFrameHostId& render_frame_host_id) override {}
};

void BindNetworkHintsHandler(
    content::RenderFrameHost* frame_host,
    mojo::PendingReceiver<network_hints::mojom::NetworkHintsHandler> receiver) {
  network_hints::SimpleNetworkHintsHandlerImpl::Create(frame_host,
                                                       std::move(receiver));
}

performance_manager::PerformanceManagerRegistry& PerformanceManager() {
  return *performance_manager::PerformanceManagerRegistry::GetInstance();
}

// Ends the spare renderers, started ahead for the next new process: they
// were launched with what the browser knew then. The next one starts with
// what it knows now.
void DropSpareRenderers() {
  std::vector<content::RenderProcessHost*> spares;
  for (content::RenderProcessHost* spare :
       content::SpareRenderProcessHostManager::Get().GetSpares()) {
    spares.push_back(spare);
  }
  for (content::RenderProcessHost* spare : spares) {
    spare->FastShutdownIfPossible(/*page_count=*/0,
                                  /*skip_unload_handlers=*/true);
  }
}

}  // namespace

LrbContentBrowserClient::LrbContentBrowserClient()
    : standard_(StandardMode()),
      site_(standard_ ? std::string()
                      : base::CommandLine::ForCurrentProcess()
                            ->GetSwitchValueASCII(switches::kSite)) {
  g_client = this;
  base::AutoLock lock(engine_path_lock_);
  engine_path_ = base::CommandLine::ForCurrentProcess()->GetSwitchValuePath(
      switches::kAdblockFile);
}

LrbContentBrowserClient::~LrbContentBrowserClient() {
  g_client = nullptr;
}

// static
LrbContentBrowserClient* LrbContentBrowserClient::Get() {
  return g_client;
}

void LrbContentBrowserClient::SetSite(const std::string& site) {
  DCHECK(site_.empty());
  site_ = site;
  VLOG(1) << "lrb: this instance is for site " << site_;
  if (coordinator_) {
    coordinator_->Send("site " + site_);
  }
}

void LrbContentBrowserClient::OpenSiteWindow(const std::string& site,
                                             const GURL& url,
                                             std::optional<gfx::Rect> bounds,
                                             const GURL& back,
                                             bool restore,
                                             uint64_t handoff) {
  const std::string place =
      bounds ? coordinator::FormatBounds({bounds->x(), bounds->y(),
                                          bounds->width(), bounds->height()})
             : std::string();
  // Only http(s) pages are worth coming back to (not about:blank).
  const std::string back_url =
      back.SchemeIsHTTPOrHTTPS() ? back.spec() : std::string();
  if (coordinator_) {
    std::string line = "open " + site + " " + url.spec();
    if (!place.empty()) {
      line += " bounds=" + place;
    }
    if (!back_url.empty()) {
      line += " back=" + back_url;
    }
    if (restore) {
      line += " restore";
    }
    if (handoff) {
      line += " handoff=" + base::NumberToString(handoff);
    }
    coordinator_->Send(line);
  } else {
    LaunchSiteWindow(site, url, place, back_url, restore);
  }
}

uint64_t LrbContentBrowserClient::StartHandoff(content::WebContents* leaving) {
  const uint64_t handoff = ++last_handoff_;
  handoffs_[handoff] = leaving->GetWeakPtr();
  return handoff;
}

void LrbContentBrowserClient::ReportShownOnNextWindow(uint64_t handoff) {
  if (!coordinator_) {
    return;  // without one, the window left hides on a timer
  }
  LrbPlatformDelegate::SetNextWindowShown(base::BindOnce(
      &LrbContentBrowserClient::SendToCoordinator, base::Unretained(this),
      "shown " + base::NumberToString(handoff)));
}

void LrbContentBrowserClient::ReportWindowsHidden(bool all_hidden) {
  if (all_hidden == reported_parked_) {
    return;
  }
  reported_parked_ = all_hidden;
  VLOG(1) << "lrb: " << (all_hidden ? "parked" : "shown again");
  SendToCoordinator(all_hidden ? "parked 1" : "parked 0");
}

void LrbContentBrowserClient::ReloadSiteBypassingCache(
    const std::string& site) {
  if (site.empty()) {
    return;
  }
  // The list survives restarts: the browser reads it at startup
  // (lrb_main_delegate.cc).
  if (LrbBrowserContext* context = browser_context()) {
    base::ThreadPool::PostTask(
        FROM_HERE,
        {base::MayBlock(), base::TaskShutdownBehavior::BLOCK_SHUTDOWN},
        base::BindOnce(
            [](base::FilePath file) {
              if (!ContentBlocker::WriteDisabledSites(file)) {
                LOG(WARNING) << "lrb: can't write " << file;
              }
            },
            context->GetPath().Append(kBlockingOffSitesFile)));
  }
  // The renderers that show the site's frames were started with the old
  // list, and site isolation keeps the site's frames in them: they end, so
  // the reloads below start new ones with the new list. Pages of other
  // sites with one of its frames reload too (the frame went with them).
  std::set<content::RenderProcessHost*> processes;
  for (Shell* window : Shell::windows()) {
    window->web_contents()->ForEachRenderFrameHost(
        [&](content::RenderFrameHost* frame) {
          if (SiteForUrl(frame->GetLastCommittedOrigin().GetURL()) == site) {
            processes.insert(frame->GetProcess());
          }
        });
  }
  std::vector<Shell*> reload;
  for (Shell* window : Shell::windows()) {
    bool affected =
        SiteForUrl(window->web_contents()->GetLastCommittedURL()) == site;
    window->web_contents()->ForEachRenderFrameHost(
        [&](content::RenderFrameHost* frame) {
          affected |= processes.contains(frame->GetProcess());
        });
    if (affected) {
      reload.push_back(window);
    }
  }
  for (content::RenderProcessHost* process : processes) {
    process->FastShutdownIfPossible(/*page_count=*/0,
                                    /*skip_unload_handlers=*/true);
  }
  // A spare renderer, started ahead for the next new process, has the old
  // list too.
  DropSpareRenderers();
  for (Shell* window : reload) {
    window->web_contents()->GetController().Reload(
        content::ReloadType::BYPASSING_CACHE, /*check_for_repost=*/false);
  }
}

void LrbContentBrowserClient::ReportBackgroundTabs(int count) {
  if (coordinator_ && count != reported_background_tabs_) {
    reported_background_tabs_ = count;
    coordinator_->Send("background-tabs " + base::NumberToString(count));
  }
}

void LrbContentBrowserClient::ReportActive() {
  const base::TimeTicks now = base::TimeTicks::Now();
  if (coordinator_ && now - last_active_report_ >= base::Seconds(1)) {
    last_active_report_ = now;
    VLOG(1) << "lrb: " << site_ << " active";
    coordinator_->Send("active");
  }
}

void LrbContentBrowserClient::DiscardPages(bool critical) {
  for (Shell* window : Shell::windows()) {
    // A page that would ask before leaving may hold what the user typed;
    // only critical pressure drops it.
    if (!critical && HasUnsavedInput(window->web_contents())) {
      continue;
    }
    ActivityTracker::CreateForWebContents(window->web_contents(), *this);
    ActivityTracker::FromWebContents(window->web_contents())->Discard();
  }
}

// static
bool LrbContentBrowserClient::HasUnsavedInput(content::WebContents* contents) {
  bool typed = false;
  contents->ForEachRenderFrameHost([&](content::RenderFrameHost* frame) {
    const base::WeakPtr<performance_manager::FrameNode> node =
        performance_manager::PerformanceManager::GetFrameNodeForRenderFrameHost(
            frame);
    typed = typed || (node && node->HadFormInteraction());
  });
  return typed;
}

void LrbContentBrowserClient::OnPageDiscarded() {
  // The dropped page's JavaScript heap and freed allocations stay resident in
  // this single process until collected and returned. A critical pressure
  // signal collects (full V8 GC, cache drops); a second one, once that has
  // settled, returns the memory. Measured on GitHub (official build): 132 MB
  // on the blank page, 84 MB after the first signal, 48 MB after the second
  // 10 s later; a blank instance is 29 MB.
  auto notify = [] {
    base::MemoryPressureListener::NotifyMemoryPressure(
        base::MEMORY_PRESSURE_LEVEL_CRITICAL);
  };
  for (int seconds : {1, 11}) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, base::BindOnce(notify), base::Seconds(seconds));
  }
}

void LrbContentBrowserClient::CloseAllWindowsSoon() {
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce([] {
        // Tabs not loaded first (else closing the last loaded one would
        // load them). Closing removes from the list (a copy would dangle).
        LrbPlatformDelegate::DropUnloadedTabs();
        while (!Shell::windows().empty()) {
          Shell::windows().back()->Close();
        }
      }));
}

void LrbContentBrowserClient::ConnectToCoordinator() {
  const base::FilePath socket_path =
      base::CommandLine::ForCurrentProcess()->GetSwitchValuePath(
          switches::kCoordinator);
  if (socket_path.empty()) {
    return;
  }
  coordinator_ = CoordinatorClient::Connect(
      socket_path,
      base::BindRepeating(&LrbContentBrowserClient::OnCoordinatorLine,
                          base::Unretained(this)));
  if (coordinator_ && !site_.empty()) {
    coordinator_->Send("site " + site_);
  }
}

void LrbContentBrowserClient::SendToCoordinator(const std::string& line) {
  if (coordinator_) {
    coordinator_->Send(line);
  }
}

void LrbContentBrowserClient::OnCoordinatorLine(const std::string& line) {
  if (OnBrokerLine(line)) {
    return;  // files the user picked (ui/file_picker.h)
  }
  if (OnBookmarksLine(line)) {
    return;  // bookmarks.h
  }
  if (OnKeyringLine(line)) {
    return;  // saved passwords and autofill (keyring.h)
  }
  if (OnSnapPlugsLine(line)) {
    return;  // the snap's connections (ui/snap_connections_dialog.h)
  }
  // "<command> [<argument>]"
  const size_t space = line.find(' ');
  const std::string_view command = std::string_view(line).substr(0, space);
  const std::string_view argument =
      space == std::string::npos ? std::string_view()
                                 : std::string_view(line).substr(space + 1);
  if (command == "discard") {
    // Bare "discard": critical (what older coordinators sent).
    DiscardPages(argument != "moderate");
    return;
  }
  if (command == "discard-background") {
    // The instance in use, under pressure: its tabs not shown sleep.
    VLOG(1) << "lrb: sleeping background tabs of " << site_;
    LrbPlatformDelegate::DiscardBackgroundTabs(argument != "moderate");
    return;
  }
  if (command == "hide") {
    // The other site's window is on screen: the one that left for it hides.
    // Its pages stay; Back from there shows it again.
    uint64_t handoff = 0;
    if (!base::StringToUint64(argument, &handoff)) {
      return;
    }
    auto it = handoffs_.find(handoff);
    if (it == handoffs_.end()) {
      return;
    }
    base::WeakPtr<content::WebContents> left = it->second;
    handoffs_.erase(it);
    if (left) {
      LrbPlatformDelegate::HideWindowOf(left.get());
    }
    return;
  }
  if (command == "adblock-file") {
    // The lists were just built; this instance started without them. Once
    // loaded, requests are checked (WillCreateURLLoaderFactory sees it for
    // factories created from then on). An engine already loaded stays: it
    // is never freed, and the next start maps the new file.
    if (!argument.empty() && !ContentBlocker::loaded()) {
      {
        // Renderers launched from now on get it as a descriptor.
        base::AutoLock lock(engine_path_lock_);
        if (engine_path_.empty()) {
          engine_path_ = base::FilePath(argument);
        }
      }
      // Without it (no descriptor): the next new site's page would get it.
      DropSpareRenderers();
      base::ThreadPool::PostTask(
          FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
          base::BindOnce(
              [](const base::FilePath& path) { ContentBlocker::Load(path); },
              base::FilePath(argument)));
    }
    return;
  }
  if (line == "close") {
    // Last resort under memory pressure: save the windows' history, exit.
    // Opening the site again restores them (RestoreSavedWindows).
    VLOG(1) << "lrb: closing " << site_ << " to free memory";
    KeepSession();  // the windows go now, but come back
    SaveWindows(browser_context(),
                base::BindOnce(&LrbContentBrowserClient::CloseAllWindowsSoon,
                               base::Unretained(this)));
    return;
  }
  if (command == "show") {
    Show(argument);
  }
}

void LrbContentBrowserClient::Show(std::string_view args) {
  // <url> [bounds=<x,y,w,h>] [back=<url>] [handoff=<n>]
  std::optional<coordinator::WindowBounds> bounds;
  GURL back;
  uint64_t handoff = 0;
  const size_t space = args.find(' ');
  for (std::string_view rest =
           space == std::string_view::npos ? "" : args.substr(space + 1);
       !rest.empty();) {
    const size_t end = std::min(rest.find(' '), rest.size());
    const std::string_view option = rest.substr(0, end);
    if (option.starts_with("bounds=")) {
      bounds = coordinator::ParseBounds(option.substr(7));
    } else if (option.starts_with("back=")) {
      back = GURL(option.substr(5));
    } else if (option.starts_with("handoff=")) {
      if (!base::StringToUint64(option.substr(8), &handoff)) {
        handoff = 0;
      }
    }
    rest = end < rest.size() ? rest.substr(end + 1) : "";
  }
  const GURL url(args.substr(0, space));
  // Lean mode: the coordinator checks this too; a window never shows
  // another site. Standard mode shows every site.
  if (!url.is_valid() ||
      (!standard_ && (site_.empty() || SiteForUrl(url) != site_))) {
    LOG(WARNING) << "lrb: refusing to show " << url << " in " << site_
                 << "'s window";
    return;
  }
  std::optional<gfx::Rect> place;
  if (bounds) {
    place = gfx::Rect(bounds->x, bounds->y, bounds->width, bounds->height);
  }
  // A tab already showing the page (Back to a parked window, the site
  // opened again): to the front, nothing new opens.
  if (LrbPlatformDelegate::ShowExisting(url, place)) {
    if (handoff) {
      SendToCoordinator("shown " + base::NumberToString(handoff));
    }
    return;
  }
  if (handoff) {
    ReportShownOnNextWindow(handoff);
  }
  // Another page of this site: its own window, like a tab. With bounds, it
  // replaces another site's window: in its place.
  if (place) {
    LrbPlatformDelegate::SetNextWindowBounds(*place);
  }
  LrbPlatformDelegate::SetNextWindowBackUrl(back);
  Shell::CreateNewWindow(browser_context(), url, nullptr, gfx::Size());
}

void LrbContentBrowserClient::WillCreateURLLoaderFactory(
    content::BrowserContext* browser_context,
    content::RenderFrameHost* frame,
    int render_process_id,
    URLLoaderFactoryType type,
    const url::Origin& request_initiator,
    const net::IsolationInfo& isolation_info,
    std::optional<int64_t> navigation_id,
    ukm::SourceIdObj ukm_source_id,
    network::URLLoaderFactoryBuilder& factory_builder,
    mojo::PendingRemote<network::mojom::TrustedURLLoaderHeaderClient>*
        header_client,
    bool* bypass_redirect_checks,
    bool* disable_secure_dns,
    network::mojom::URLLoaderFactoryOverridePtr* factory_override,
    scoped_refptr<base::SequencedTaskRunner> navigation_response_task_runner,
    bool is_for_network_service) {
  // The list updater catches its scriptlet page's result.
  ListUpdater::MaybeInterceptRequests(factory_builder);
  if (!ContentBlocker::loaded() || type == URLLoaderFactoryType::kDownload) {
    return;
  }
  // Every request of the page or worker goes through content blocking
  // (navigations too: ad iframes are subframe navigations; top-level pages
  // are never blocked).
  auto [receiver, target] = factory_builder.Append();
  BlockingURLLoaderFactory::Create(
      request_initiator.GetURL(),
      frame ? frame->GetGlobalId() : content::GlobalRenderFrameHostId(),
      std::move(receiver), std::move(target));
}

bool LrbContentBrowserClient::CanCreateWindow(
    content::RenderFrameHost* opener,
    const GURL& opener_url,
    const GURL& opener_top_level_frame_url,
    const url::Origin& source_origin,
    content::mojom::WindowContainerType container_type,
    const GURL& target_url,
    const content::Referrer& referrer,
    const std::string& frame_name,
    WindowOpenDisposition disposition,
    const blink::mojom::WindowFeatures& features,
    bool user_gesture,
    bool opener_suppressed,
    bool* no_javascript_access) {
  *no_javascript_access = false;
  // Every disposition here opens a window or a tab. Without a gesture it's
  // a page opening windows on its own: ads, pop-unders.
  if (!user_gesture) {
    VLOG(1) << "lrb: blocked a window nobody asked for: " << target_url;
    return false;
  }
  if (disposition != WindowOpenDisposition::NEW_BACKGROUND_TAB ||
      !target_url.SchemeIsHTTPOrHTTPS() || !opener) {
    return true;
  }
  const std::string target = SiteForUrl(target_url);
  if (!standard_ && target != site_) {
    OpenSiteWindow(target, target_url);  // another site: its own window
    return false;
  }
  Shell* shell = Shell::FromWebContents(
      content::WebContents::FromRenderFrameHost(opener));
  return !(shell && LrbPlatformDelegate::AddBackgroundTab(shell, target_url));
}

std::unique_ptr<content::LoginDelegate>
LrbContentBrowserClient::CreateLoginDelegate(
    const net::AuthChallengeInfo& auth_info,
    content::WebContents* web_contents,
    content::BrowserContext* browser_context,
    const content::GlobalRequestID& request_id,
    bool is_request_for_primary_main_frame_navigation,
    bool is_request_for_navigation,
    const GURL& url,
    scoped_refptr<net::HttpResponseHeaders> response_headers,
    bool first_auth_attempt,
    bool do_not_prompt_for_login,
    content::GuestPageHolder* guest,
    content::LoginDelegate::LoginAuthRequiredCallback auth_required_callback) {
  // Requests that must not show a sign-in question (some proxy challenges,
  // since M157): lrb has no credentials but the ones the user types, so the
  // challenge is cancelled.
  if (do_not_prompt_for_login) {
    return nullptr;
  }
  return AskToSignIn(auth_info, web_contents,
                     is_request_for_primary_main_frame_navigation, url,
                     std::move(auth_required_callback));
}

std::unique_ptr<content::WebContentsViewDelegate>
LrbContentBrowserClient::GetWebContentsViewDelegate(
    content::WebContents* web_contents) {
  return std::make_unique<LrbWebContentsViewDelegate>(web_contents);
}

std::unique_ptr<content::BrowserMainParts>
LrbContentBrowserClient::CreateBrowserMainParts(bool is_integration_test) {
  auto parts = std::make_unique<LrbBrowserMainParts>(*this);
  main_parts_ = parts.get();
  return parts;
}

void LrbContentBrowserClient::CreateThrottlesForNavigation(
    content::NavigationThrottleRegistry& registry) {
  // Every page navigates before it matters, so this reaches all of them.
  ActivityTracker::CreateForWebContents(
      registry.GetNavigationHandle().GetWebContents(), *this);
  registry.AddThrottle(std::make_unique<SiteWindowThrottle>(registry, *this));
  // After it: a page another site's window will open is checked there.
  if (SafetyList::loaded()) {
    registry.AddThrottle(std::make_unique<SafetyThrottle>(registry));
  }
}

LrbBrowserContext* LrbContentBrowserClient::browser_context() {
  return main_parts_ ? main_parts_->browser_context() : nullptr;
}

// static
std::string LrbContentBrowserClient::UserAgent() {
  if (const auto custom_ua = embedder_support::GetUserAgentFromCommandLine()) {
    return *custom_ua;
  }
  return embedder_support::BuildUnifiedPlatformUserAgentFromProduct(
      base::StrCat({"Chrome/", version_info::GetMajorVersionNumber(),
                    ".0.0.0"}));
}

// static
blink::UserAgentMetadata LrbContentBrowserClient::UserAgentMetadata() {
  // Chromium's (brand "Chromium", platform "Linux"), what sites see from any
  // Chromium build; content_shell said "content_shell" on "Unknown".
  return embedder_support::GetUserAgentMetadata();
}

bool LrbContentBrowserClient::IsHandledURL(const GURL& url) {
  if (!url.is_valid()) {
    return false;
  }
  static constexpr auto kProtocols = base::MakeFixedFlatSet<std::string_view>({
      url::kHttpScheme,
      url::kHttpsScheme,
      url::kWsScheme,
      url::kWssScheme,
      url::kBlobScheme,
      url::kFileSystemScheme,
      content::kChromeUIScheme,
      content::kChromeUIUntrustedScheme,
      content::kChromeDevToolsScheme,
      url::kDataScheme,
      url::kFileScheme,
  });
  return kProtocols.contains(url.scheme());
}

std::string LrbContentBrowserClient::GetAcceptLangs(
    content::BrowserContext* context) {
  // The system's language (LANG, LC_ALL...), then English: "es-CL,es,en-US,en".
  // content_shell always sent "en-us,en". ICU's default locale is the
  // system's because LrbMainDelegate::PreSandboxStartup sets it.
  static const base::NoDestructor<std::string> langs([] {
    std::vector<std::string> list;
    auto add = [&list](const std::string& lang) {
      if (!lang.empty() && std::ranges::find(list, lang) == list.end()) {
        list.push_back(lang);
      }
    };
    std::string locale = base::i18n::GetConfiguredLocale();
    std::ranges::replace(locale, '_', '-');
    if (locale != "C" && locale != "POSIX") {
      add(locale);
      add(locale.substr(0, locale.find('-')));
    }
    add("en-US");
    add("en");
    return base::JoinString(list, ",");
  }());
  return *langs;
}

std::string LrbContentBrowserClient::GetDefaultDownloadName() {
  return "download";
}

content::GeneratedCodeCacheSettings
LrbContentBrowserClient::GetGeneratedCodeCacheSettings(
    content::BrowserContext* context) {
  return content::GeneratedCodeCacheSettings(true, 0, context->GetPath());
}

base::OnceClosure LrbContentBrowserClient::SelectClientCertificate(
    content::BrowserContext* browser_context,
    int process_id,
    content::WebContents* web_contents,
    net::SSLCertRequestInfo* cert_request_info,
    net::ClientCertIdentityList client_certs,
    std::unique_ptr<content::ClientCertificateDelegate> delegate) {
  // lrb has no client certificates: continue without one, as Chrome does when
  // the user has none (sites that only offer certificate sign-in still load).
  // content_shell dropped the delegate, which cancels: the page failed.
  delegate->ContinueWithCertificate(nullptr, nullptr);
  return base::OnceClosure();
}

content::SpeechRecognitionManagerDelegate*
LrbContentBrowserClient::CreateSpeechRecognitionManagerDelegate() {
  return new SpeechRecognitionManagerDelegate();
}

void LrbContentBrowserClient::OverrideWebPreferences(
    content::WebContents* web_contents,
    content::SiteInstance& main_frame_site,
    blink::web_pref::WebPreferences* prefs) {
  // Content follows the desktop's light or dark theme; content_shell forced
  // light. These switches still force one.
  const base::CommandLine& command_line =
      *base::CommandLine::ForCurrentProcess();
  if (command_line.HasSwitch(kForceDarkMode)) {
    prefs->preferred_color_scheme = blink::mojom::PreferredColorScheme::kDark;
  }
  if (command_line.HasSwitch(kForceHighContrast)) {
    prefs->in_forced_colors = true;
    prefs->preferred_contrast = blink::mojom::PreferredContrast::kMore;
  }
  // The site's choice (the menu): per site in standard mode too. Computed
  // before the first page commits (an empty URL: off), then kept in line
  // with each committed page below.
  ApplyDarkPages(prefs, web_contents->GetLastCommittedURL());
}

bool LrbContentBrowserClient::OverrideWebPreferencesAfterNavigation(
    content::WebContents* web_contents,
    content::SiteInstance& main_frame_site,
    blink::web_pref::WebPreferences* prefs) {
  return RefreshDarkPages(prefs, web_contents->GetLastCommittedURL());
}

std::unique_ptr<content::DevToolsManagerDelegate>
LrbContentBrowserClient::CreateDevToolsManagerDelegate() {
  return std::make_unique<LrbDevToolsManagerDelegate>(browser_context());
}

void LrbContentBrowserClient::ExposeInterfacesToRenderer(
    service_manager::BinderRegistry* registry,
    blink::AssociatedInterfaceRegistry* associated_registry,
    content::RenderProcessHost* render_process_host) {
  PerformanceManager().CreateProcessNode(render_process_host);
  PerformanceManager().GetBinders().ExposeInterfacesToRendererProcess(
      registry, render_process_host);
}

void LrbContentBrowserClient::AppendExtraCommandLineSwitches(
    base::CommandLine* command_line,
    int child_process_id) {
  const base::CommandLine& browser = *base::CommandLine::ForCurrentProcess();
  // Every child applies the same defaults as the browser
  // (lrb_main_delegate.cc): a standard-mode zygote must not take lean
  // mode's --no-sandbox.
  if (browser.HasSwitch(switches::kMode)) {
    command_line->AppendSwitchASCII(switches::kMode,
                                    browser.GetSwitchValueASCII(switches::kMode));
  }
  // Renderers run the cosmetic rules and scriptlets. The sandboxed zygote
  // loads the engine before its sandbox starts, and the renderers it forks
  // share it; the unsandboxed zygote forks no renderers.
  const std::string type =
      command_line->GetSwitchValueASCII(::switches::kProcessType);
  const bool renderers =
      type == ::switches::kRendererProcess ||
      (type == ::switches::kZygoteProcess &&
       !command_line->HasSwitch(sandbox::policy::switches::kNoZygoteSandbox));
  if (!renderers) {
    return;
  }
  if (browser.HasSwitch(switches::kAdblockFile)) {
    command_line->AppendSwitchPath(
        switches::kAdblockFile, browser.GetSwitchValuePath(switches::kAdblockFile));
  }
  if (standard_) {
    const std::vector<std::string> off = ContentBlocker::DisabledSites();
    if (!off.empty()) {
      command_line->AppendSwitchASCII(switches::kBlockingOffSites,
                                      base::JoinString(off, ","));
    }
  }
}

void LrbContentBrowserClient::GetAdditionalMappedFilesForChildProcess(
    const base::CommandLine& command_line,
    int child_process_id,
    content::PosixFileDescriptorInfo* mappings) {
  if (command_line.GetSwitchValueASCII(::switches::kProcessType) !=
      ::switches::kRendererProcess) {
    return;
  }
  base::FilePath path;
  {
    base::AutoLock lock(engine_path_lock_);
    path = engine_path_;
  }
  if (path.empty()) {
    return;  // no lists yet
  }
  // A renderer whose zygote mapped the engine at its start ignores it
  // (LrbContentRendererClient::RenderThreadStarted). Opened on each launch:
  // the launcher thread may block, and the mappings own the descriptor until
  // the child has it.
  base::File file(path, base::File::FLAG_OPEN | base::File::FLAG_READ);
  if (!file.IsValid()) {
    LOG(WARNING) << "lrb: can't open " << path << " for a renderer: "
                 << base::File::ErrorToString(file.error_details());
    return;
  }
  mappings->Transfer(kLrbEngineDescriptor,
                     base::ScopedFD(file.TakePlatformFile()));
  // The scriptlets come with the resources file (none if the lists were
  // built without them).
  base::File resources(path.AddExtension(FILE_PATH_LITERAL("resources.json")),
                       base::File::FLAG_OPEN | base::File::FLAG_READ);
  if (resources.IsValid()) {
    mappings->Transfer(kLrbResourcesDescriptor,
                       base::ScopedFD(resources.TakePlatformFile()));
  }
}

void LrbContentBrowserClient::ExposeInterfacesToChild(
    mojo::BinderMapWithContext<content::BrowserChildProcessHost*>* map) {
  PerformanceManager().GetBinders().ExposeInterfacesToBrowserChildProcess(map);
}

void LrbContentBrowserClient::RegisterBrowserInterfaceBindersForFrame(
    content::RenderFrameHost* render_frame_host,
    mojo::BinderMapWithContext<content::RenderFrameHost*>* map) {
  PerformanceManager().GetBinders().ExposeInterfacesToRenderFrame(map);
  map->Add<network_hints::mojom::NetworkHintsHandler>(&BindNetworkHintsHandler);
  map->Add<mojom::Forms>(&BindForms);
}

void LrbContentBrowserClient::OpenURL(
    content::SiteInstance* site_instance,
    const content::OpenURLParams& params,
    base::OnceCallback<void(content::WebContents*)> callback) {
  std::move(callback).Run(
      Shell::CreateNewWindow(site_instance->GetBrowserContext(), params.url,
                             nullptr, gfx::Size())
          ->web_contents());
}

base::DictValue LrbContentBrowserClient::GetNetLogConstants() {
  base::DictValue client_constants;
  client_constants.Set("name", "lrb");
  client_constants.Set(
      "command_line",
      base::CommandLine::ForCurrentProcess()->GetCommandLineString());
  base::DictValue constants;
  constants.Set("clientInfo", std::move(client_constants));
  return constants;
}

base::FilePath
LrbContentBrowserClient::GetSandboxedStorageServiceDataDirectory() {
  return browser_context()->GetPath();
}

base::FilePath LrbContentBrowserClient::GetFirstPartySetsDirectory() {
  return browser_context()->GetPath();
}

std::optional<base::FilePath>
LrbContentBrowserClient::GetLocalTracesDirectory() {
  return browser_context()->GetPath();
}

std::string LrbContentBrowserClient::GetUserAgent() {
  return UserAgent();
}

blink::UserAgentMetadata LrbContentBrowserClient::GetUserAgentMetadata() {
  return UserAgentMetadata();
}

void LrbContentBrowserClient::OnNetworkServiceCreated(
    network::mojom::NetworkService* network_service) {
  // Chromium's own DNS resolver, DNS over HTTPS when the system's server
  // offers it.
  if (base::FeatureList::IsEnabled(net::features::kAsyncDns)) {
    network_service->ConfigureStubHostResolver(
        net::InsecureDnsMode::kEnabledBuiltIn,
        base::FeatureList::IsEnabled(net::features::kHappyEyeballsV3),
        net::SecureDnsMode::kAutomatic, net::DnsOverHttpsConfig(),
        /*additional_dns_types_enabled=*/true,
        /*fallback_doh_nameservers=*/{});
  }
}

void LrbContentBrowserClient::ConfigureNetworkContextParams(
    content::BrowserContext* context,
    bool in_memory,
    const base::FilePath& relative_partition_path,
    network::mojom::NetworkContextParams* network_context_params,
    cert_verifier::mojom::CertVerifierCreationParams*
        cert_verifier_creation_params) {
  network_context_params->user_agent = GetUserAgent();
  network_context_params->accept_language = GetAcceptLangs(context);
  network_context_params->enable_zstd = true;
  network_context_params->device_bound_sessions_enabled =
      base::FeatureList::IsEnabled(net::features::kDeviceBoundSessions);
  // Network state on disk, in the site's profile: content_shell kept it
  // in memory only.
  if (in_memory || !relative_partition_path.empty()) {
    return;
  }
  network_context_params->file_paths =
      network::mojom::NetworkContextFilePaths::New();
  network_context_params->file_paths->data_directory = context->GetPath();
  // Cookies: in memory, every site was signed out at each start. Session
  // cookies too are kept and restored at every start, as Chrome does with
  // "continue where you left off": a lean instance the coordinator closed,
  // that crashed, or that Back started again comes back signed in.
  // Unencrypted: Chromium 155 has no fallback without a key
  // provider (NOTREACHED), and on Linux without a keyring Chrome's key is a
  // fixed, public one anyway. The profile is the user's alone (0700) and
  // other sites' confined processes can't reach it.
  network_context_params->file_paths->cookie_database_name =
      base::FilePath(FILE_PATH_LITERAL("Cookies"));
  network_context_params->enable_encrypted_cookies = false;
  network_context_params->persist_session_cookies = true;
  network_context_params->restore_old_session_cookies = true;
  // The HTTP cache: in memory it held up to 2% of RAM per site (50 MB from
  // 2.5 GB up), anonymous memory the kernel can't drop, and started empty
  // each time. 32 MB holds a site's scripts, styles and images; small, for
  // SD cards' sake.
  network_context_params->file_paths->http_cache_directory =
      context->GetPath().Append(FILE_PATH_LITERAL("Cache"));
  network_context_params->http_cache_max_size = 32 * 1024 * 1024;
  // HSTS learnt from sites ("HTTPS only from now on"): remembered, so the
  // first request after a restart can't be downgraded to plain HTTP.
  network_context_params->file_paths->transport_security_persister_file_name =
      base::FilePath(FILE_PATH_LITERAL("TransportSecurity"));
}

void LrbContentBrowserClient::AllowCertificateError(
    content::WebContents* web_contents,
    int cert_error,
    const net::SSLInfo& ssl_info,
    const GURL& request_url,
    bool is_primary_main_frame_request,
    bool strict_enforcement,
    base::OnceCallback<void(content::CertificateRequestResultType)> callback) {
  if (!web_contents || !is_primary_main_frame_request) {
    std::move(callback).Run(content::CERTIFICATE_REQUEST_RESULT_TYPE_DENY);
    return;
  }
  AskAboutCertificateError(web_contents, cert_error, request_url,
                           strict_enforcement, std::move(callback));
}

bool LrbContentBrowserClient::HandleExternalProtocol(
    const GURL& url,
    base::RepeatingCallback<content::WebContents*()> web_contents_getter,
    content::FrameTreeNodeId frame_tree_node_id,
    content::NavigationUIData* navigation_data,
    bool is_primary_main_frame,
    bool is_in_fenced_frame_tree,
    network::mojom::WebSandboxFlags sandbox_flags,
    ui::PageTransition page_transition,
    bool has_user_gesture,
    const std::optional<url::Origin>& initiating_origin,
    content::RenderFrameHost* initiator_document,
    const net::IsolationInfo& isolation_info,
    mojo::PendingRemote<network::mojom::URLLoaderFactory>* out_factory) {
  if (content::WebContents* contents = web_contents_getter.Run()) {
    AskToOpenInApp(contents, url, has_user_gesture,
                   is_primary_main_frame && !is_in_fenced_frame_tree &&
                       sandbox_flags == network::mojom::WebSandboxFlags::kNone);
  }
  // Cancelled without an error page: the question in the window is all
  // the page shows.
  return true;
}

std::vector<base::FilePath>
LrbContentBrowserClient::GetNetworkContextsParentDirectory() {
  return {browser_context()->GetPath()};
}

bool LrbContentBrowserClient::HasErrorPage(int http_status_code) {
  return http_status_code >= 400 && http_status_code < 600;
}

void LrbContentBrowserClient::OnWebContentsCreated(
    content::WebContents* web_contents) {
  PerformanceManager().MaybeCreatePageNodeForWebContents(web_contents);
  SetUpPrinting(web_contents);
}

void LrbContentBrowserClient::
    RegisterAssociatedInterfaceBindersForRenderFrameHost(
        content::RenderFrameHost& render_frame_host,
        blink::AssociatedInterfaceRegistry& associated_registry) {
  RegisterPrintingInterface(render_frame_host, associated_registry);
}

}  // namespace lrb
