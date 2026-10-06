// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/ui/context_menu.h"

#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/referrer.h"
#include "lrb/browser/shell.h"
#include "lrb/browser/lrb_content_browser_client.h"
#include "lrb/browser/site.h"
#include "lrb/browser/ui/window_view.h"
#include "third_party/blink/public/common/context_menu_data/edit_flags.h"
#include "third_party/blink/public/mojom/context_menu/context_menu.mojom.h"
#include "ui/aura/client/screen_position_client.h"
#include "ui/aura/window.h"
#include "ui/base/clipboard/scoped_clipboard_writer.h"
#include "ui/views/controls/menu/menu_runner.h"
#include "ui/views/widget/widget.h"

namespace lrb {

namespace {

enum Command {
  kOpenLink = 1,
  kCopyLink,
  kSaveImage,
  kCopyImageAddress,
  kSearch,
  kUndo,
  kCut,
  kCopy,
  kPaste,
  kSelectAll,
  kBack,
  kForward,
  kReload,
};

void CopyText(const std::u16string& text) {
  ui::ScopedClipboardWriter(ui::ClipboardBuffer::kCopyPaste).WriteText(text);
}

// Opens `url` in a new window: this site's, or the url's site's own.
void OpenInNewWindow(content::WebContents* contents, const GURL& url) {
  LrbContentBrowserClient* client = LrbContentBrowserClient::Get();
  const std::string site = SiteForUrl(url);
  if (!site.empty() && client && site != client->site()) {
    client->OpenSiteWindow(site, url);
    return;
  }
  Shell::CreateNewWindow(contents->GetBrowserContext(), url, nullptr,
                                  gfx::Size());
}

}  // namespace

LrbWebContentsViewDelegate::LrbWebContentsViewDelegate(
    content::WebContents* web_contents)
    : web_contents_(web_contents) {}

LrbWebContentsViewDelegate::~LrbWebContentsViewDelegate() = default;

void LrbWebContentsViewDelegate::ShowContextMenu(
    content::RenderFrameHost& render_frame_host,
    const content::ContextMenuParams& params) {
  params_ = params;
  frame_ = render_frame_host.GetGlobalId();
  model_.Clear();
  using MediaType = blink::mojom::ContextMenuDataMediaType;
  const bool link = params.link_url.is_valid();
  const bool image =
      params.media_type == MediaType::kImage && params.src_url.is_valid();
  const bool selection = !params.selection_text.empty();

  if (link) {
    model_.AddItem(kOpenLink, u"Open link in new window");
    model_.AddItem(kCopyLink, u"Copy link address");
  }
  if (image) {
    if (model_.GetItemCount()) {
      model_.AddSeparator(ui::NORMAL_SEPARATOR);
    }
    model_.AddItem(kSaveImage, u"Save image as...");
    model_.AddItem(kCopyImageAddress, u"Copy image address");
  }
  if (params.is_editable) {
    if (model_.GetItemCount()) {
      model_.AddSeparator(ui::NORMAL_SEPARATOR);
    }
    model_.AddItem(kUndo, u"Undo");
    model_.AddSeparator(ui::NORMAL_SEPARATOR);
    model_.AddItem(kCut, u"Cut");
    model_.AddItem(kCopy, u"Copy");
    model_.AddItem(kPaste, u"Paste");
    model_.AddSeparator(ui::NORMAL_SEPARATOR);
    model_.AddItem(kSelectAll, u"Select all");
  } else if (selection) {
    if (model_.GetItemCount()) {
      model_.AddSeparator(ui::NORMAL_SEPARATOR);
    }
    model_.AddItem(kCopy, u"Copy");
    std::u16string shown = params.selection_text.substr(0, 30);
    if (shown.size() < params.selection_text.size()) {
      shown += u"…";
    }
    model_.AddItem(kSearch, u"Search for “" + shown + u"”");
  }
  if (!model_.GetItemCount()) {
    model_.AddItem(kBack, u"Back");
    model_.AddItem(kForward, u"Forward");
    model_.AddItem(kReload, u"Reload");
  }

  // From page coordinates to the screen (as content_shell does).
  gfx::Point point(params.x, params.y);
  aura::Window* window = web_contents_->GetNativeView();
  if (aura::client::ScreenPositionClient* screen =
          aura::client::GetScreenPositionClient(window->GetRootWindow())) {
    screen->ConvertPointToScreen(window, &point);
  }
  runner_ = std::make_unique<views::MenuRunner>(
      &model_, views::MenuRunner::CONTEXT_MENU);
  runner_->RunMenuAt(views::Widget::GetWidgetForNativeView(
                         web_contents_->GetTopLevelNativeWindow()),
                     nullptr, gfx::Rect(point, gfx::Size()),
                     views::MenuAnchorPosition::kTopLeft,
                     ui::mojom::MenuSourceType::kMouse);
}

void LrbWebContentsViewDelegate::DismissContextMenu() {
  if (runner_) {
    runner_->Cancel();
  }
}

bool LrbWebContentsViewDelegate::IsContextMenuShowingForTesting() {
  return runner_ && runner_->IsRunning();
}

bool LrbWebContentsViewDelegate::IsCommandIdEnabled(int command_id) const {
  const int flags = params_.edit_flags;
  content::NavigationController& history = web_contents_->GetController();
  switch (command_id) {
    case kUndo:
      return flags & blink::ContextMenuDataEditFlags::kCanUndo;
    case kCut:
      return flags & blink::ContextMenuDataEditFlags::kCanCut;
    case kCopy:
      return !params_.is_editable ||
             (flags & blink::ContextMenuDataEditFlags::kCanCopy);
    case kPaste:
      return flags & blink::ContextMenuDataEditFlags::kCanPaste;
    case kSelectAll:
      return flags & blink::ContextMenuDataEditFlags::kCanSelectAll;
    case kBack:
      return history.CanGoBack();
    case kForward:
      return history.CanGoForward();
  }
  return true;
}

void LrbWebContentsViewDelegate::ExecuteCommand(int command_id,
                                                int event_flags) {
  switch (command_id) {
    case kOpenLink:
      OpenInNewWindow(web_contents_, params_.link_url);
      break;
    case kCopyLink:
      CopyText(base::UTF8ToUTF16(params_.link_url.spec()));
      break;
    case kSaveImage:
      // A download (asks where to save), from the page that shows it.
      if (content::RenderFrameHost* frame =
              content::RenderFrameHost::FromID(frame_)) {
        web_contents_->SaveFrame(
            params_.src_url,
            content::Referrer(frame->GetLastCommittedURL(),
                              params_.referrer_policy),
            frame);
      }
      break;
    case kCopyImageAddress:
      CopyText(base::UTF8ToUTF16(params_.src_url.spec()));
      break;
    case kSearch:
      WindowView::Search(
          params_.selection_text,
          base::BindOnce(
              [](base::WeakPtr<content::WebContents> contents, GURL url) {
                if (contents) {
                  OpenInNewWindow(contents.get(), url);
                }
              },
              web_contents_->GetWeakPtr()));
      break;
    case kUndo:
      web_contents_->Undo();
      break;
    case kCut:
      web_contents_->Cut();
      break;
    case kCopy:
      web_contents_->Copy();
      break;
    case kPaste:
      web_contents_->Paste();
      break;
    case kSelectAll:
      web_contents_->SelectAll();
      break;
    case kBack:
      web_contents_->GetController().GoBack();
      break;
    case kForward:
      web_contents_->GetController().GoForward();
      break;
    case kReload:
      web_contents_->GetController().Reload(content::ReloadType::NORMAL,
                                            false);
      break;
  }
}

}  // namespace lrb
