// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/ui/lrb_platform_delegate.h"

#include <memory>
#include <utility>

#include "base/check.h"
#include "base/files/file_enumerator.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/file_select_listener.h"
#include "content/public/browser/media_capture_devices.h"
#include "content/public/browser/media_stream_request.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "lrb/browser/shell.h"
#include "lrb/browser/activity_tracker.h"
#include "lrb/browser/lrb_browser_context.h"
#include "lrb/browser/lrb_content_browser_client.h"
#include "lrb/browser/permission_manager.h"
#include "lrb/browser/permissions/site_permissions.h"
#include "lrb/browser/saved_windows.h"
#include "lrb/browser/site.h"
#include "lrb/browser/ui/file_picker.h"
#include "lrb/browser/ui/javascript_dialogs.h"
#include "lrb/browser/ui/window_view.h"
#include "third_party/blink/public/mojom/choosers/file_chooser.mojom.h"
#include "third_party/blink/public/mojom/mediastream/media_stream.mojom.h"
#include "ui/aura/window.h"
#include "ui/aura/window_tree_host.h"
#include "ui/color/color_id.h"
#include "ui/color/color_provider.h"
#include "ui/views/controls/webview/web_contents_set_background_color.h"
#include "ui/display/screen.h"
#include "ui/views/layout/layout_provider.h"
#include "ui/views/views_delegate.h"
#include "ui/views/widget/desktop_aura/desktop_native_widget_aura.h"
#include "ui/views/widget/desktop_aura/desktop_screen.h"
#include "ui/views/widget/native_widget_aura.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_delegate.h"
#include "ui/wm/core/wm_state.h"

namespace lrb {

namespace {

LrbPlatformDelegate* g_delegate = nullptr;
std::optional<gfx::Rect>& NextWindowBounds() {
  static std::optional<gfx::Rect> bounds;
  return bounds;
}

GURL& NextWindowBackUrl() {
  static base::NoDestructor<GURL> back;
  return *back;
}

WindowView* ViewOf(views::Widget* widget) {
  return static_cast<WindowView*>(widget->widget_delegate()->GetContentsView());
}

// Shows camera/microphone use in the window's bar while a stream runs.
class CaptureIndicator : public content::MediaStreamUI {
 public:
  CaptureIndicator(base::WeakPtr<WindowView> view, bool audio, bool video)
      : view_(std::move(view)), audio_(audio), video_(video) {}
  ~CaptureIndicator() override {
    if (started_ && view_) {
      view_->RemoveCapture(audio_, video_);
    }
  }

  // content::MediaStreamUI:
  gfx::NativeViewId OnStarted(
      base::RepeatingClosure stop,
      SourceCallback source,
      const std::string& label,
      std::vector<content::DesktopMediaID> screen_capture_ids,
      StateChangeCallback state_change) override {
    if (!started_ && view_) {
      started_ = true;
      view_->AddCapture(audio_, video_);
    }
    return 0;
  }
  void OnDeviceStoppedForSourceChange(
      const std::string& label,
      const content::DesktopMediaID& old_media_id,
      const content::DesktopMediaID& new_media_id,
      bool captured_surface_control_active) override {}
  void OnDeviceStopped(const std::string& label,
                       const content::DesktopMediaID& media_id) override {}

 private:
  base::WeakPtr<WindowView> view_;
  const bool audio_;
  const bool video_;
  bool started_ = false;
};

// The requested device, else the first one; none if there is none.
std::optional<blink::MediaStreamDevice> PickDevice(
    const blink::MediaStreamDevices& devices,
    const std::vector<std::string>& requested_ids) {
  for (const blink::MediaStreamDevice& device : devices) {
    if (requested_ids.empty() || device.id == requested_ids.front()) {
      return device;
    }
  }
  return devices.empty() || !requested_ids.empty()
             ? std::nullopt
             : std::optional<blink::MediaStreamDevice>(devices.front());
}

std::vector<blink::mojom::FileChooserFileInfoPtr> NativeFiles(
    const std::vector<base::FilePath>& paths) {
  std::vector<blink::mojom::FileChooserFileInfoPtr> files;
  for (const base::FilePath& path : paths) {
    files.push_back(blink::mojom::FileChooserFileInfo::NewNativeFile(
        blink::mojom::NativeFileInfo::New(path, std::u16string(),
                                          std::vector<std::u16string>())));
  }
  return files;
}

LrbPermissionManager& PermissionManagerOf(content::WebContents* contents) {
  return *static_cast<LrbPermissionManager*>(
      contents->GetBrowserContext()->GetPermissionControllerDelegate());
}

// Views' platform choices, as content_shell made them through views'
// DesktopTestViewsDelegate: opaque windows unless asked otherwise; top-level
// windows, menus and tooltips get desktop widgets (their own X11 or Wayland
// window), widgets with a parent live inside it. Owns the layout provider
// (spacing and sizes: buttons need one).
class LrbViewsDelegate : public views::ViewsDelegate {
 public:
  void OnBeforeWidgetInit(
      views::Widget::InitParams* params,
      views::internal::NativeWidgetDelegate* delegate) override {
    if (params->opacity ==
        views::Widget::InitParams::WindowOpacity::kInferred) {
      params->opacity = views::Widget::InitParams::WindowOpacity::kOpaque;
    }
    if (params->native_widget) {
      return;
    }
    if (params->parent &&
        params->type != views::Widget::InitParams::TYPE_MENU &&
        params->type != views::Widget::InitParams::TYPE_TOOLTIP) {
      params->native_widget = new views::NativeWidgetAura(delegate);
    } else {
      params->native_widget = new views::DesktopNativeWidgetAura(delegate);
    }
  }

