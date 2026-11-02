// Copyright 2026 The low-ram-browser Authors
// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_LRB_CONTENT_BROWSER_CLIENT_H_
#define LRB_BROWSER_LRB_CONTENT_BROWSER_CLIENT_H_

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "content/public/browser/content_browser_client.h"
#include "third_party/blink/public/common/user_agent/user_agent_metadata.h"
#include "ui/gfx/geometry/rect.h"

class GURL;

namespace base {
class CommandLine;
}

namespace content {
class WebContents;
}

namespace lrb {

class CoordinatorClient;
class LrbBrowserContext;
class LrbBrowserMainParts;

// What the browser does for content: the one-site-per-instance rule (lean
// mode), the connection to lrb_coordinator, content blocking, windows,
// sign-in, the network's settings. From content_shell's
// ShellContentBrowserClient without its web-test and browser-test hooks.
//
// Lean mode: the instance shows one site; another site's pages open in that
// site's instance. Leaving a site parks the instance: its window hides once
// the other site's window is on screen (the coordinator's hand-off: "open
// ... handoff=<n>", then "hide <n>"), and it stays alive with its pages
// until Back shows it again or the coordinator needs the memory.
//
// Standard mode (lrb/common/mode.h): one instance for every site, with
// Chromium's sandboxed renderers and site isolation; no site rule anywhere.
class LrbContentBrowserClient : public content::ContentBrowserClient {
 public:
  LrbContentBrowserClient();
  LrbContentBrowserClient(const LrbContentBrowserClient&) = delete;
  LrbContentBrowserClient& operator=(const LrbContentBrowserClient&) = delete;
  ~LrbContentBrowserClient() override;

  // The browser's one instance.
  static LrbContentBrowserClient* Get();

  // Standard mode: every site in this instance, no site rule.
  bool standard() const { return standard_; }
  // Whether the user typed into a form of `contents`' page (Chrome's tab
  // discarding spares such pages too): performance_manager's frame nodes,
  // which the renderer tells about form interaction. An unload handler
  // alone says nothing: most sites register one for analytics.
  static bool HasUnsavedInput(content::WebContents* contents);

  // Lean mode: the one site this instance shows; empty until known (and
  // always in standard mode).
  const std::string& site() const { return site_; }
  void SetSite(const std::string& site);

  bool has_coordinator() const { return !!coordinator_; }
  // Sends a line to the coordinator, if connected (the file broker).
  void SendToCoordinator(const std::string& line);

  // Shows `url` in `site`'s window: through the coordinator, which reuses
  // an open window, or by launching an instance directly without one.
  // With `bounds` (screen origin, page size), the new window replaces one
  // of ours and opens in its place. `back`: the page it was left from, for
  // Back. `restore`: going Back to `site`, so restore the window it left.
  // `handoff` (StartHandoff()'s token, 0 for none): the window at `bounds`
  // is being replaced, and the coordinator tells this instance to hide it
  // ("hide <n>") once the new one is on screen.
  void OpenSiteWindow(const std::string& site,
                      const GURL& url,
                      std::optional<gfx::Rect> bounds = std::nullopt,
                      const GURL& back = GURL(),
                      bool restore = false,
                      uint64_t handoff = 0);

  // A token for a hand-off of `leaving`'s window (from 1, per process): the
  // coordinator's "hide <token>" hides it, if it's still there.
  uint64_t StartHandoff(content::WebContents* leaving);

  // Tells the coordinator "shown <handoff>" when the next window is on
  // screen: this instance's window replaces the one that hand-off left.
  void ReportShownOnNextWindow(uint64_t handoff);

  // The platform delegate's report whenever the set of visible windows
  // changes: tells the coordinator, on change, whether every window is
  // hidden ("parked 1": the instance is parked) or not ("parked 0").
  void ReportWindowsHidden(bool all_hidden);

  // Standard mode, after the user turned content blocking off (or on) for
  // `site`: saves the list in the profile, ends the renderers showing the
  // site (they have the old list: --lrb-blocking-off-sites) and reloads
  // its pages, bypassing the cache, in new ones.
  void ReloadSiteBypassingCache(const std::string& site);

  // Tells the coordinator how many live tabs aren't shown (when it
  // changes): they can sleep before any site closes.
  void ReportBackgroundTabs(int count);

  // Tells the coordinator the user is using this instance (at most once a
  // second).
  void ReportActive();

  // A page finished discarding (ActivityTracker).
  void OnPageDiscarded();

  // Closes this instance's windows (and so the instance) once the current
  // task is done.
  void CloseAllWindowsSoon();

  // Connects if --lrb-coordinator is given. Needs the IO thread.
  void ConnectToCoordinator();

  // The profile (null before the browser's main loop starts and after it
  // ends).
  LrbBrowserContext* browser_context();

  // The user agent string and its client hints (Sec-CH-UA).
  static std::string UserAgent();
  static blink::UserAgentMetadata UserAgentMetadata();

