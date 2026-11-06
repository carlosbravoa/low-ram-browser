// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/ui/about_dialog.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/environment.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/path_service.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "build/build_config.h"
#include "components/version_info/version_info.h"
#include "lrb/browser/lrb_content_browser_client.h"
#include "lrb/browser/site.h"
#include "lrb/browser/ui/lrb_platform_delegate.h"
#include "lrb/browser/ui/snap_connections_dialog.h"
#include "lrb/common/lrb_switches.h"
#include "lrb/common/mode.h"
#include "lrb/common/version.h"
#include "net/base/filename_util.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/models/image_model.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/base/mojom/ui_base_types.mojom-shared.h"
#include "ui/color/color_id.h"
#include "ui/gfx/geometry/size.h"
#include "ui/views/controls/image_view.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/link.h"
#include "ui/views/controls/scroll_view.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/style/typography.h"
#include "ui/views/widget/widget.h"
#include "ui/views/window/dialog_delegate.h"
#include "url/gurl.h"

namespace lrb {

namespace {

constexpr char kRepository[] = "https://github.com/carlosbravoa/low-ram-browser";
constexpr char kIssues[] =
    "https://github.com/carlosbravoa/low-ram-browser/issues";
constexpr int kIconSize = 64;
// Chromium's releases for Linux, to compare with the version lrb is on.
constexpr char kChromiumReleases[] =
    "https://chromiumdash.appspot.com/releases?platform=Linux";

// Opens an http(s) page in its site's window (another site's, as a link
// asking for a new window would).
void OpenSite(const GURL& url) {
  if (LrbContentBrowserClient* client = LrbContentBrowserClient::Get()) {
    client->OpenSiteWindow(SiteForUrl(url), url);
  }
}

// What a problem report needs beyond the versions: the display system
// the windows are on, the CPU this build is for, and the package.
std::u16string RunningOn() {
  // The coordinator gives --ozone-platform=wayland on a Wayland desktop
  // (ui/ozone's switch); without it Chromium's windows are X11's.
  const std::string platform =
      base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
          "ozone-platform");
  const std::string display =
      platform.empty() || platform == "x11"
          ? "X11"
          : platform == "wayland" ? "Wayland" : platform;
#if defined(ARCH_CPU_ARM64)
  const char cpu[] = "arm64";
#elif defined(ARCH_CPU_X86_64)
  const char cpu[] = "x86-64";
#else
  const char cpu[] = "another CPU";
#endif
  std::string package = "not as a snap";
  if (RunningAsSnap()) {
    // snapd numbers what it installs; "x1" and so on for a local file.
    const std::string revision =
        base::Environment::Create()->GetVar("SNAP_REVISION").value_or("");
    package = revision.empty()
                  ? "as a snap"
                  : base::StrCat({"as a snap (revision ", revision, ")"});
  }
  return base::UTF8ToUTF16(
      base::StrCat({"Running on ", display, ", ", cpu, ", ", package}));
}

// The window's contents. `close` closes the window (after a link opened
// something: the window is modal to the one the credits open in).
class AboutView : public views::View {
  METADATA_HEADER(AboutView, views::View)

 public:
  AboutView(const base::FilePath& credits,
            base::RepeatingCallback<void(const GURL&)> open_tab,
            base::RepeatingClosure close) {
    SetLayoutManager(std::make_unique<views::BoxLayout>(
                         views::BoxLayout::Orientation::kVertical,
                         gfx::Insets::VH(16, 20), 8))
        ->set_cross_axis_alignment(
            views::BoxLayout::CrossAxisAlignment::kStretch);

    // The icon, with the name and the version beside it: single lines,
    // whose height doesn't depend on the width they get (a wrapped label
    // in this row was measured at no width: a window of blank space).
    auto* header = AddChildView(std::make_unique<views::View>());
    header
        ->SetLayoutManager(std::make_unique<views::BoxLayout>(
            views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 14))
        ->set_cross_axis_alignment(
            views::BoxLayout::CrossAxisAlignment::kCenter);
    if (const ui::ImageModel icon = AppIcon(); !icon.IsEmpty()) {
      auto* image = header->AddChildView(std::make_unique<views::ImageView>());
      image->SetImage(icon);
      image->SetImageSize(gfx::Size(kIconSize, kIconSize));
    }
    auto* name = header->AddChildView(std::make_unique<views::View>());
    name->SetLayoutManager(std::make_unique<views::BoxLayout>(
                               views::BoxLayout::Orientation::kVertical,
                               gfx::Insets(), 4))
        ->set_cross_axis_alignment(
            views::BoxLayout::CrossAxisAlignment::kStart);
    Heading(u"Low RAM Browser (lrb)", name);
    const std::string version(Version());
    auto* version_label = name->AddChildView(std::make_unique<views::Label>(
        version.empty()
            ? u"A development build"
            : base::UTF8ToUTF16(base::StrCat({"Version ", version})),
        views::style::CONTEXT_LABEL, views::style::STYLE_PRIMARY));
    version_label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    version_label->SetSelectable(true);  // to copy into a report

    Text(u"A Chromium-based browser with the smallest memory footprint that "
         u"still browses the mainstream web.");
    const std::string released(ChromiumReleased());
    Text(base::UTF8ToUTF16(base::StrCat(
        {"Chromium ", version_info::GetVersionNumber(),
         released.empty() ? "" : ", released to Stable on ", released})));
    Text(StandardMode()
             ? u"Running in standard mode: Chromium's sandbox and site "
               u"isolation"
             : u"Running in lean mode: one confined process per site");
    if (!StandardMode()) {
      LeanReason(base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
          switches::kLeanReason));
    }
    if (const std::string source(SourceCommit()); !source.empty()) {
      Note(base::UTF8ToUTF16(base::StrCat({"Built from source ", source})));
    }
    Note(RunningOn());

