// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/ui/tab_strip.h"

#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "cc/paint/paint_flags.h"
#include "components/vector_icons/vector_icons.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/models/image_model.h"
#include "ui/color/color_id.h"
#include "ui/color/color_provider.h"
#include "ui/events/event.h"
#include "ui/events/event_constants.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/scoped_canvas.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/background.h"
#include "ui/views/controls/button/button.h"
#include "ui/views/controls/button/image_button.h"
#include "ui/views/controls/button/image_button_factory.h"
#include "ui/views/controls/image_view.h"
#include "ui/views/controls/label.h"
#include "ui/views/layout/flex_layout.h"
#include "ui/views/layout/flex_layout_types.h"
#include "ui/views/style/typography.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/widget/widget.h"

namespace lrb {

namespace {

// The strip is kHeight tall: the tabs stand on its bottom edge with this
// much air above them.
constexpr int kTabHeight = 30;
constexpr int kTabWidth = 220;  // while there is room
constexpr int kCardRadius = 8;  // the shown tab's top corners
constexpr int kAccentHeight = 2;
constexpr int kPillRadius = 6;  // under the pointer, on the other tabs
constexpr int kIconSize = 16;
constexpr int kCloseSize = 14;
// Narrower than this, only the shown tab keeps its close button.
constexpr int kCloseMinTabWidth = 100;

// One tab. A button: a click (or Enter, Space) shows it, a middle-click
// closes it. The close button inside takes its own clicks.
class StripTab : public views::Button {
  METADATA_HEADER(StripTab, views::Button)

 public:
  // `separator`: a hairline at its right edge (between two tabs that are
  // not the shown one).
  StripTab(const TabStrip::Tab& tab,
           bool active,
           bool separator,
           SkColor accent,
           base::RepeatingClosure select,
           base::RepeatingClosure close)
      : views::Button(base::BindRepeating(
            [](const base::RepeatingClosure& select,
               const base::RepeatingClosure& close,
               const ui::Event& event) {
              if (event.IsMouseEvent() &&
                  event.AsMouseEvent()->IsOnlyMiddleMouseButton()) {
                close.Run();
              } else {
                select.Run();
              }
            },
            std::move(select), close)),
        active_(active),
        separator_(separator),
        accent_(accent) {
    SetTriggerableEventFlags(ui::EF_LEFT_MOUSE_BUTTON |
                             ui::EF_MIDDLE_MOUSE_BUTTON);
    // The pointer over the icon, the title or the close button is still
    // over the tab.
    SetNotifyEnterExitOnChild(true);
    SetPreferredSize(gfx::Size(kTabWidth, kTabHeight));
    SetTooltipText(tab.title);
    GetViewAccessibility().SetRole(ax::mojom::Role::kTab);
    GetViewAccessibility().SetName(tab.title);

    SetLayoutManager(std::make_unique<views::FlexLayout>())
        ->SetOrientation(views::LayoutOrientation::kHorizontal)
        .SetCrossAxisAlignment(views::LayoutAlignment::kCenter)
        .SetInteriorMargin(gfx::Insets::TLBR(kAccentHeight, 10, 0, 6));

    // What the tab is painted on, for the title's text rendering.
    const ui::ColorId surface = active ? ui::kColorWindowBackground
                                       : ui::kColorSubtleEmphasisBackground;
    if (!tab.icon.isNull()) {
      auto* icon = AddChildView(std::make_unique<views::ImageView>());
      icon->SetImage(ui::ImageModel::FromImageSkia(tab.icon));
      icon->SetImageSize(gfx::Size(kIconSize, kIconSize));
      icon->SetProperty(views::kMarginsKey, gfx::Insets::TLBR(0, 0, 0, 8));
      icon->SetCanProcessEventsWithinSubtree(false);
    }
    auto* title = AddChildView(std::make_unique<views::Label>(
        tab.title, views::style::CONTEXT_LABEL,
        active ? views::style::STYLE_PRIMARY : views::style::STYLE_SECONDARY));
    title->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    title->SetElideBehavior(gfx::ELIDE_TAIL);
    title->SetBackgroundColor(surface);
    title->SetCanProcessEventsWithinSubtree(false);
    // The title takes what the icon and the close button leave, and is the
    // first to give way in a narrow tab.
    title->SetProperty(
        views::kFlexBehaviorKey,
        views::FlexSpecification(views::LayoutOrientation::kHorizontal,
                                 views::MinimumFlexSizeRule::kScaleToZero,
                                 views::MaximumFlexSizeRule::kUnbounded));
    close_ = AddChildView(views::CreateVectorImageButtonWithNativeTheme(
        std::move(close), vector_icons::kCloseSmallIcon, kCloseSize));
    close_->SetTooltipText(active ? u"Close tab (Ctrl+W)" : u"Close tab");
    close_->SetProperty(views::kMarginsKey, gfx::Insets::TLBR(0, 4, 0, 0));
  }
  StripTab(const StripTab&) = delete;
  StripTab& operator=(const StripTab&) = delete;
  ~StripTab() override = default;