 private:
  views::LayoutProvider layout_provider_;
};

}  // namespace

LrbPlatformDelegate::Tab::Tab() = default;
LrbPlatformDelegate::Tab::Tab(Tab&&) = default;
LrbPlatformDelegate::Tab& LrbPlatformDelegate::Tab::operator=(Tab&&) = default;
LrbPlatformDelegate::Tab::~Tab() = default;
LrbPlatformDelegate::Window::Window() = default;
LrbPlatformDelegate::Window::~Window() = default;

LrbPlatformDelegate::LrbPlatformDelegate() {
  g_delegate = this;
}

LrbPlatformDelegate::~LrbPlatformDelegate() {
  g_delegate = nullptr;
}

void LrbPlatformDelegate::Initialize() {
  wm_state_ = std::make_unique<wm::WMState>();
  if (!display::Screen::HasScreen()) {
    screen_ = views::CreateDesktopScreen();
  }
  views_delegate_ = std::make_unique<LrbViewsDelegate>();
}

void LrbPlatformDelegate::DidCloseLastWindow() {
  // The user closed the site's windows (closing to free memory keeps them:
  // KeepSession): they don't come back next time.
  if (LrbContentBrowserClient* client = LrbContentBrowserClient::Get()) {
    SessionClosedByUser(client->browser_context());
  }
  Shell::Shutdown();
}

// static
void LrbPlatformDelegate::SetNextWindowBounds(const gfx::Rect& bounds) {
  NextWindowBounds() = bounds;
}

// static
void LrbPlatformDelegate::SetNextWindowBackUrl(const GURL& back) {
  NextWindowBackUrl() = back;
}

LrbPlatformDelegate::Window* LrbPlatformDelegate::WindowOf(
    Shell* shell) {
  for (const std::unique_ptr<Window>& window : windows_) {
    if (TabOf(window.get(), shell)) {
      return window.get();
    }
  }
  return nullptr;
}

std::optional<size_t> LrbPlatformDelegate::TabOf(Window* window,
                                                  Shell* shell) {
  for (size_t i = 0; i < window->tabs.size(); ++i) {
    if (shell && window->tabs[i].shell == shell) {
      return i;
    }
  }
  return std::nullopt;
}

bool LrbPlatformDelegate::Exists(const Window* window) const {
  for (const std::unique_ptr<Window>& w : windows_) {
    if (w.get() == window) {
      return !w->closing;
    }
  }
  return false;
}

LrbPlatformDelegate::Window* LrbPlatformDelegate::LastActive() {
  if (last_active_ && Exists(last_active_)) {
    return last_active_;
  }
  for (const std::unique_ptr<Window>& window : windows_) {
    if (!window->closing) {
      return window.get();
    }
  }
  return nullptr;
}

WindowView* LrbPlatformDelegate::ViewOf(Window* window) {
  return lrb::ViewOf(window->widget.get());
}

// static
GURL LrbPlatformDelegate::BackUrlOf(Shell* shell) {
  Window* window = g_delegate ? g_delegate->WindowOf(shell) : nullptr;
  return window ? window->tabs[*g_delegate->TabOf(window, shell)].back_url
                : GURL();
}

// static
WindowView* LrbPlatformDelegate::ViewFor(Shell* shell) {
  Window* window = g_delegate ? g_delegate->WindowOf(shell) : nullptr;
  if (!window || window->closing) {
    return nullptr;
  }
  return g_delegate->ViewOf(window);
}

// static
WindowView* LrbPlatformDelegate::ShowTabFor(Shell* shell) {
  Window* window = g_delegate ? g_delegate->WindowOf(shell) : nullptr;
  if (!window || window->closing) {
    return nullptr;
  }
  const size_t index = *g_delegate->TabOf(window, shell);
  if (index != window->active) {
    g_delegate->Activate(window, index);
  }
  return g_delegate->ViewOf(window);
}

// static
bool LrbPlatformDelegate::AddBackgroundTab(Shell* opener,
                                           const GURL& url) {
  Window* window = g_delegate ? g_delegate->WindowOf(opener) : nullptr;
  if (!window || window->closing) {
    return false;
  }
  Tab tab;
  tab.url = url;
  tab.title = base::UTF8ToUTF16(base::StrCat({url.host(), url.path()}));
  const size_t index = *g_delegate->TabOf(window, opener) + 1;
  window->tabs.insert(window->tabs.begin() + index, std::move(tab));
  if (window->active >= index) {
    ++window->active;
  }
  g_delegate->UpdateTabs(window);
  return true;
}

// static
void LrbPlatformDelegate::CloseAllByUser() {
  if (!g_delegate) {
    return;
  }
  DropUnloadedTabs();
  std::vector<Shell*> live;
  for (const std::unique_ptr<Window>& window : g_delegate->windows_) {
    for (const Tab& tab : window->tabs) {
      live.push_back(tab.shell);
    }
  }
  for (Shell* shell : live) {
    if (g_delegate && g_delegate->WindowOf(shell)) {
      CloseByUser(shell);
    }
  }
}

// static
void LrbPlatformDelegate::DiscardBackgroundTabs() {
  if (!g_delegate) {
    return;
  }
  LrbContentBrowserClient* client = LrbContentBrowserClient::Get();
  for (const std::unique_ptr<Window>& window : g_delegate->windows_) {
    Shell* shown = g_delegate->ViewOf(window.get())->shell();
    for (const Tab& tab : window->tabs) {
      if (!tab.shell || tab.shell == shown || !client ||
          tab.shell->web_contents()->IsCurrentlyAudible()) {
        continue;
      }
      ActivityTracker::CreateForWebContents(tab.shell->web_contents(), *client);
      ActivityTracker::FromWebContents(tab.shell->web_contents())->Discard();
    }
  }
  if (client) {
    client->ReportBackgroundTabs(g_delegate->BackgroundTabs());
  }
}

int LrbPlatformDelegate::BackgroundTabs() {
  int count = 0;
  for (const std::unique_ptr<Window>& window : windows_) {
    Shell* shown = ViewOf(window.get())->shell();
    for (const Tab& tab : window->tabs) {
      if (!tab.shell || tab.shell == shown) {
        continue;
      }
      ActivityTracker* tracker =
          ActivityTracker::FromWebContents(tab.shell->web_contents());
      count += !(tracker && tracker->discarded());
    }
  }
  return count;
}

// static
void LrbPlatformDelegate::DropUnloadedTabs() {
  if (!g_delegate) {
    return;
  }
  for (const std::unique_ptr<Window>& window : g_delegate->windows_) {
    std::erase_if(window->tabs, [](const Tab& tab) { return !tab.shell; });
    window->active =
        g_delegate->TabOf(window.get(), g_delegate->ViewOf(window.get())->shell())
            .value_or(0);
  }
}

// static
size_t LrbPlatformDelegate::TabCount() {
  size_t count = 0;
  if (g_delegate) {
    for (const std::unique_ptr<Window>& window : g_delegate->windows_) {
      count += window->tabs.size();
    }
  }
  return count;
}

// static
std::vector<LrbPlatformDelegate::WindowState> LrbPlatformDelegate::Windows() {
  std::vector<WindowState> states;
  if (!g_delegate) {
    return states;
  }
  for (const std::unique_ptr<Window>& window : g_delegate->windows_) {
    WindowState state;
    state.active = window->active;
    for (const Tab& tab : window->tabs) {
      TabState t;
      t.shell = tab.shell;
      t.url = tab.url;
      t.title = tab.title;
      t.back_url = tab.back_url;
      if (tab.history) {
        t.history = tab.history->Clone();
      }
      state.tabs.push_back(std::move(t));
    }
    states.push_back(std::move(state));
  }
  return states;
}

// static
void LrbPlatformDelegate::OpenWindow(
    content::BrowserContext* context,
    std::vector<TabState> tabs,
    size_t active,
    base::RepeatingCallback<Shell*(content::BrowserContext*,
                                            const TabState&)> load) {
  if (!g_delegate || tabs.empty()) {
    return;
  }
  active = std::min(active, tabs.size() - 1);
  g_delegate->tab_loader_ = load;
  // The shown tab loads in a window of its own...
  g_delegate->next_ = Next();
  g_delegate->next_.new_window = true;
  Shell* shell = load.Run(context, tabs[active]);
  Window* window = g_delegate->WindowOf(shell);
  if (!window) {
    return;
  }
  // ...then the others join it, not loaded until shown.
  Tab live = std::move(window->tabs.front());
  window->tabs.clear();
  for (size_t i = 0; i < tabs.size(); ++i) {
    if (i == active) {
      window->tabs.push_back(std::move(live));
      continue;
    }
    Tab tab;
    tab.url = tabs[i].url;
    tab.title = tabs[i].title;
    tab.back_url = tabs[i].back_url;
    tab.history = std::move(tabs[i].history);
    window->tabs.push_back(std::move(tab));
  }
  window->active = active;
  g_delegate->UpdateTabs(window);
}

// static
void LrbPlatformDelegate::CloseByUser(Shell* shell) {
  content::WebContents* contents = shell->web_contents();
  if (contents && contents->NeedToFireBeforeUnloadOrUnloadEvents()) {
    // The page's beforeunload runs; if it proceeds (or the user says
    // Leave), content closes the page and Shell::CloseContents the tab.
    contents->DispatchBeforeUnload(/*auto_cancel=*/false);
    return;
  }
  shell->Close();
}

// static
void LrbPlatformDelegate::AskPermission(
    content::WebContents* contents,
    const url::Origin& origin,
    const std::vector<blink::PermissionType>& types,
    bool focus,
    base::OnceCallback<void(std::optional<bool>)> answer) {
  Shell* shell =
      contents ? Shell::FromWebContents(contents) : nullptr;
  WindowView* view = shell ? ShowTabFor(shell) : nullptr;
  if (!view) {
    std::move(answer).Run(std::nullopt);  // no window to ask in
    return;
  }
  // "example.com wants to use your camera and use your microphone" reads
  // badly: "use your camera and microphone".
  std::u16string what;
  for (size_t i = 0; i < types.size(); ++i) {
    const AskablePermission* askable = FindAskable(types[i]);
    if (!askable) {
      continue;
    }
    std::u16string part = askable->request;
    if (!what.empty()) {
      what += u" and ";
      if (part.starts_with(u"use your ") && what.starts_with(u"use your ")) {
        part = part.substr(9);
      }
    }
    what += part;
  }
  view->AskPermission(base::UTF8ToUTF16(origin.host()) + u" wants to " + what,
                      focus, std::move(answer));
}

// static
std::optional<gfx::Rect> LrbPlatformDelegate::WindowBoundsOf(
    content::WebContents* contents) {
  Shell* shell = Shell::FromWebContents(contents);
  Window* window = g_delegate && shell ? g_delegate->WindowOf(shell) : nullptr;
  if (!window) {
    return std::nullopt;
  }
  const gfx::Rect bounds = window->widget->GetWindowBoundsInScreen();
  return gfx::Rect(bounds.origin(), contents->GetContainerBounds().size());
}

void LrbPlatformDelegate::CreatePlatformWindow(Shell* shell,
                                               const gfx::Size& initial_size) {
  DCHECK(!WindowOf(shell));
  const Next next = std::exchange(next_, Next());
  const GURL back = std::exchange(NextWindowBackUrl(), GURL());
  // A tab, unless asked for a window, or a popup with an opener.
  Window* target = nullptr;
  if (!next.new_window) {
    if (next.window && Exists(next.window)) {
      target = next.window;
    } else if (!shell->web_contents()->HasOpener()) {
      target = LastActive();
    }
  }
  if (target) {
    NextWindowBounds().reset();  // a window's place: not a tab's
    if (next.tab && *next.tab < target->tabs.size() &&
        !target->tabs[*next.tab].shell) {
      target->tabs[*next.tab].shell = shell;  // a tab loading when shown
    } else {
      Tab tab;
      tab.shell = shell;
      tab.back_url = back;
      const size_t index = std::min(target->active + 1, target->tabs.size());
      target->tabs.insert(target->tabs.begin() + index, std::move(tab));
      if (target->active >= index) {
        ++target->active;
      }
    }
    return;  // shown by SetContents()
  }

  gfx::Rect bounds(initial_size);
  if (NextWindowBounds()) {
    bounds = *NextWindowBounds();
    NextWindowBounds().reset();
  }
  auto window = std::make_unique<Window>();
  Window* raw = window.get();
  window->context = shell->web_contents()->GetBrowserContext();
  window->delegate = std::make_unique<views::WidgetDelegate>();
  WindowView::TabActions actions;
  actions.select = base::BindRepeating(
      [](base::WeakPtr<LrbPlatformDelegate> self, Window* window,
         size_t index) {
        if (!self || !self->Exists(window)) {
          return;
        }
        if (index == SIZE_MAX) {
          index = window->tabs.size() - 1;  // Ctrl+9: the last tab
        }
        if (index < window->tabs.size()) {
          self->Activate(window, index);
        }
      },
      weak_factory_.GetWeakPtr(), base::Unretained(raw));
  actions.cycle = base::BindRepeating(
      [](base::WeakPtr<LrbPlatformDelegate> self, Window* window, int delta) {
        if (self && self->Exists(window)) {
          self->Cycle(window, delta);
        }
      },
      weak_factory_.GetWeakPtr(), base::Unretained(raw));
  actions.close = base::BindRepeating(
      [](base::WeakPtr<LrbPlatformDelegate> self, Window* window,
         size_t index) {
        if (self && self->Exists(window) && index < window->tabs.size()) {
          self->CloseTab(window, index);
        }
      },
      weak_factory_.GetWeakPtr(), base::Unretained(raw));
  actions.new_tab = base::BindRepeating(
      [](base::WeakPtr<LrbPlatformDelegate> self, Window* window) {
        if (self && self->Exists(window)) {
          self->NewTab(window);
        }
      },
      weak_factory_.GetWeakPtr(), base::Unretained(raw));
  actions.new_window = base::BindRepeating(
      [](base::WeakPtr<LrbPlatformDelegate> self, Window* window) {
        if (self && self->Exists(window)) {
          self->NewWindow(window);
        }
      },
      weak_factory_.GetWeakPtr(), base::Unretained(raw));
  actions.reopen_closed = base::BindRepeating(
      [](base::WeakPtr<LrbPlatformDelegate> self, Window* window) {
        if (self && self->Exists(window)) {
          self->ReopenClosed(window);
        }
      },
      weak_factory_.GetWeakPtr(), base::Unretained(raw));
  window->delegate->SetContentsView(
      std::make_unique<WindowView>(std::move(actions)));
  window->delegate->SetHasWindowSizeControls(true);
  window->content_size = bounds.size();
  Tab tab;
  tab.shell = shell;
  tab.back_url = back;
  window->tabs.push_back(std::move(tab));

  window->widget = std::make_unique<views::Widget>();
  views::Widget::InitParams params(
      views::Widget::InitParams::CLIENT_OWNS_WIDGET);
  params.bounds = bounds;
  params.delegate = window->delegate.get();
  params.wm_class_class = "lrb";
  params.wm_class_name = "lrb";
  window->widget->Init(std::move(params));
  window->widget->AddObserver(this);
  // Closing the window (its close button, the window manager) closes its
  // tabs; the last one's Shell destroys the window through CleanUp().
  ArmCloseButton(raw);
  windows_.push_back(std::move(window));
  last_active_ = raw;
  // Shown in SetContents(), once the page is in, so the window doesn't
  // change size right after appearing.
}

gfx::NativeWindow LrbPlatformDelegate::GetNativeWindow(Shell* shell) {
  Window* window = WindowOf(shell);
  DCHECK(window);
  return window->widget->GetNativeWindow();
}

void LrbPlatformDelegate::CleanUp(Shell* shell) {
  Window* window = WindowOf(shell);
  DCHECK(window);
  const size_t index = *TabOf(window, shell);
  RememberClosed(window->tabs[index]);
  if (window->tabs.size() > 1) {
    // One tab of several: the window stays.
    const bool was_active = index == window->active;
    window->tabs.erase(window->tabs.begin() + index);
    if (index < window->active ||
        window->active >= window->tabs.size()) {
      window->active = window->active ? window->active - 1 : 0;
    }
    if (was_active) {
      ViewOf(window)->PageClosed();  // its page is going away
      // A neighbour shows instead, after this Shell is gone (it may load:
      // not from within a Shell's destructor).
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE,
          base::BindOnce(
              [](base::WeakPtr<LrbPlatformDelegate> self, Window* window) {
                if (self && self->Exists(window)) {
                  self->Activate(window, window->active);
                }
              },
              weak_factory_.GetWeakPtr(), base::Unretained(window)));
    } else {
      UpdateTabs(window);
    }
    return;
  }
  // The last tab: the window goes. Close the native window now: destroying
  // a client-owned Widget only posts its native window's close, and an
  // instance quitting with its last window would exit with the window's
  // compositor still registered.
  window->closing = true;  // CloseNow() runs the close callback
  window->widget->RemoveObserver(this);
  window->widget->CloseNow();
  if (last_active_ == window) {
    last_active_ = nullptr;
  }
  std::erase_if(windows_, [window](const std::unique_ptr<Window>& w) {
    return w.get() == window;
  });
}