  // content::ContentBrowserClient:
  bool IsHandledURL(const GURL& url) override;
  std::string GetAcceptLangs(content::BrowserContext* context) override;
  std::string GetDefaultDownloadName() override;
  content::GeneratedCodeCacheSettings GetGeneratedCodeCacheSettings(
      content::BrowserContext* context) override;
  base::OnceClosure SelectClientCertificate(
      content::BrowserContext* browser_context,
      int process_id,
      content::WebContents* web_contents,
      net::SSLCertRequestInfo* cert_request_info,
      net::ClientCertIdentityList client_certs,
      std::unique_ptr<content::ClientCertificateDelegate> delegate) override;
  content::SpeechRecognitionManagerDelegate*
  CreateSpeechRecognitionManagerDelegate() override;
  void OverrideWebPreferences(content::WebContents* web_contents,
                              content::SiteInstance& main_frame_site,
                              blink::web_pref::WebPreferences* prefs) override;
  std::unique_ptr<content::DevToolsManagerDelegate>
  CreateDevToolsManagerDelegate() override;
  void ExposeInterfacesToRenderer(
      service_manager::BinderRegistry* registry,
      blink::AssociatedInterfaceRegistry* associated_registry,
      content::RenderProcessHost* render_process_host) override;
  // Renderers and the zygotes get the mode, the engine file and (standard
  // mode) the sites blocking is off for; every other child the mode.
  void AppendExtraCommandLineSwitches(base::CommandLine* command_line,
                                      int child_process_id) override;
  void ExposeInterfacesToChild(
      mojo::BinderMapWithContext<content::BrowserChildProcessHost*>* map)
      override;
  void RegisterBrowserInterfaceBindersForFrame(
      content::RenderFrameHost* render_frame_host,
      mojo::BinderMapWithContext<content::RenderFrameHost*>* map) override;
  void RegisterAssociatedInterfaceBindersForRenderFrameHost(
      content::RenderFrameHost& render_frame_host,
      blink::AssociatedInterfaceRegistry& associated_registry) override;
  void OpenURL(content::SiteInstance* site_instance,
               const content::OpenURLParams& params,
               base::OnceCallback<void(content::WebContents*)> callback)
      override;
  base::DictValue GetNetLogConstants() override;
  base::FilePath GetSandboxedStorageServiceDataDirectory() override;
  base::FilePath GetFirstPartySetsDirectory() override;
  std::optional<base::FilePath> GetLocalTracesDirectory() override;
  std::string GetUserAgent() override;
  blink::UserAgentMetadata GetUserAgentMetadata() override;
  void OnNetworkServiceCreated(
      network::mojom::NetworkService* network_service) override;
  void ConfigureNetworkContextParams(
      content::BrowserContext* context,
      bool in_memory,
      const base::FilePath& relative_partition_path,
      network::mojom::NetworkContextParams* network_context_params,
      cert_verifier::mojom::CertVerifierCreationParams*
          cert_verifier_creation_params) override;
  std::vector<base::FilePath> GetNetworkContextsParentDirectory() override;
  bool HasErrorPage(int http_status_code) override;
  void OnWebContentsCreated(content::WebContents* web_contents) override;
  // No window or tab without a user gesture (Chrome's popup blocker). A link
  // opened in the background (middle-click) becomes a tab that loads when
  // first shown: no page is created for it now.
  bool CanCreateWindow(content::RenderFrameHost* opener,
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
                       bool* no_javascript_access) override;
  std::unique_ptr<content::LoginDelegate> CreateLoginDelegate(
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
      content::LoginDelegate::LoginAuthRequiredCallback auth_required_callback)
      override;
  std::unique_ptr<content::WebContentsViewDelegate> GetWebContentsViewDelegate(
      content::WebContents* web_contents) override;
  std::unique_ptr<content::BrowserMainParts> CreateBrowserMainParts(
      bool is_integration_test) override;
  void CreateThrottlesForNavigation(
      content::NavigationThrottleRegistry& registry) override;
  // A page whose certificate isn't valid: explained in the window, with
  // Back and (unless the site requires valid HTTPS) Continue anyway
  // (certificate_errors.h). Other requests with one are refused.
  void AllowCertificateError(
      content::WebContents* web_contents,
      int cert_error,
      const net::SSLInfo& ssl_info,
      const GURL& request_url,
      bool is_primary_main_frame_request,
      bool strict_enforcement,
      base::OnceCallback<void(content::CertificateRequestResultType)>
          callback) override;
  // Links for other apps (mailto:, tel:, ...): asked about in the window,
  // then opened by the desktop (external_links.h).
  bool HandleExternalProtocol(
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
      mojo::PendingRemote<network::mojom::URLLoaderFactory>* out_factory)
      override;
  void WillCreateURLLoaderFactory(
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
      bool is_for_network_service) override;

 private:
  void OnCoordinatorLine(const std::string& line);

  // Drops every window's page, keeping the windows and their history; a
  // page comes back when its window is used again (ActivityTracker). Not
  // critical: pages that would ask before leaving (unsaved changes) stay.
  void DiscardPages(bool critical);

  // "show <url> [bounds=..] [back=..] [handoff=<n>]".
  void Show(std::string_view args);

  raw_ptr<LrbBrowserMainParts> main_parts_ = nullptr;
  const bool standard_;
  std::string site_;
  uint64_t last_handoff_ = 0;
  // Windows left for another site, until the coordinator says to hide them.
  std::map<uint64_t, base::WeakPtr<content::WebContents>> handoffs_;
  bool reported_parked_ = false;
  int reported_background_tabs_ = 0;
  std::unique_ptr<CoordinatorClient> coordinator_;
  base::TimeTicks last_active_report_;
};

}  // namespace lrb

#endif  // LRB_BROWSER_LRB_CONTENT_BROWSER_CLIENT_H_
