// Copyright 2026 The low-ram-browser Authors
// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/lrb_content_browser_client.h"

#include <memory>
#include <vector>

#include "base/command_line.h"
#include "base/containers/fixed_flat_set.h"
#include "base/i18n/rtl.h"
#include "base/no_destructor.h"
#include "base/strings/string_util.h"
#include "base/feature_list.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/memory_pressure_listener.h"
#include "base/memory/raw_ptr.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/values.h"
#include "components/embedder_support/user_agent_utils.h"
#include "components/network_hints/browser/simple_network_hints_handler_impl.h"
#include "components/performance_manager/embedder/binders.h"
#include "components/performance_manager/embedder/performance_manager_registry.h"
#include "components/version_info/version_info.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/client_certificate_delegate.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/navigation_throttle_registry.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/speech_recognition_manager_delegate.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/url_constants.h"
#include "lrb/browser/activity_tracker.h"
#include "lrb/browser/blocking_url_loader_factory.h"
#include "lrb/browser/coordinator_client.h"
#include "lrb/browser/devtools_manager_delegate.h"
#include "lrb/browser/list_updater.h"
#include "lrb/browser/lrb_browser_context.h"
#include "lrb/browser/lrb_browser_main_parts.h"
#include "lrb/browser/saved_windows.h"
#include "lrb/browser/shell.h"
#include "lrb/browser/site.h"
#include "lrb/browser/site_launcher.h"
#include "lrb/browser/site_window_throttle.h"
#include "lrb/browser/ui/context_menu.h"
#include "lrb/browser/ui/file_picker.h"
#include "lrb/browser/ui/login_prompt.h"
#include "lrb/browser/ui/lrb_platform_delegate.h"
#include "lrb/common/content_blocker.h"
#include "lrb/common/lrb_switches.h"
#include "lrb/coordinator/rules.h"
#include "net/base/features.h"
#include "net/dns/public/dns_over_https_config.h"
#include "net/dns/public/insecure_dns_mode.h"
#include "net/dns/public/secure_dns_mode.h"
#include "mojo/public/cpp/bindings/binder_map.h"
#include "net/ssl/client_cert_identity.h"
#include "net/ssl/ssl_private_key.h"
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

}  // namespace

LrbContentBrowserClient::LrbContentBrowserClient()
    : site_(base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
          switches::kSite)) {
  g_client = this;
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
                                             bool restore) {
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
    coordinator_->Send(line);
  } else {
    LaunchSiteWindow(site, url, place, back_url, restore);
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

void LrbContentBrowserClient::DiscardPages() {
  for (Shell* window : Shell::windows()) {
    ActivityTracker::CreateForWebContents(window->web_contents(), *this);
    ActivityTracker::FromWebContents(window->web_contents())->Discard();
  }
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
  if (line == "discard") {
    DiscardPages();
    return;
  }
  if (line == "discard-background") {
    // The instance in use, under pressure: its tabs not shown sleep.
    VLOG(1) << "lrb: sleeping background tabs of " << site_;
    LrbPlatformDelegate::DiscardBackgroundTabs();
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
  static constexpr char kShow[] = "show ";
  if (!line.starts_with(kShow)) {
    return;
  }
  // <url> [bounds=<x,y,w,h>] [back=<url>]
  std::string_view args = std::string_view(line).substr(sizeof(kShow) - 1);
  std::optional<coordinator::WindowBounds> bounds;
  GURL back;
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
    }
    rest = end < rest.size() ? rest.substr(end + 1) : "";
  }
  args = args.substr(0, space);
  const GURL url(args);
  // The coordinator checks this too; a window never shows another site.
  if (!url.is_valid() || site_.empty() || SiteForUrl(url) != site_) {
    LOG(WARNING) << "lrb: refusing to show " << url << " in " << site_
                 << "'s window";
    return;
  }
  // Another page of this site: its own window, like a tab. With bounds, it
  // replaces another site's window: in its place.
  if (bounds) {
    LrbPlatformDelegate::SetNextWindowBounds(
        gfx::Rect(bounds->x, bounds->y, bounds->width, bounds->height));
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
  if (disposition != WindowOpenDisposition::NEW_BACKGROUND_TAB ||
      !user_gesture || !target_url.SchemeIsHTTPOrHTTPS() || !opener) {
    return true;
  }
  const std::string target = SiteForUrl(target_url);
  if (target != site_) {
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
    content::GuestPageHolder* guest,
    content::LoginDelegate::LoginAuthRequiredCallback auth_required_callback) {
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
  // content_shell always sent "en-us,en".
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

void LrbContentBrowserClient::ExposeInterfacesToChild(
    mojo::BinderMapWithContext<content::BrowserChildProcessHost*>* map) {
  PerformanceManager().GetBinders().ExposeInterfacesToBrowserChildProcess(map);
}

void LrbContentBrowserClient::RegisterBrowserInterfaceBindersForFrame(
    content::RenderFrameHost* render_frame_host,
    mojo::BinderMapWithContext<content::RenderFrameHost*>* map) {
  PerformanceManager().GetBinders().ExposeInterfacesToRenderFrame(map);
  map->Add<network_hints::mojom::NetworkHintsHandler>(&BindNetworkHintsHandler);
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
}

}  // namespace lrb