void LrbPlatformDelegate::Activate(Window* window, size_t index) {
  if (index >= window->tabs.size()) {
    return;
  }
  Tab& tab = window->tabs[index];
  if (!tab.shell) {
    // Not loaded yet: its Shell fills this tab (CreatePlatformWindow), and
    // SetContents() comes back here.
    window->active = index;
    next_ = Next();
    next_.window = window;
    next_.tab = index;
    TabState state;
    state.url = tab.url;
    state.title = tab.title;
    state.back_url = tab.back_url;
    if (tab.history) {
      state.history = std::move(tab.history);
      tab.history.reset();
    }
    LoadTab(window->context, state);
    return;
  }
  WindowView* view = ViewOf(window);
  Shell* shown = view->shell();
  if (shown && shown != tab.shell) {
    shown->web_contents()->WasHidden();
  }
  window->active = index;
  if (shown != tab.shell) {
    view->ShowTab(tab.shell, tab.back_url);
    tab.shell->web_contents()->WasShown();
  }
  // A tab put to sleep under pressure wakes when shown.
  if (ActivityTracker* tracker =
          ActivityTracker::FromWebContents(tab.shell->web_contents());
      tracker && tracker->discarded()) {
    tracker->RestoreNow();
  }
  window->widget->widget_delegate()->SetTitle(
      tab.shell->web_contents()->GetTitle());
  UpdateTabs(window);
}

