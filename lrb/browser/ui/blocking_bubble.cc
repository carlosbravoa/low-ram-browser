// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/ui/blocking_bubble.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/views/bubble/bubble_border.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"
#include "ui/views/controls/button/md_text_button.h"
#include "ui/views/controls/label.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/style/typography.h"
#include "ui/views/view.h"
#include "ui/views/widget/widget.h"

namespace lrb {

namespace {

// Owns the bubble's delegate and widget (CLIENT_OWNS_WIDGET, as lrb's
// windows), and itself, until the bubble closes.
class BlockingBubble {
 public:
  BlockingBubble(views::View* anchor,
                 int blocked,
                 const std::string& site,
                 std::optional<bool> enabled,
                 base::OnceClosure toggle,
                 base::OnceClosure settings)
      : toggle_(std::move(toggle)), settings_(std::move(settings)) {
    // Under the shield, opening towards the window's middle (the shield is
    // at the bar's right end).
    delegate_ = std::make_unique<views::BubbleDialogDelegate>(
        anchor, views::BubbleBorder::TOP_RIGHT);
    delegate_->SetTitle(u"Content blocking");
    delegate_->SetShowCloseButton(true);
    delegate_->SetButtons(static_cast<int>(ui::mojom::DialogButton::kNone));
    // Narrower in a narrow window: it must fit inside it (on Wayland a
    // child takes no clicks beyond its parent's edge).
    int width = 340;
    if (views::Widget* window = anchor->GetWidget()) {
      width = std::clamp(window->GetClientAreaBoundsInScreen().width() - 24,
                         200, width);
    }
    delegate_->set_fixed_width(width);

    auto contents = std::make_unique<views::View>();
    contents->SetLayoutManager(std::make_unique<views::BoxLayout>(
                                   views::BoxLayout::Orientation::kVertical,
                                   gfx::Insets(), 10))
        ->set_cross_axis_alignment(
            views::BoxLayout::CrossAxisAlignment::kStretch);
    const std::u16string site16 = base::UTF8ToUTF16(site);
    Text(contents.get(),
         base::NumberToString16(blocked) +
             (blocked == 1 ? u" request" : u" requests") +
             u" blocked on this page: ads, trackers and malware, by the "
             u"filter lists (uBlock Origin's and the safety lists, matched by "
             u"Brave's adblock-rust).");
    if (enabled && !site.empty()) {
      Text(contents.get(), (*enabled ? u"Blocking is on for "
                                     : u"Blocking is off for ") +
                               site16 + u".");
    }
    auto* buttons = contents->AddChildView(std::make_unique<views::View>());
    buttons->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 8));
    views::View* first = nullptr;
    if (enabled && !site.empty()) {
      first = buttons->AddChildView(std::make_unique<views::MdTextButton>(
          base::BindRepeating(&BlockingBubble::Toggle,
                              weak_factory_.GetWeakPtr()),
          (*enabled ? u"Turn off for " : u"Turn on for ") + site16));
    }
    auto* settings_button = buttons->AddChildView(
        std::make_unique<views::MdTextButton>(
            base::BindRepeating(&BlockingBubble::Settings,
                                weak_factory_.GetWeakPtr()),
            u"Settings..."));
    delegate_->SetContentsView(std::move(contents));
    // Enter or Space switches it at once: the reason it was opened.
    delegate_->SetInitiallyFocusedView(first ? first : settings_button);

    widget_ = views::BubbleDialogDelegate::CreateBubble(
        delegate_.get(),
        base::BindOnce(
            [](BlockingBubble* bubble, views::Widget::ClosedReason) {
              // After the event that closed it: not inside its own event.
              base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
                  FROM_HERE, bubble);
            },
            base::Unretained(this)));
    widget_->Show();
    VLOG(1) << "lrb: blocking bubble: " << blocked << " blocked, "
            << (!enabled ? "unknown" : *enabled ? "on" : "off");
  }

  ~BlockingBubble() {
    VLOG(1) << "lrb: blocking bubble: closed";
    widget_->CloseNow();
    widget_.reset();  // the widget before its delegate
  }

 private:
  static void Text(views::View* parent, const std::u16string& text) {
    auto* label = parent->AddChildView(std::make_unique<views::Label>(
        text, views::style::CONTEXT_DIALOG_BODY_TEXT,
        views::style::STYLE_PRIMARY));
    label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    label->SetMultiLine(true);
  }

  void Toggle() {
    widget_->Close();  // deletes this, soon
    if (toggle_) {
      std::move(toggle_).Run();
    }
  }

  void Settings() {
    widget_->Close();
    if (settings_) {
      std::move(settings_).Run();
    }
  }

  base::OnceClosure toggle_;
  base::OnceClosure settings_;
  std::unique_ptr<views::BubbleDialogDelegate> delegate_;
  std::unique_ptr<views::Widget> widget_;
  base::WeakPtrFactory<BlockingBubble> weak_factory_{this};
};

}  // namespace

void ShowBlockingBubble(views::View* anchor,
                        int blocked,
                        const std::string& site,
                        std::optional<bool> enabled,
                        base::OnceClosure toggle,
                        base::OnceClosure settings) {
  new BlockingBubble(anchor, blocked, site, enabled, std::move(toggle),
                     std::move(settings));
}

}  // namespace lrb
