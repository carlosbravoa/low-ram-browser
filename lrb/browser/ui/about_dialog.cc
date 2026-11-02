// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/ui/about_dialog.h"

#include <memory>
#include <string>
#include <utility>

#include "base/base_paths.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/path_service.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "components/version_info/version_info.h"
#include "lrb/browser/lrb_content_browser_client.h"
#include "lrb/browser/site.h"
#include "lrb/common/mode.h"
#include "lrb/common/version.h"
#include "net/base/filename_util.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/base/mojom/ui_base_types.mojom-shared.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/link.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/style/typography.h"
#include "ui/views/widget/widget.h"
#include "ui/views/window/dialog_delegate.h"
#include "url/gurl.h"

namespace lrb {

namespace {

constexpr char kRepository[] = "https://github.com/carlosbravoa/low-ram-browser";
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

    Heading(u"low-ram-browser (lrb)");
    const std::string version(Version());
    const std::string source(SourceCommit());
    Text(version.empty()
             ? u"A development build"
             : base::UTF8ToUTF16(base::StrCat({"Version ", version})));
    if (!source.empty()) {
      Note(base::UTF8ToUTF16(base::StrCat({"Built from source ", source})));
    }
    const std::string released(ChromiumReleased());
    Text(base::UTF8ToUTF16(base::StrCat(
        {"Chromium ", version_info::GetVersionNumber(),
         released.empty() ? "" : ", released to Stable on ", released})));
    Text(StandardMode()
             ? u"Running in standard mode: Chromium's sandbox and site "
               u"isolation"
             : u"Running in lean mode: one confined process per site");
    Note(u"Chromium's security fixes reach lrb with its releases. Compare "
         u"with Chromium's latest release:");
    AddLink(u"Chromium releases for Linux",
            base::BindRepeating(&OpenSite, GURL(kChromiumReleases)), close);

    Heading(u"Source");
    AddLink(u"github.com/carlosbravoa/low-ram-browser",
            base::BindRepeating(&OpenSite, GURL(kRepository)), close);
    Note(u"The source code, how to build it, and every release.");

    Heading(u"Copyright and licenses");
    Text(u"Copyright 2026 The low-ram-browser Authors. Free software under "
         u"the BSD 3-Clause License.");
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
  void Heading(const std::u16string& text) {
    auto* label = AddChildView(std::make_unique<views::Label>(
        text, views::style::CONTEXT_LABEL, views::style::STYLE_EMPHASIZED));
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
    delegate_ = std::make_unique<views::DialogDelegate>();
    delegate_->SetContentsView(std::make_unique<AboutView>(
        credits, std::move(open_tab),
        base::BindRepeating(&AboutWindow::Close,
                            weak_factory_.GetWeakPtr())));
    delegate_->SetTitle(u"About lrb");
    // A fixed width, so the wrapped paragraphs' heights count (else the
    // last line is cut off).
    delegate_->set_fixed_width(480);
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