void LrbPlatformDelegate::LoadTab(content::BrowserContext* context,
                                  const TabState& tab) {
  if (tab.history && tab_loader_) {
    tab_loader_.Run(context, tab);
    return;
  }
  Shell::CreateNewWindow(context, tab.url, nullptr, gfx::Size());
}

void LrbPlatformDelegate::Cycle(Window* window, int delta) {
  const int count = static_cast<int>(window->tabs.size());
  if (count > 1) {
    Activate(window, static_cast<size_t>(
                         (static_cast<int>(window->active) + delta + count) %
                         count));
  }
}

void LrbPlatformDelegate::CloseTab(Window* window, size_t index) {
  Tab& tab = window->tabs[index];
  if (tab.shell) {
    CloseByUser(tab.shell);  // CleanUp() removes the tab
    return;
  }
  RememberClosed(tab);
  window->tabs.erase(window->tabs.begin() + index);  // nothing loaded
  if (index < window->active) {
    --window->active;
  }
  UpdateTabs(window);
}

void LrbPlatformDelegate::NewTab(Window* window) {
  WindowView* view = ViewOf(window);
  next_ = Next();
  next_.window = window;
  Shell::CreateNewWindow(window->context, view->NewTabUrl(),
                                  nullptr, gfx::Size());
}

