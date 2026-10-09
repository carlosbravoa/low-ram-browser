// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/ui/suggestions_popup.h"

#include <algorithm>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "components/vector_icons/vector_icons.h"
#include "lrb/coordinator/bookmarks.h"
#include "ui/base/models/image_model.h"
#include "ui/color/color_id.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/controls/button/label_button.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/view.h"
#include "ui/views/widget/widget.h"

namespace lrb {

namespace {

constexpr int kRowHeight = 30;
constexpr int kMinWidth = 360;

// "Title  address" for a bookmark, the address for a site.
std::u16string RowText(const Suggestion& suggestion) {
  std::string address(
      coordinator::DisplayAddress(suggestion.url.spec()));
  if (address.ends_with('/') &&
      address.find('/') == address.size() - 1) {
    address.pop_back();  // "example.org/" reads as "example.org"
  }
  if (suggestion.title.empty()) {
    return base::UTF8ToUTF16(address);
  }
  return suggestion.title + u"  —  " + base::UTF8ToUTF16(address);
}

}  // namespace

SuggestionsPopup::SuggestionsPopup(
    views::View* anchor,
    base::RepeatingCallback<void(GURL)> open)
    : anchor_(anchor), open_(std::move(open)) {}

SuggestionsPopup::~SuggestionsPopup() = default;

void SuggestionsPopup::Show(std::vector<Suggestion> suggestions) {
  suggestions_ = std::move(suggestions);
  selected_.reset();
  if (suggestions_.empty() || !anchor_->GetWidget()) {
    Hide();
    return;
  }
  Build();
}

void SuggestionsPopup::Hide() {
  widget_.reset();
  suggestions_.clear();
  selected_.reset();
}

const Suggestion* SuggestionsPopup::Move(int delta) {
  if (!widget_ || suggestions_.empty()) {
    return nullptr;
  }
  const int count = static_cast<int>(suggestions_.size());
  // From nothing selected, Down goes to the first and Up to the last.
  int next = selected_ ? static_cast<int>(*selected_) + delta
                       : (delta > 0 ? 0 : count - 1);
  if (next < 0 || next >= count) {
    selected_.reset();  // back to what was typed
  } else {
    selected_ = static_cast<size_t>(next);
  }
  Build();
  return selected_ ? &suggestions_[*selected_] : nullptr;
}

void SuggestionsPopup::Build() {
  const gfx::Rect anchor = anchor_->GetBoundsInScreen();
  const gfx::Rect bounds(
      anchor.x(), anchor.bottom() + 2, std::max(anchor.width(), kMinWidth),
      kRowHeight * static_cast<int>(suggestions_.size()) + 2);
  if (!widget_) {
    widget_ = std::make_unique<views::Widget>();
    views::Widget::InitParams params(
        views::Widget::InitParams::CLIENT_OWNS_WIDGET,
        views::Widget::InitParams::TYPE_POPUP);
    params.parent = anchor_->GetWidget()->GetNativeView();
    params.activatable = views::Widget::InitParams::Activatable::kNo;
    params.bounds = bounds;
    params.name = "SuggestionsPopup";
    widget_->Init(std::move(params));
  } else {
    widget_->SetBounds(bounds);
  }
  auto contents = std::make_unique<views::View>();
  contents->SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kVertical));
  contents->SetBackground(
      views::CreateSolidBackground(ui::kColorWindowBackground));
  contents->SetBorder(views::CreateSolidBorder(1, ui::kColorSeparator));
  for (size_t i = 0; i < suggestions_.size(); ++i) {
    const Suggestion& suggestion = suggestions_[i];
    auto* row = contents->AddChildView(std::make_unique<views::LabelButton>(
        base::BindRepeating(open_, suggestion.url), RowText(suggestion)));
    row->SetImageModel(
        views::Button::STATE_NORMAL,
        ui::ImageModel::FromVectorIcon(suggestion.bookmark
                                           ? vector_icons::kStarFilledIcon
                                           : vector_icons::kGlobeIcon,
                                       ui::kColorIcon, 14));
    row->SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(0, 10)));
    row->SetPreferredSize(gfx::Size(bounds.width() - 2, kRowHeight));
    row->SetElideBehavior(gfx::ELIDE_TAIL);
    row->SetFocusBehavior(views::View::FocusBehavior::NEVER);
    if (selected_ == i) {
      row->SetBackground(
          views::CreateSolidBackground(ui::kColorSubtleEmphasisBackground));
    }
  }
  widget_->SetContentsView(std::move(contents));
  widget_->ShowInactive();
}

}  // namespace lrb