    Text(u"© 2026 Carlos Bravo. Free software under the BSD 3-Clause "
         u"License.");
    // The source, how to build it, every release; and the issues, for
    // which the lines above (selectable) are what a report needs.
    AddLink(u"github.com/carlosbravoa/low-ram-browser",
            base::BindRepeating(&OpenSite, GURL(kRepository)), close);
    AddLink(u"Report a problem",
            base::BindRepeating(&OpenSite, GURL(kIssues)), close);

    Note(u"Chromium's security fixes reach lrb with its releases. Compare "
         u"with Chromium's latest:");
    AddLink(u"Chromium releases for Linux",
            base::BindRepeating(&OpenSite, GURL(kChromiumReleases)), close);
    Note(u"Built on Chromium (Copyright The Chromium Authors, BSD 3-Clause "
         u"License) and Brave's adblock-rust (Mozilla Public License 2.0), "
         u"with other open-source components under their own licenses.");
    if (!credits.empty()) {
      AddLink(u"Licenses of all components",
              base::BindRepeating(open_tab, net::FilePathToFileURL(credits)),
              close);
    }
  }

 private:
  // `parent`: where the label goes; null: this view.
  void Heading(const std::u16string& text, views::View* parent = nullptr) {
    auto* label = (parent ? parent : this)
                      ->AddChildView(std::make_unique<views::Label>(
                          text, views::style::CONTEXT_LABEL,
                          views::style::STYLE_EMPHASIZED));
    label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  }

  void Text(const std::u16string& text) {
    auto* label = AddChildView(std::make_unique<views::Label>(
        text, views::style::CONTEXT_LABEL, views::style::STYLE_PRIMARY));
    label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    label->SetMultiLine(true);
    label->SetSelectable(true);  // to copy the version into a report
  }

  void Note(const std::u16string& text) {
    auto* label = AddChildView(std::make_unique<views::Label>(
        text, views::style::CONTEXT_LABEL, views::style::STYLE_SECONDARY));
    label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    label->SetMultiLine(true);
    label->SetSelectable(true);
  }

  // Why lean mode, as the coordinator decided it (--lrb-lean-reason).
  void LeanReason(const std::string& reason) {
    if (reason == "sandbox" || reason == "refused") {
      Note(u"Standard mode is unavailable here: this system lets only its "
           u"own packages create user namespaces, which Chromium's sandbox "
           u"needs. The snap package has standard mode.");
    } else if (reason == "memory") {
      Note(u"Standard mode needs 3 GB of RAM.");
    } else if (reason == "available") {
      Note(u"Standard mode needs 1.5 GB free when lrb starts.");
    } else if (reason == "snap") {
      Note(u"Standard mode needs this snap's browser-sandbox connection: "
           u"open Snap connections in the menu.");
    } else if (reason == "option") {
      Note(u"Lean mode was chosen in Settings.");
    }
  }

  void AddLink(const std::u16string& text,
               base::RepeatingClosure open,
               base::RepeatingClosure close) {
    auto* link = AddChildView(std::make_unique<views::Link>(text));
    link->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    link->SetCallback(base::BindRepeating(
        [](base::RepeatingClosure open, base::RepeatingClosure close) {
          open.Run();
          close.Run();
        },
        std::move(open), std::move(close)));
  }
};

BEGIN_METADATA(AboutView)
END_METADATA