void LrbPlatformDelegate::UpdateTabs(Window* window) {
  std::vector<std::u16string> titles;
  for (const Tab& tab : window->tabs) {
    std::u16string title =
        tab.shell ? tab.shell->web_contents()->GetTitle() : tab.title;
    if (title.empty() && tab.shell) {
      title = base::UTF8ToUTF16(tab.shell->web_contents()->GetVisibleURL().spec());
    }
    titles.push_back(title.empty() ? u"New page" : title);
  }
  ViewOf(window)->SetTabs(titles, window->active);
  if (LrbContentBrowserClient* client = LrbContentBrowserClient::Get()) {
    client->ReportBackgroundTabs(BackgroundTabs());
  }
  SessionChanged(window->context);
}

// static
bool LrbPlatformDelegate::CanReopenClosed() {
  return g_delegate && !g_delegate->closed_.empty();
}

void LrbPlatformDelegate::RememberClosed(const Tab& tab) {
  constexpr size_t kMaxClosed = 10;
  TabState closed;
  closed.back_url = tab.back_url;
  if (tab.shell) {
    base::DictValue history = SerializeTab(tab.shell);
    const base::ListValue* entries = history.FindList("entries");
    if (!entries || entries->empty()) {
      return;  // nothing but blank pages
    }
    closed.url = tab.shell->web_contents()->GetLastCommittedURL();
    closed.title = tab.shell->web_contents()->GetTitle();
    closed.history = std::move(history);
  } else {
    closed.url = tab.url;
    closed.title = tab.title;
    if (tab.history) {
      closed.history = tab.history->Clone();
    }
  }
  if (!closed.url.is_valid() || closed.url.IsAboutBlank()) {
    return;
  }
  closed_.push_back(std::move(closed));
  while (closed_.size() > kMaxClosed) {
    closed_.pop_front();
  }
}

