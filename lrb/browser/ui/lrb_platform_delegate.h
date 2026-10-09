// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_UI_LRB_PLATFORM_DELEGATE_H_
#define LRB_BROWSER_UI_LRB_PLATFORM_DELEGATE_H_

#include <deque>
#include <memory>
#include <optional>
#include <vector>

#include "base/functional/callback_forward.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "content/public/browser/media_stream_request.h"
#include "content/public/browser/page_navigator.h"
#include "third_party/blink/public/mojom/choosers/file_chooser.mojom-forward.h"
#include "ui/gfx/native_ui_types.h"
#include "ui/views/widget/widget_observer.h"
#include "ui/gfx/geometry/rect.h"
#include "third_party/blink/public/common/permissions/permission_utils.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace content {
class BrowserContext;
class FileSelectListener;
class JavaScriptDialogManager;
class RenderFrameHost;
class WebContents;
}  // namespace content

namespace display {
class Screen;
}

namespace wm {
class WMState;
}

namespace views {
class ViewsDelegate;
class Widget;
class WidgetDelegate;
}  // namespace views

namespace lrb {

class Shell;
class WindowView;

// lrb's windows: Views windows with lrb's slim bar (see window_view.h), and
// tabs. Shell (a page) reports to it. Started from content_shell's
// ShellPlatformDelegate and its Views implementation.
//
// Tabs (decided 2026-10-05): a window shows one of its tabs. A live tab is a
// Shell (its page). A tab opened in the background, or restored but not
// shown yet, is only its address (and saved history): it loads when first
// shown. New pages of the site become tabs of the most recently used
// window: everything lrb opens itself (New page, the coordinator's "show",
// "Open link in new window") and new windows a page opens without an
// opener (target=_blank). Popups with an opener (sign-in, payment) stay
// windows of their own: they must stay live and talk to their opener.
class LrbPlatformDelegate : public views::WidgetObserver {
 public:
  enum UIControl { BACK_BUTTON, FORWARD_BUTTON, STOP_BUTTON };

  LrbPlatformDelegate();
  LrbPlatformDelegate(const LrbPlatformDelegate&) = delete;
  LrbPlatformDelegate& operator=(const LrbPlatformDelegate&) = delete;
  ~LrbPlatformDelegate() override;

  // The next window opens at `bounds` (screen origin, page size) instead of
  // the default place and size: a window replacing another site's.
  static void SetNextWindowBounds(const gfx::Rect& bounds);
  // The next window was opened from `back`, a page of another site: Back on
  // its first page returns there.
  static void SetNextWindowBackUrl(const GURL& back);
  // The page of another site `shell`'s window was opened from, if any.
  static GURL BackUrlOf(Shell* shell);

  // Asks the user, in `contents`' window, whether `origin` may have
  // `types` (camera, microphone, ...: AskablePermission). `focus`: the
  // request follows a click, so the question may take keyboard focus.
  // `answer` gets the answer, or nothing if the question went away
  // unanswered (the page navigated, the window closed).
  static void AskPermission(
      content::WebContents* contents,
      const url::Origin& origin,
      const std::vector<blink::PermissionType>& types,
      bool focus,
      base::OnceCallback<void(std::optional<bool>)> answer);

  // `shell`'s window contents (bar, question row, page), if it has a window.
  static WindowView* ViewFor(Shell* shell);

  // The user closes `shell`'s window (close button, Ctrl+W, the menu): the
  // page may ask "leave this page?" first (content_shell closed at once).
  // Closing under memory pressure doesn't ask.
  static void CloseByUser(Shell* shell);

  // As ViewFor(), first bringing `shell`'s tab forward: for questions and
  // dialogs from a page, which must be seen to be answered.
  static WindowView* ShowTabFor(Shell* shell);

  // A tab for `url`, loaded when first shown, after the tab of `opener` (a
  // link opened in the background). False if `opener` has no window.
  static bool AddBackgroundTab(Shell* opener, const GURL& url);

  // Closes every window (the menu's "Close all pages of this site"): tabs
  // not loaded go at once, each page may ask "leave this page?".
  static void CloseAllByUser();

  // Under pressure (the coordinator's "discard-background"): live tabs not
  // shown sleep (URL + history, back when shown), except ones playing sound.
  static void DiscardBackgroundTabs();

  // Forgets tabs not loaded (before closing everything).
  static void DropUnloadedTabs();

  // All tabs, live or not, in all windows (to know a site's last tab).
  static size_t TabCount();

  // Whether a closed tab can be reopened (Ctrl+Shift+T, the menu).
  static bool CanReopenClosed();

  // For saving and restoring windows (saved_windows.cc).
  struct TabState {
    raw_ptr<Shell> shell;  // live; else the fields below
    GURL url;
    std::u16string title;
    GURL back_url;
    std::optional<base::DictValue> history;  // a restored tab's history
  };
  struct WindowState {
    std::vector<TabState> tabs;
    size_t active = 0;
  };
  static std::vector<WindowState> Windows();
  // Opens a window with `tabs`, showing (and loading) the `active` one;
  // `load` loads a tab into the Shell it creates: its URL, or its history.
  static void OpenWindow(
      content::BrowserContext* context,
      std::vector<TabState> tabs,
      size_t active,
      base::RepeatingCallback<Shell*(content::BrowserContext*,
                                              const TabState&)> load);

  // The window showing `contents`: its screen origin and page size.
  static std::optional<gfx::Rect> WindowBoundsOf(
      content::WebContents* contents);

