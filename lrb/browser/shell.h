// Copyright 2026 The low-ram-browser Authors
// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_SHELL_H_
#define LRB_BROWSER_SHELL_H_

#include <memory>
#include <string>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "content/public/browser/web_contents_delegate.h"
#include "content/public/browser/web_contents_observer.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/native_ui_types.h"

class GURL;

namespace content {
class BrowserContext;
class JavaScriptDialogManager;
class SiteInstance;
}  // namespace content

namespace lrb {

class LrbPlatformDelegate;

// One page: a WebContents and the delegate that answers for it. A tab of a
// window (LrbPlatformDelegate owns the windows and the tabs). Owns itself:
// Close() deletes it.
//
// From content_shell's Shell (content/shell/browser/shell.cc at
// CHROMIUM_COMMIT), without web tests, DevTools frontend windows, test hooks
// and other platforms, calling lrb's platform delegate directly; the hooks
// lrb used to patch into content_shell (patches 0003-0006) are plain calls
// here.
class Shell : public content::WebContentsDelegate,
              public content::WebContentsObserver {
 public:
  Shell(const Shell&) = delete;
  Shell& operator=(const Shell&) = delete;
  ~Shell() override;

  void LoadURL(const GURL& url);
  void GoBackOrForward(int offset);
  void Reload();
  void ReloadBypassingCache();
  void Stop();
  void UpdateNavigationControls(bool should_show_loading_ui);
  void Close();

  // The windows' platform: owned until Shutdown().
  static void Initialize(std::unique_ptr<LrbPlatformDelegate> platform);
  // Closes every page and quits the main loop. Idempotent.
  static void Shutdown();

  static Shell* CreateNewWindow(
      content::BrowserContext* browser_context,
      const GURL& url,
      const scoped_refptr<content::SiteInstance>& site_instance,
      const gfx::Size& initial_size);

  // The Shell for `web_contents`, or null.
  static Shell* FromWebContents(content::WebContents* web_contents);
  // Every live page, oldest first.
  static std::vector<Shell*>& windows();
  static void SetMainMessageLoopQuitClosure(base::OnceClosure quit_closure);
  // The size of a window that gives none (--content-shell-host-window-size,
  // kept for the harness, or 800x600).
  static gfx::Size GetShellDefaultSize();
  // --content-shell-hide-toolbar: windows without the bar (kiosks, and
  // measuring the bar).
  static bool ShouldHideToolbar();

  content::WebContents* web_contents() const { return web_contents_.get(); }
  gfx::NativeWindow window();

  // content::WebContentsDelegate:
  content::WebContents* OpenURLFromTab(
      content::WebContents* source,
      const content::OpenURLParams& params,
      base::OnceCallback<void(content::NavigationHandle&)>
          navigation_handle_callback) override;
  content::WebContents* AddNewContents(
      content::WebContents* source,
      std::unique_ptr<content::WebContents> new_contents,
      const GURL& target_url,
      WindowOpenDisposition disposition,
      const blink::mojom::WindowFeatures& window_features,
      bool user_gesture,
      bool* was_blocked) override;
  void LoadingStateChanged(content::WebContents* source,
                           bool should_show_loading_ui) override;
  void EnterFullscreenModeForTab(
      content::RenderFrameHost* requesting_frame,
      const blink::mojom::FullscreenOptions& options) override;
  void ExitFullscreenModeForTab(content::WebContents* web_contents) override;
  bool IsFullscreenForTabOrPending(
      const content::WebContents* web_contents) override;
  blink::mojom::DisplayMode GetDisplayMode(
      const content::WebContents* web_contents) override;
  void RequestPointerLock(content::WebContents* web_contents,
                          bool user_gesture,
                          bool last_unlocked_by_target) override;
  void CloseContents(content::WebContents* source) override;
  // The page's beforeunload (or the user answering its dialog) decides
  // whether the page closes; WebContentsDelegate's default closed anyway.
  using content::WebContentsObserver::BeforeUnloadFired;
  void BeforeUnloadFired(content::WebContents* tab,
                         bool proceed,
                         bool* proceed_to_fire_unload) override;
  bool CanOverscrollContent() override;
  void NavigationStateChanged(content::WebContents* source,
                              content::InvalidateTypes changed_flags) override;
  content::JavaScriptDialogManager* GetJavaScriptDialogManager(
      content::WebContents* source) override;
  void FindReply(content::WebContents* web_contents,
                 int request_id,
                 int number_of_matches,
                 const gfx::Rect& selection_rect,
                 int active_match_ordinal,
                 bool final_update) override;
  void RequestMediaAccessPermission(
      content::WebContents* web_contents,
      const content::MediaStreamRequest& request,
      content::MediaResponseCallback callback) override;
  bool CheckMediaAccessPermission(content::RenderFrameHost* render_frame_host,
                                  const url::Origin& security_origin,
                                  blink::mojom::MediaStreamType type) override;
  void RendererUnresponsive(
      content::WebContents* source,
      content::RenderWidgetHost* render_widget_host,
      base::RepeatingClosure hang_monitor_restarter) override;
  void ActivateContents(content::WebContents* contents) override;
  void RunFileChooser(content::RenderFrameHost* render_frame_host,
                      scoped_refptr<content::FileSelectListener> listener,
                      const blink::mojom::FileChooserParams& params) override;
  void EnumerateDirectory(content::WebContents* web_contents,
                          scoped_refptr<content::FileSelectListener> listener,
                          const base::FilePath& path) override;
  bool IsBackForwardCacheSupported(content::WebContents& contents) override;
  content::PreloadingEligibility IsPrerender2Supported(
      content::WebContents& web_contents,
      content::PreloadingTriggerType trigger_type) override;

 private:
  explicit Shell(std::unique_ptr<content::WebContents> web_contents);

  static Shell* CreateShell(std::unique_ptr<content::WebContents> web_contents,
                            const gfx::Size& initial_size);
  static gfx::Size AdjustWindowSize(const gfx::Size& initial_size);

  void ToggleFullscreenModeForTab(content::WebContents* web_contents,
                                  bool enter_fullscreen);

  // content::WebContentsObserver:
  void TitleWasSet(content::NavigationEntry* entry) override;

  std::unique_ptr<content::JavaScriptDialogManager> dialog_manager_;
  std::unique_ptr<content::WebContents> web_contents_;
  bool is_fullscreen_ = false;
};

}  // namespace lrb

#endif  // LRB_BROWSER_SHELL_H_