void LrbPlatformDelegate::ReopenClosed(Window* window) {
  if (closed_.empty()) {
    return;
  }
  TabState tab = std::move(closed_.back());
  closed_.pop_back();
  next_ = Next();
  next_.window = window;
  LoadSavedTab(window->context, tab);
}

void LrbPlatformDelegate::NewWindow(Window* window) {
  WindowView* view = ViewOf(window);
  next_ = Next();
  next_.new_window = true;
  Shell::CreateNewWindow(window->context, view->NewTabUrl(), nullptr,
                         gfx::Size());
}

void LrbPlatformDelegate::OnWidgetActivationChanged(views::Widget* widget,
                                                    bool active) {
  if (!active) {
    return;
  }
  for (const std::unique_ptr<Window>& window : windows_) {
    if (window->widget.get() == widget) {
      last_active_ = window.get();
    }
  }
}

void LrbPlatformDelegate::OnWindowClosed(Window* window) {
  if (!Exists(window)) {
    return;
  }
  // The close callback is used up: set it again, or a second click on the
  // close button (after "Stay") would tear the window down under the page.
  ArmCloseButton(window);
  // Not from within the native window's close event: closing the last
  // window shuts the browser down, which pumps a nested message loop, and
  // on Wayland that can wait forever for the event reader the outer
  // dispatch holds (the process stayed alive with no window).
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<LrbPlatformDelegate> self, Window* window) {
            if (self && self->Exists(window)) {
              self->CloseWindow(window);
            }
          },
          weak_factory_.GetWeakPtr(), base::Unretained(window)));
}

void LrbPlatformDelegate::CloseWindow(Window* window) {
  // Tabs not loaded have nothing to lose; each page may ask first.
  for (const Tab& tab : window->tabs) {
    if (!tab.shell) {
      RememberClosed(tab);
    }
  }
  std::erase_if(window->tabs, [](const Tab& tab) { return !tab.shell; });
  window->active = TabOf(window, ViewOf(window)->shell()).value_or(0);
  std::vector<Shell*> live;
  for (const Tab& tab : window->tabs) {
    live.push_back(tab.shell);
  }
  for (Shell* shell : live) {
    if (WindowOf(shell)) {
      CloseByUser(shell);
    }
  }
}

void LrbPlatformDelegate::ArmCloseButton(Window* window) {
  window->widget->MakeCloseSynchronous(base::BindOnce(
      [](base::WeakPtr<LrbPlatformDelegate> delegate, Window* window,
         views::Widget::ClosedReason) {
        if (delegate) {
          delegate->OnWindowClosed(window);
        }
      },
      weak_factory_.GetWeakPtr(), base::Unretained(window)));
}

void LrbPlatformDelegate::SetContents(Shell* shell) {
  Window* window = WindowOf(shell);
  DCHECK(window);
  views::WebContentsSetBackgroundColor::CreateForWebContentsWithColor(
      shell->web_contents(),
      window->widget->GetColorProvider()->GetColor(ui::kColorWindowBackground));
  const size_t index = *TabOf(window, shell);
  if (!window->shown) {
    window->shown = true;
    window->active = index;
    ViewOf(window)->ShowTab(shell, window->tabs[index].back_url,
                            window->content_size);
    UpdateTabs(window);
    window->widget->GetNativeWindow()->GetHost()->Show();
    window->widget->Show();
    return;
  }
  Activate(window, index);  // a new tab comes to the front
}

void LrbPlatformDelegate::EnableUIControl(Shell* shell,
                                          UIControl control,
                                          bool is_enabled) {
  Window* window = WindowOf(shell);
  WindowView* view = window ? ViewOf(window) : nullptr;
  if (!view || view->shell() != shell) {
    return;  // a tab not shown: the bar follows the shown one
  }
  if (control == BACK_BUTTON) {
    view->EnableBack(is_enabled);
  } else if (control == FORWARD_BUTTON) {
    view->EnableForward(is_enabled);
  }
  // STOP_BUTTON: SetIsLoading() switches reload to stop.
}

