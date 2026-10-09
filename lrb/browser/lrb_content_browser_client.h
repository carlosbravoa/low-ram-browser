// Copyright 2026 The low-ram-browser Authors
// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_LRB_CONTENT_BROWSER_CLIENT_H_
#define LRB_BROWSER_LRB_CONTENT_BROWSER_CLIENT_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/time/time.h"
#include "content/public/browser/content_browser_client.h"
#include "third_party/blink/public/common/user_agent/user_agent_metadata.h"
#include "ui/gfx/geometry/rect.h"

class GURL;

namespace lrb {

class CoordinatorClient;
class LrbBrowserContext;
class LrbBrowserMainParts;

// What the browser does for content: the one-site-per-window rule, the
// connection to lrb_coordinator, content blocking, windows, sign-in, the
// network's settings. From content_shell's ShellContentBrowserClient without
// its web-test and browser-test hooks.
class LrbContentBrowserClient : public content::ContentBrowserClient {
 public:
  LrbContentBrowserClient();
  LrbContentBrowserClient(const LrbContentBrowserClient&) = delete;
  LrbContentBrowserClient& operator=(const LrbContentBrowserClient&) = delete;
  ~LrbContentBrowserClient() override;

  // The browser's one instance.
  static LrbContentBrowserClient* Get();

  // The one site this instance shows; empty until known.
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
  void OpenSiteWindow(const std::string& site,
                      const GURL& url,
                      std::optional<gfx::Rect> bounds = std::nullopt,
                      const GURL& back = GURL(),
                      bool restore = false);

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
  // A link opened in the background (middle-click) becomes a tab that loads
  // when first shown: no page is created for it now.
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
  // page comes back when its window is used again (ActivityTracker).
  void DiscardPages();

  raw_ptr<LrbBrowserMainParts> main_parts_ = nullptr;
  std::string site_;
  int reported_background_tabs_ = 0;
  std::unique_ptr<CoordinatorClient> coordinator_;
  base::TimeTicks last_active_report_;
};

}  // namespace lrb

#endif  // LRB_BROWSER_LRB_CONTENT_BROWSER_CLIENT_H_