  // From Shell:
  // The Views platform: window manager state, screen, ViewsDelegate.
  void Initialize();
  // The last page closed: quit.
  void DidCloseLastWindow();
  void CreatePlatformWindow(Shell* shell, const gfx::Size& initial_size);
  gfx::NativeWindow GetNativeWindow(Shell* shell);
  void CleanUp(Shell* shell);
  void SetContents(Shell* shell);
  void EnableUIControl(Shell* shell, UIControl control, bool is_enabled);
  void SetAddressBarURL(Shell* shell, const GURL& url);
  void SetIsLoading(Shell* shell, bool loading);
  void SetTitle(Shell* shell, const std::u16string& title);
  // Whether it destroys `shell` itself (true) or Shell deletes itself.
  bool DestroyShell(Shell* shell);
  // alert(), confirm(), prompt(), "leave this page?": the question row.
  std::unique_ptr<content::JavaScriptDialogManager>
  CreateJavaScriptDialogManager(Shell* shell);
  // A link opened in the background (middle-click): a tab that loads when
  // first shown (another site's: that site's window).
  bool OpenURLFromTab(Shell* shell,
                      content::WebContents* source,
                      const content::OpenURLParams& params);
  // Find-in-page results: the find row of the window showing `shell`.
  void FindReply(Shell* shell,
                 int request_id,
                 int number_of_matches,
                 int active_match_ordinal,
                 bool final_update);
  // A page (window.focus()) or DevTools brings its tab to the front.
  void ActivateContents(Shell* shell, content::WebContents* contents);
  // <input type=file>: the system file picker (content_shell cancelled).
  void RunFileChooser(content::RenderFrameHost* render_frame_host,
                      scoped_refptr<content::FileSelectListener> listener,
                      const blink::mojom::FileChooserParams& params);
  void RequestMediaAccessPermission(
      Shell* shell,
      content::WebContents* web_contents,
      const content::MediaStreamRequest& request,
      content::MediaResponseCallback callback);
  bool CheckMediaAccessPermission(Shell* shell,
                                  content::RenderFrameHost* render_frame_host,
                                  const url::Origin& security_origin,
                                  blink::mojom::MediaStreamType type);

  // views::WidgetObserver: the most recently used window gets new tabs.
  void OnWidgetActivationChanged(views::Widget* widget, bool active) override;

 private:
  struct Tab {
    Tab();
    Tab(Tab&&);
    Tab& operator=(Tab&&);
    ~Tab();
    raw_ptr<Shell> shell;  // null: not loaded yet
    GURL url;
    std::u16string title;
    GURL back_url;  // the page of another site it was opened from
    std::optional<base::DictValue> history;
  };

  // Owned here (CLIENT_OWNS_WIDGET): a Shell owns itself, and its
  // destructor's CleanUp() removes its tab, and with the last one destroys
  // the window. The widget goes first (members are destroyed in reverse
  // order): a WidgetDelegate must outlive its Widget.
  struct Window {
    Window();
    ~Window();
    std::unique_ptr<views::WidgetDelegate> delegate;
    std::unique_ptr<views::Widget> widget;
    gfx::Size content_size;
    raw_ptr<content::BrowserContext> context;
    bool shown = false;
    bool closing = false;  // in CleanUp(): its last Shell is being destroyed
    std::vector<Tab> tabs;
    size_t active = 0;
  };

  // Where the next Shell goes: a new window, or a tab of `window` (in place
  // `tab`, or a new one after the active tab).
  struct Next {
    bool new_window = false;
    raw_ptr<Window> window = nullptr;
    std::optional<size_t> tab;
  };

  Window* WindowOf(Shell* shell);
  std::optional<size_t> TabOf(Window* window, Shell* shell);
  bool Exists(const Window* window) const;
  Window* LastActive();
  WindowView* ViewOf(Window* window);
  // Shows tab `index` of `window`, loading it if it isn't.
  void Activate(Window* window, size_t index);
  void Cycle(Window* window, int delta);
  void CloseTab(Window* window, size_t index);
  void NewTab(Window* window);
  // A window of its own for the site's home page (Ctrl+N).
  void NewWindow(Window* window);
  // Remembers a tab going away, for ReopenClosed().
  void RememberClosed(const Tab& tab);
  // The last tab closed comes back, with its history, in `window`.
  void ReopenClosed(Window* window);
  // The tab row and the window's icon.
  void ShowTabRow(Window* window);
  // The tab row, and what follows a change of tabs (the session, the
  // coordinator's count of background tabs).
  void UpdateTabs(Window* window);
  // Live, awake tabs not shown, in all windows (for the coordinator).
  int BackgroundTabs();
  // Loads a tab into a new Shell: its saved history (tab_loader_), else its
  // address.
  void LoadTab(content::BrowserContext* context, const TabState& tab);

  // The window's close button or the window manager closed it.
  void OnWindowClosed(Window* window);
  void CloseWindow(Window* window);
  // The window's close button (and the window manager's close) go through
  // OnWindowClosed: the pages may ask first.
  void ArmCloseButton(Window* window);

  // The Views platform (Initialize()).
  std::unique_ptr<wm::WMState> wm_state_;
  std::unique_ptr<display::Screen> screen_;
  std::unique_ptr<views::ViewsDelegate> views_delegate_;

  std::vector<std::unique_ptr<Window>> windows_;
  raw_ptr<Window> last_active_ = nullptr;
  Next next_;
  // Tabs closed, the last one at the back (kMaxClosed at most): their
  // address and history, as saved windows keep them.
  std::deque<TabState> closed_;
  base::RepeatingCallback<Shell*(content::BrowserContext*,
                                          const TabState&)>
      tab_loader_;

  base::WeakPtrFactory<LrbPlatformDelegate> weak_factory_{this};
};

}  // namespace lrb

#endif  // LRB_BROWSER_UI_LRB_PLATFORM_DELEGATE_H_