void LrbPlatformDelegate::SetAddressBarURL(Shell* shell,
                                           const GURL& url) {
  Window* window = WindowOf(shell);
  if (!window) {
    return;
  }
  window->tabs[*TabOf(window, shell)].url = url;
  WindowView* view = ViewOf(window);
  if (view->shell() == shell) {
    view->SetUrl(url);
  }
  SessionChanged(window->context);
}

void LrbPlatformDelegate::SetIsLoading(Shell* shell, bool loading) {
  Window* window = WindowOf(shell);
  if (!window) {
    return;
  }
  WindowView* view = ViewOf(window);
  if (view->shell() == shell) {
    view->SetLoading(loading);
    if (!loading) {
      // A restored page commits with the title it already had, which Shell
      // doesn't report: the window would stay untitled.
      window->widget->widget_delegate()->SetTitle(
          shell->web_contents()->GetTitle());
    }
  }
  if (!loading) {
    UpdateTabs(window);
  }
}

void LrbPlatformDelegate::SetTitle(Shell* shell,
                                   const std::u16string& title) {
  Window* window = WindowOf(shell);
  if (!window) {
    return;
  }
  window->tabs[*TabOf(window, shell)].title = title;
  if (ViewOf(window)->shell() == shell) {
    window->widget->widget_delegate()->SetTitle(title);
  }
  UpdateTabs(window);
}

void LrbPlatformDelegate::RequestMediaAccessPermission(
    Shell* shell,
    content::WebContents* web_contents,
    const content::MediaStreamRequest& request,
    content::MediaResponseCallback callback) {
  using blink::mojom::MediaStreamRequestResult;
  using blink::mojom::MediaStreamType;
  const bool audio = request.audio_type == MediaStreamType::DEVICE_AUDIO_CAPTURE;
  const bool video = request.video_type == MediaStreamType::DEVICE_VIDEO_CAPTURE;
  const bool other =
      (request.audio_type != MediaStreamType::NO_SERVICE && !audio) ||
      (request.video_type != MediaStreamType::NO_SERVICE && !video);
  content::RenderFrameHost* frame = content::RenderFrameHost::FromID(
      request.render_process_id, request.render_frame_id);
  if (other || (!audio && !video) || !frame || !WindowOf(shell)) {
    // Screen and tab capture: not offered.
    std::move(callback).Run(blink::mojom::StreamDevicesSet(),
                            MediaStreamRequestResult::NOT_SUPPORTED, nullptr);
    return;
  }
  std::vector<blink::PermissionType> types;
  if (video) {
    types.push_back(blink::PermissionType::VIDEO_CAPTURE);
  }
  if (audio) {
    types.push_back(blink::PermissionType::AUDIO_CAPTURE);
  }
  PermissionManagerOf(web_contents)
      .Decide(
          frame, types, request.user_gesture,
          base::BindOnce(
              [](base::WeakPtr<WindowView> view,
                 content::MediaStreamRequest request, bool audio, bool video,
                 content::MediaResponseCallback callback,
                 const std::vector<blink::mojom::PermissionStatus>& statuses) {
                for (blink::mojom::PermissionStatus status : statuses) {
                  if (status != blink::mojom::PermissionStatus::GRANTED) {
                    std::move(callback).Run(
                        blink::mojom::StreamDevicesSet(),
                        MediaStreamRequestResult::PERMISSION_DENIED, nullptr);
                    return;
                  }
                }
                content::MediaCaptureDevices* all =
                    content::MediaCaptureDevices::GetInstance();
                auto devices = blink::mojom::StreamDevices::New();
                if (audio) {
                  devices->audio_device =
                      PickDevice(all->GetAudioCaptureDevices(),
                                 request.requested_audio_device_ids);
                }
                if (video) {
                  devices->video_device =
                      PickDevice(all->GetVideoCaptureDevices(),
                                 request.requested_video_device_ids);
                }
                if ((audio && !devices->audio_device) ||
                    (video && !devices->video_device)) {
                  std::move(callback).Run(
                      blink::mojom::StreamDevicesSet(),
                      MediaStreamRequestResult::NO_HARDWARE, nullptr);
                  return;
                }
                blink::mojom::StreamDevicesSet set;
                set.stream_devices.push_back(std::move(devices));
                std::move(callback).Run(
                    set, MediaStreamRequestResult::OK,
                    std::make_unique<CaptureIndicator>(view, audio, video));
              },
              ViewOf(WindowOf(shell))->GetWeakPtr(), request,
              audio, video, std::move(callback)));
}

bool LrbPlatformDelegate::CheckMediaAccessPermission(
    Shell* shell,
    content::RenderFrameHost* render_frame_host,
    const url::Origin& security_origin,
    blink::mojom::MediaStreamType type) {
  // Device labels (enumerateDevices) only once the user allowed the device.
  const blink::PermissionType permission =
      type == blink::mojom::MediaStreamType::DEVICE_AUDIO_CAPTURE
          ? blink::PermissionType::AUDIO_CAPTURE
          : blink::PermissionType::VIDEO_CAPTURE;
  return render_frame_host &&
         PermissionManagerOf(shell->web_contents())
                 .Status(permission, security_origin,
                         render_frame_host->GetMainFrame()
                             ->GetLastCommittedOrigin()) ==
             blink::mojom::PermissionStatus::GRANTED;
}