  views::View* close_button() { return close_; }

  void SetAccent(SkColor accent) {
    if (accent_ == accent) {
      return;
    }
    accent_ = accent;
    SchedulePaint();
  }

 protected:
  // views::Button:
  void OnBoundsChanged(const gfx::Rect& previous_bounds) override {
    views::Button::OnBoundsChanged(previous_bounds);
    // A narrow tab that isn't the shown one gives its close button's room
    // to the title (a middle-click still closes it, and so does its button
    // once shown). The tab's own size doesn't depend on this.
    const bool room = active_ || width() >= kCloseMinTabWidth;
    if (close_->GetVisible() != room) {
      close_->SetVisible(room);
    }
  }

  void StateChanged(ButtonState old_state) override {
    views::Button::StateChanged(old_state);
    SchedulePaint();  // the pill under the pointer comes and goes
  }

  void OnPaintBackground(gfx::Canvas* canvas) override {
    const ui::ColorProvider* colors = GetColorProvider();
    if (!colors) {
      return;
    }
    const gfx::Rect bounds = GetLocalBounds();
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setStyle(cc::PaintFlags::kFill_Style);
    if (active_) {
      // A card in the bar's colour standing on the bar: round at the top,
      // square at the foot (a taller rounded rectangle, cut at the tab's
      // bottom edge). Under it, the same shape in the site's colour, of
      // which the card leaves a line along the top.
      gfx::ScopedCanvas scoped(canvas);
      canvas->ClipRect(bounds);
      gfx::Rect card = bounds;
      card.set_height(bounds.height() + kCardRadius);
      if (accent_ != SK_ColorTRANSPARENT) {
        flags.setColor(accent_);
        canvas->DrawRoundRect(card, kCardRadius, flags);
        card.Inset(gfx::Insets::TLBR(kAccentHeight, 0, 0, 0));
      }
      flags.setColor(colors->GetColor(ui::kColorWindowBackground));
      canvas->DrawRoundRect(card, kCardRadius, flags);
      return;
    }
    if (GetState() == STATE_HOVERED || GetState() == STATE_PRESSED) {
      // A tenth of the text's colour: darker on a light strip, lighter on
      // a dark one.
      gfx::Rect pill = bounds;
      pill.Inset(gfx::Insets::TLBR(2, 2, 3, 2));
      flags.setColor(SkColorSetA(
          colors->GetColor(ui::kColorPrimaryForeground), 0x1A));
      canvas->DrawRoundRect(pill, kPillRadius, flags);
    } else if (separator_) {
      canvas->FillRect(
          gfx::Rect(bounds.right() - 1, 8, 1, bounds.height() - 16),
          colors->GetColor(ui::kColorSeparator));
    }
  }