// Owns the dialog's delegate and widget (CLIENT_OWNS_WIDGET, as lrb's
// windows), and itself, until the window closes.
class AboutWindow {
 public:
  AboutWindow(const base::FilePath& credits,
              base::RepeatingCallback<void(const GURL&)> open_tab,
              gfx::NativeView parent) {
    // The dialog is drawn inside the browser window, and must fit in it: on
    // Wayland a child window takes no clicks beyond its parent's edge.
    views::Widget* parent_widget =
        views::Widget::GetWidgetForNativeView(parent);
    const gfx::Size room =
        parent_widget ? parent_widget->GetClientAreaBoundsInScreen().size() -
                            gfx::Size(24, 24)
                      : gfx::Size(480, 10000);
    delegate_ = std::make_unique<views::DialogDelegate>();
    // In a scroll view, for a window too short for it all.
    auto* scroll = delegate_->SetContentsView(
        std::make_unique<views::ScrollView>());
    scroll->SetHorizontalScrollBarMode(
        views::ScrollView::ScrollBarMode::kDisabled);
    // Always with a height limit, lowered below for a short window: a
    // ScrollView without one never sizes its contents (they stay 0 x 0, the
    // window blank).
    scroll->ClipHeightTo(0, 10000);
    // Opaque, as the dialog: text on a transparent layer draws heavier.
    scroll->SetBackgroundColor(ui::kColorDialogBackground);
    scroll->SetContents(std::make_unique<AboutView>(
        credits, std::move(open_tab),
        base::BindRepeating(&AboutWindow::Close,
                            weak_factory_.GetWeakPtr())));
    delegate_->SetTitle(u"About Low RAM Browser");
    // A fixed width, so the wrapped paragraphs' heights count (else the
    // last line is cut off); narrower in a narrow window.
    delegate_->set_fixed_width(std::max(std::min(480, room.width()), 320));
    delegate_->SetModalType(ui::mojom::ModalType::kWindow);
    delegate_->SetButtons(
        static_cast<int>(ui::mojom::DialogButton::kOk));
    delegate_->SetButtonLabel(ui::mojom::DialogButton::kOk, u"Close");
    views::Widget::InitParams params =
        views::DialogDelegate::GetDialogWidgetInitParams(
            delegate_.get(), gfx::NativeWindow(), parent, gfx::Rect());
    params.ownership = views::Widget::InitParams::CLIENT_OWNS_WIDGET;
    widget_ = std::make_unique<views::Widget>();
    widget_->Init(std::move(params));
    // Too tall for the browser window: the text gives up the excess (it
    // scrolls), before the window is shown.
    const int excess = widget_->GetWindowBoundsInScreen().height() -
                       room.height();
    if (excess > 0) {
      widget_->GetRootView()->DeprecatedLayoutImmediately();
      scroll->ClipHeightTo(0, std::max(scroll->height() - excess, 80));
      const gfx::Size size = widget_->non_client_view()->GetPreferredSize();
      widget_->SetSize(size);
      widget_->CenterWindow(size);
    }
    // Closing (Close, the close button, a link) deletes all this, after the
    // event that closed it: not inside its own window's event.
    widget_->MakeCloseSynchronous(base::BindOnce(
        [](AboutWindow* window, views::Widget::ClosedReason) {
          base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
              FROM_HERE, window);
        },
        base::Unretained(this)));
    widget_->Show();
  }

  ~AboutWindow() {
    widget_->CloseNow();  // the native window too, now (see lrb windows)
    widget_.reset();      // the widget before its delegate
  }

 private:
  void Close() { widget_->Close(); }  // deletes this, as above

  std::unique_ptr<views::DialogDelegate> delegate_;
  std::unique_ptr<views::Widget> widget_;
  base::WeakPtrFactory<AboutWindow> weak_factory_{this};
};

}  // namespace

void ShowAbout(base::WeakPtr<views::View> parent,
               base::RepeatingCallback<void(const GURL&)> open_tab) {
  // Whether the credits are there (off the UI thread), then the window.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce([] {
        base::FilePath dir;
        if (!base::PathService::Get(base::DIR_EXE, &dir)) {
          return base::FilePath();
        }
        const base::FilePath credits = dir.Append("credits.html");
        return base::PathExists(credits) ? credits : base::FilePath();
      }),
      base::BindOnce(
          [](base::WeakPtr<views::View> parent,
             base::RepeatingCallback<void(const GURL&)> open_tab,
             base::FilePath credits) {
            if (!parent || !parent->GetWidget()) {
              return;  // its window closed meanwhile
            }
            new AboutWindow(credits, std::move(open_tab),
                            parent->GetWidget()->GetNativeView());
          },
          parent, std::move(open_tab)));
}

}  // namespace lrb