std::unique_ptr<content::JavaScriptDialogManager>
LrbPlatformDelegate::CreateJavaScriptDialogManager(Shell* shell) {
  return std::make_unique<LrbJavaScriptDialogManager>(shell);
}

void LrbPlatformDelegate::RunFileChooser(
    content::RenderFrameHost* render_frame_host,
    scoped_refptr<content::FileSelectListener> listener,
    const blink::mojom::FileChooserParams& params) {
  using Mode = blink::mojom::FileChooserParams::Mode;
  ui::SelectFileDialog::Type type = ui::SelectFileDialog::SELECT_OPEN_FILE;
  std::u16string title = u"Choose a file to upload";
  switch (params.mode) {
    case Mode::kOpen:
      break;
    case Mode::kOpenMultiple:
      type = ui::SelectFileDialog::SELECT_OPEN_MULTI_FILE;
      title = u"Choose files to upload";
      break;
    case Mode::kUploadFolder:
      type = ui::SelectFileDialog::SELECT_UPLOAD_FOLDER;
      title = u"Choose a folder to upload";
      break;
    case Mode::kOpenDirectory:
      type = ui::SelectFileDialog::SELECT_EXISTING_FOLDER;
      title = u"Choose a folder";
      break;
    case Mode::kSave:
      type = ui::SelectFileDialog::SELECT_SAVEAS_FILE;
      title = u"Save file";
      break;
  }
  Shell* shell = Shell::FromWebContents(
      content::WebContents::FromRenderFrameHost(render_frame_host));
  PickFiles(
      type, title, params.default_file_name,
      shell ? shell->window() : gfx::NativeWindow(),
      base::BindOnce(
          [](scoped_refptr<content::FileSelectListener> listener, Mode mode,
             base::WeakPtr<LrbPlatformDelegate> self, Shell* shell,
             PickResult result, std::vector<base::FilePath> paths) {
            if (result != PickResult::kChosen) {
              listener->FileSelectionCanceled();
              if (result == PickResult::kUnavailable && self) {
                // Say why nothing happened.
                if (WindowView* view = ViewFor(shell)) {
                  WindowView::Question question;
                  question.text =
                      u"Files can't be chosen here: this system has no file "
                      u"picker (no desktop portal).";
                  question.accept_label = u"OK";
                  question.answer = base::DoNothing();
                  view->Ask(std::move(question));
                }
              }
              return;
            }
            if (mode != Mode::kUploadFolder) {
              listener->FileSelected(NativeFiles(paths), base::FilePath(),
                                     mode);
              return;
            }
            // A folder to upload: its files, listed off the UI thread.
            const base::FilePath folder = paths.front();
            base::ThreadPool::PostTaskAndReplyWithResult(
                FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
                base::BindOnce(
                    [](base::FilePath folder) {
                      std::vector<base::FilePath> files;
                      base::FileEnumerator walk(folder, /*recursive=*/true,
                                                base::FileEnumerator::FILES);
                      for (base::FilePath file = walk.Next(); !file.empty();
                           file = walk.Next()) {
                        files.push_back(file);
                      }
                      return files;
                    },
                    folder),
                base::BindOnce(
                    [](scoped_refptr<content::FileSelectListener> listener,
                       base::FilePath folder, Mode mode,
                       std::vector<base::FilePath> files) {
                      listener->FileSelected(NativeFiles(files), folder, mode);
                    },
                    std::move(listener), folder, mode));
          },
          std::move(listener), params.mode, weak_factory_.GetWeakPtr(),
          base::Unretained(shell)));
}

bool LrbPlatformDelegate::OpenURLFromTab(Shell* shell,
                                         content::WebContents* source,
                                         const content::OpenURLParams& params) {
  if (params.disposition != WindowOpenDisposition::NEW_BACKGROUND_TAB ||
      !params.url.SchemeIsHTTPOrHTTPS() || params.post_data) {
    return false;  // a form's POST can't wait in a tab not loaded
  }
  LrbContentBrowserClient* client = LrbContentBrowserClient::Get();
  const std::string site = SiteForUrl(params.url);
  if (client && site != client->site()) {
    client->OpenSiteWindow(site, params.url);  // another site's window
    return true;
  }
  return AddBackgroundTab(shell, params.url);
}

void LrbPlatformDelegate::FindReply(Shell* shell,
                                    int request_id,
                                    int number_of_matches,
                                    int active_match_ordinal,
                                    bool final_update) {
  Window* window = WindowOf(shell);
  if (window && ViewOf(window)->shell() == shell) {
    ViewOf(window)->OnFindReply(number_of_matches, active_match_ordinal,
                                final_update);
  }
}

void LrbPlatformDelegate::ActivateContents(Shell* shell,
                                           content::WebContents* contents) {
  ShowTabFor(shell);
  contents->Focus();
}

bool LrbPlatformDelegate::DestroyShell(Shell* shell) {
  return false;  // the Shell deletes itself; CleanUp() destroys the window
}

}  // namespace lrb