 private:
  const bool active_;
  const bool separator_;
  SkColor accent_;
  raw_ptr<views::View> close_ = nullptr;
};

BEGIN_METADATA(StripTab)
END_METADATA

// `view`'s centre in its window, as the tests' pointer clicks take it.
gfx::Point CenterInWindow(views::View* view) {
  gfx::Point center = view->GetLocalBounds().CenterPoint();
  views::View::ConvertPointToTarget(view, view->GetWidget()->GetRootView(),
                                    &center);
  return center;
}

}  // namespace

TabStrip::TabStrip(base::RepeatingCallback<void(size_t)> select,
                   base::RepeatingCallback<void(size_t)> close,
                   base::RepeatingClosure new_tab)
    : select_(std::move(select)),
      close_(std::move(close)),
      new_tab_(std::move(new_tab)) {
  SetBackground(
      views::CreateSolidBackground(ui::kColorSubtleEmphasisBackground));
  // The tabs stand on the strip's bottom edge, which is the bar's top.
  SetLayoutManager(std::make_unique<views::FlexLayout>())
      ->SetOrientation(views::LayoutOrientation::kHorizontal)
      .SetCrossAxisAlignment(views::LayoutAlignment::kEnd)
      .SetInteriorMargin(gfx::Insets::TLBR(kHeight - kTabHeight, 6, 0, 6));
  GetViewAccessibility().SetRole(ax::mojom::Role::kTabList);
  GetViewAccessibility().SetName(u"Tabs");
}

TabStrip::~TabStrip() = default;

void TabStrip::SetTabs(std::vector<Tab> tabs, size_t active) {
  active_tab_ = nullptr;  // before its view goes
  RemoveAllChildViews();
  tab_count_ = tabs.size();
  for (size_t i = 0; i < tabs.size(); ++i) {
    const bool is_active = i == active;
    // A hairline between two neighbours, neither of them the card.
    const bool separator =
        !is_active && i + 1 != active && i + 1 < tabs.size();
    auto* tab = AddChildView(std::make_unique<StripTab>(
        tabs[i], is_active, separator, accent_,
        base::BindRepeating(select_, i), base::BindRepeating(close_, i)));
    // 220 px each while they fit, narrower together when they don't.
    tab->SetProperty(
        views::kFlexBehaviorKey,
        views::FlexSpecification(views::LayoutOrientation::kHorizontal,
                                 views::MinimumFlexSizeRule::kScaleToMinimum,
                                 views::MaximumFlexSizeRule::kPreferred));
    if (is_active) {
      active_tab_ = tab;
    }
  }
  auto* add = AddChildView(views::CreateVectorImageButtonWithNativeTheme(
      new_tab_, vector_icons::kAddOldIcon, kIconSize));
  add->SetTooltipText(u"New tab (Ctrl+T)");
  add->SetProperty(views::kMarginsKey, gfx::Insets::TLBR(0, 6, 0, 0));
  // In the middle of the tabs' height, not on the bottom edge with them.
  add->SetProperty(views::kCrossAxisAlignmentKey,
                   views::LayoutAlignment::kCenter);
  InvalidateLayout();

  if (VLOG_IS_ON(1)) {
    // Once laid out.
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&TabStrip::LogPositions, weak_factory_.GetWeakPtr()),
        base::Milliseconds(500));
  }
}

void TabStrip::SetAccentColor(SkColor color) {
  accent_ = color;
  if (active_tab_) {
    static_cast<StripTab*>(active_tab_.get())->SetAccent(color);
  }
}

void TabStrip::LogPositions() {
  if (!GetWidget() || !GetVisible()) {
    return;
  }
  size_t index = 0;
  for (views::View* child : children()) {
    const gfx::Point center = CenterInWindow(child);
    if (index < tab_count_) {
      const gfx::Point close =
          CenterInWindow(static_cast<StripTab*>(child)->close_button());
      VLOG(1) << "lrb: tab " << index << " at " << center.x() << ","
              << center.y() << ", close at " << close.x() << "," << close.y()
              << (child == active_tab_ ? ", shown" : "");
    } else {
      VLOG(1) << "lrb: new tab button at " << center.x() << "," << center.y();
    }
    ++index;
  }
}

BEGIN_METADATA(TabStrip)
END_METADATA

}  // namespace lrb
