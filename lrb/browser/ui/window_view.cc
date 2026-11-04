// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/ui/window_view.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/hash/hash.h"
#include "base/logging.h"
#include "base/process/kill.h"
#include "base/process/launch.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "components/vector_icons/vector_icons.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browsing_data_filter_builder.h"
#include "content/public/browser/browsing_data_remover.h"
#include "content/public/browser/host_zoom_map.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/stop_find_action.h"
#include "lrb/browser/shell.h"
#include "lrb/browser/blocked_counter.h"
#include "lrb/browser/bookmarks.h"
#include "lrb/browser/dark_pages.h"
#include "lrb/browser/download_manager_delegate.h"
#include "lrb/browser/history.h"
#include "lrb/browser/lrb_content_browser_client.h"
#include "lrb/browser/permission_manager.h"
#include "lrb/browser/permissions/site_permissions.h"
#include "lrb/browser/print.h"
#include "lrb/browser/site.h"
#include "lrb/browser/ui/about_dialog.h"
#include "lrb/browser/ui/blocking_bubble.h"
#include "lrb/browser/ui/lrb_platform_delegate.h"
#include "lrb/browser/ui/settings_dialog.h"
#include "lrb/browser/ui/snap_connections_dialog.h"
#include "lrb/browser/ui/suggestions_popup.h"
#include "lrb/browser/zoom_levels.h"
#include "lrb/common/content_blocker.h"
#include "lrb/common/lrb_switches.h"
#include "lrb/common/mode.h"
#include "lrb/common/settings.h"
#include "lrb/common/version.h"
#include "lrb/coordinator/bookmarks.h"
#include "third_party/blink/public/common/page/page_zoom.h"
#include "third_party/blink/public/mojom/frame/find_in_page.mojom.h"
#include "ui/aura/window.h"
#include "ui/aura/window_observer.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/mojom/menu_source_type.mojom.h"
#include "ui/color/color_id.h"
#include "ui/events/event.h"
#include "ui/events/event_handler.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/color_utils.h"
#include "ui/gfx/range/range.h"
#include "ui/gfx/vector_icon_types.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/controls/button/image_button.h"
#include "ui/views/controls/button/image_button_factory.h"
#include "ui/views/controls/button/label_button.h"
#include "ui/views/controls/button/md_text_button.h"
#include "ui/views/controls/image_view.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/menu/menu_runner.h"
#include "ui/views/controls/separator.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/controls/webview/webview.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/layout/flex_layout.h"
#include "ui/views/layout/flex_layout_types.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/widget/widget.h"
#include "url/origin.h"

namespace lrb {

namespace {

// Three vertical dots: the menu. Components' icons have none.
constexpr gfx::PathElement kMenuPath[] = {
    gfx::CANVAS_DIMENSIONS, 24,
    gfx::CIRCLE, 12, 5, 2, gfx::NEW_PATH,
    gfx::CIRCLE, 12, 12, 2, gfx::NEW_PATH,
    gfx::CIRCLE, 12, 19, 2};
constexpr gfx::VectorIconRep kMenuReps[] = {{kMenuPath}};
constexpr gfx::VectorIcon kMenuIcon(kMenuReps, 1, "lrb_menu");

// A star's outline: not bookmarked (components' stars are filled).
constexpr gfx::PathElement kStarOutlinePath[] = {
    gfx::CANVAS_DIMENSIONS, 24,
    gfx::STROKE, 1.8f,
    gfx::MOVE_TO, 12.0f, 3.3f,
    gfx::LINE_TO, 14.4f, 9.6f,
    gfx::LINE_TO, 21.0f, 9.9f,
    gfx::LINE_TO, 15.8f, 14.0f,
    gfx::LINE_TO, 17.6f, 20.5f,
    gfx::LINE_TO, 12.0f, 16.8f,
    gfx::LINE_TO, 6.4f, 20.5f,
    gfx::LINE_TO, 8.2f, 14.0f,
    gfx::LINE_TO, 3.0f, 9.9f,
    gfx::LINE_TO, 9.6f, 9.6f,
    gfx::CLOSE};
constexpr gfx::VectorIconRep kStarOutlineReps[] = {{kStarOutlinePath}};
constexpr gfx::VectorIcon kStarOutlineIcon(kStarOutlineReps,
                                           1,
                                           "lrb_star_outline");

constexpr int kBarHeight = 36;
constexpr int kIconSize = 18;

// The site's colour: a hue from the site name, so each site's windows are
// recognisable at a glance and a page passing through another site in a
// flow (a login, a payment) visibly changes colour.
// Twelve hues 30 degrees apart: any hue would put many sites too close to
// tell apart.
SkColor SiteColor(const std::string& site) {
  const double hue = (base::PersistentHash(site) % 12) / 12.0;
  return color_utils::HSLToSkColor({hue, 0.55, 0.38}, SK_AlphaOPAQUE);
}

// What the address field shows when not editing, next to the site chip:
// the rest of the address. The whole URL where there is no site.
std::u16string AddressRest(const GURL& url, const std::string& site) {
  if (site.empty()) {
    return url.is_empty() ? std::u16string() : base::UTF8ToUTF16(url.spec());
  }
  std::string rest(url.host());
  // The host's part beyond the site ("www.", "en.") is noise; other
  // subdomains (mail.google.com) say where you are.
  if (rest == site || rest == "www." + site) {
    rest.clear();
  }
  if (url.path() != "/" || url.has_query()) {
    rest += url.path();
  }
  if (url.has_query()) {
    rest += "?";
    rest += url.query();
  }
  return base::UTF8ToUTF16(rest);
}

constexpr char kReleases[] =
    "https://github.com/carlosbravoa/low-ram-browser/releases";

// Compares dotted parts ("0.1.0", "alpha.7"): numbers as numbers
// ("alpha.10" after "alpha.9"), a number before a word (as semver), words
// alphabetically, a prefix first. -1, 0 or 1.
int CompareDotted(std::string_view a, std::string_view b) {
  const std::vector<std::string_view> as = base::SplitStringPiece(
      a, ".", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
  const std::vector<std::string_view> bs = base::SplitStringPiece(
      b, ".", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
  for (size_t i = 0; i < std::max(as.size(), bs.size()); ++i) {
    if (i >= as.size()) {
      return -1;
    }
    if (i >= bs.size()) {
      return 1;
    }
    unsigned na = 0;
    unsigned nb = 0;
    const bool a_number = base::StringToUint(as[i], &na);
    const bool b_number = base::StringToUint(bs[i], &nb);
    if (a_number && b_number) {
      if (na != nb) {
        return na < nb ? -1 : 1;
      }
    } else if (a_number != b_number) {
      return a_number ? -1 : 1;
    } else if (as[i] != bs[i]) {
      return as[i] < bs[i] ? -1 : 1;
    }
  }
  return 0;
}

// Whether release `candidate` ("0.1.0-alpha.7") is newer than `current`:
// the dotted numbers first, then a release beats its pre-releases
// ("0.1.0" after "0.1.0-alpha.7"), then the pre-releases' parts. False for
// anything that doesn't read as a version (the check's file is the
// network's word).
bool IsNewerRelease(std::string_view candidate, std::string_view current) {
  auto split = [](std::string_view version) {
    const size_t dash = version.find('-');
    return std::make_pair(version.substr(0, dash),
                          dash == std::string_view::npos
                              ? std::string_view()
                              : version.substr(dash + 1));
  };
  const auto [new_core, new_pre] = split(candidate);
  const auto [core, pre] = split(current);
  for (std::string_view part : base::SplitStringPiece(
           new_core, ".", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL)) {
    unsigned number = 0;
    if (!base::StringToUint(part, &number)) {
      return false;
    }
  }
  if (core.empty()) {
    return false;  // a development build: no release to compare
  }
  if (const int by_core = CompareDotted(new_core, core); by_core != 0) {
    return by_core > 0;
  }
  if (new_pre.empty() != pre.empty()) {
    return new_pre.empty();
  }
  return CompareDotted(new_pre, pre) > 0;
}

// Runs `done` once a removal of browsing data is complete; owns itself.
class RemovalObserver : public content::BrowsingDataRemover::Observer {
 public:
  RemovalObserver(content::BrowsingDataRemover* remover, base::OnceClosure done)
      : remover_(remover), done_(std::move(done)) {
    remover_->AddObserver(this);
  }

  // content::BrowsingDataRemover::Observer:
  void OnBrowsingDataRemoverDone(uint64_t failed_data_types) override {
    remover_->RemoveObserver(this);
    base::OnceClosure done = std::move(done_);
    delete this;
    std::move(done).Run();
  }

 private:
  ~RemovalObserver() override = default;

  raw_ptr<content::BrowsingDataRemover> remover_;
  base::OnceClosure done_;
};

}  // namespace

// The address: the rest of the URL beside the chip while reading, the whole
// URL, selected, while editing.
class AddressField : public views::Textfield {
  METADATA_HEADER(AddressField, views::Textfield)

 public:
  AddressField() {
    SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(0, 6)));
    SetBackgroundEnabled(false);
    SetTextInputType(ui::TEXT_INPUT_TYPE_URL);
    SetAccessibleName(u"Address");
  }

  void Show(const GURL& url, const std::u16string& rest) {
    url_ = url;
    rest_ = rest;
    // Only for an empty window: beside a site chip it would read as part
    // of the address.
    SetPlaceholderText(url.is_empty() || url.IsAboutBlank()
                           ? u"Search or type an address"
                           : u"");
    if (!HasFocus()) {
      SetText(rest_);
    }
  }

  void Revert() {
    SetText(base::UTF8ToUTF16(url_.spec()));
    SelectAll(false);
  }

  void set_on_blur(base::RepeatingClosure on_blur) {
    on_blur_ = std::move(on_blur);
  }

  // views::Textfield:
  void OnFocus() override {
    views::Textfield::OnFocus();
    SetText(url_.is_empty() || url_.IsAboutBlank()
                ? std::u16string()
                : base::UTF8ToUTF16(url_.spec()));
    SelectAll(false);
  }
  void OnBlur() override {
    views::Textfield::OnBlur();
    SetText(rest_);
    if (on_blur_) {
      on_blur_.Run();
    }
  }

 private:
  GURL url_;
  std::u16string rest_;
  base::RepeatingClosure on_blur_;
};

BEGIN_METADATA(AddressField)
END_METADATA

// Window shortcuts must win over the page (Ctrl+L, Ctrl+W, ...), and
// content_shell's Shell doesn't hand unhandled keys back on Linux, so keys
// are looked at on their way to the page: a pre-target handler on the
// window sees every key event of its descendants first.
class ShortcutHandler : public ui::EventHandler, public aura::WindowObserver {
 public:
  ShortcutHandler(aura::Window* window, WindowView* view)
      : window_(window), view_(view) {
    window_->AddPreTargetHandler(this);
    window_->AddObserver(this);
  }
  ~ShortcutHandler() override { Stop(); }

  // ui::EventHandler:
  void OnKeyEvent(ui::KeyEvent* event) override {
    if (event->type() == ui::EventType::kKeyPressed &&
        view_->HandleShortcut(*event)) {
      event->StopPropagation();
    }
  }

  // aura::WindowObserver:
  void OnWindowDestroying(aura::Window* window) override { Stop(); }

 private:
  void Stop() {
    if (window_) {
      window_->RemovePreTargetHandler(this);
      window_->RemoveObserver(this);
      window_ = nullptr;
    }
  }

  raw_ptr<aura::Window> window_;
  raw_ptr<WindowView> view_;
};

WindowView::TabActions::TabActions() = default;
WindowView::TabActions::TabActions(TabActions&&) = default;
WindowView::TabActions& WindowView::TabActions::operator=(TabActions&&) =
    default;
WindowView::TabActions::~TabActions() = default;

WindowView::WindowView(TabActions actions) : actions_(std::move(actions)) {
  auto* layout = SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kVertical));
  SetBackground(views::CreateSolidBackground(ui::kColorWindowBackground));

  auto* bar = AddChildView(std::make_unique<views::View>());
  bar_ = bar;
  // --content-shell-hide-toolbar: no bar (kiosks, and measuring the bar).
  const bool hidden = Shell::ShouldHideToolbar();
  bar->SetVisible(!hidden);
  bar->SetPreferredSize(gfx::Size(0, kBarHeight));
  bar->SetLayoutManager(std::make_unique<views::FlexLayout>())
      ->SetOrientation(views::LayoutOrientation::kHorizontal)
      .SetCrossAxisAlignment(views::LayoutAlignment::kCenter)
      .SetInteriorMargin(gfx::Insets::VH(0, 4))
      .SetDefault(views::kMarginsKey, gfx::Insets::VH(0, 1));

  back_ = bar->AddChildView(views::CreateVectorImageButtonWithNativeTheme(
      base::BindRepeating(&WindowView::GoBack, base::Unretained(this)),
      vector_icons::kArrowBackIcon, kIconSize));
  back_->SetTooltipText(u"Back (Alt+Left)");
  back_->SetEnabled(false);
  forward_ = bar->AddChildView(views::CreateVectorImageButtonWithNativeTheme(
      base::BindRepeating(
          [](WindowView* view) {
            if (view->shell_) {
              view->shell_->GoBackOrForward(1);
            }
          },
          base::Unretained(this)),
      vector_icons::kArrowForwardIcon, kIconSize));
  forward_->SetTooltipText(u"Forward (Alt+Right)");
  forward_->SetEnabled(false);
  reload_ = bar->AddChildView(views::CreateVectorImageButtonWithNativeTheme(
      base::BindRepeating(
          [](WindowView* view) {
            if (view->shell_) {
              view->loading_ ? view->shell_->Stop() : view->shell_->Reload();
            }
          },
          base::Unretained(this)),
      vector_icons::kReloadChromeRefreshOldIcon, kIconSize));
  reload_->SetTooltipText(u"Reload (F5)");

  chip_ = bar->AddChildView(std::make_unique<views::LabelButton>(
      base::BindRepeating(&WindowView::FocusAddress, base::Unretained(this))));
  chip_->SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(3, 10)));
  chip_->SetEnabledTextColors(SK_ColorWHITE);
  chip_->SetProperty(views::kMarginsKey, gfx::Insets::TLBR(0, 6, 0, 0));
  chip_->SetVisible(false);

  address_ = bar->AddChildView(std::make_unique<AddressField>());
  address_->set_controller(this);
  address_->set_on_blur(base::BindRepeating(&WindowView::HideSuggestions,
                                            base::Unretained(this)));
  address_->SetProperty(
      views::kFlexBehaviorKey,
      views::FlexSpecification(views::LayoutOrientation::kHorizontal,
                               views::MinimumFlexSizeRule::kScaleToMinimum,
                               views::MaximumFlexSizeRule::kUnbounded));
  suggestions_ = std::make_unique<SuggestionsPopup>(
      address_, base::BindRepeating(&WindowView::OpenSuggestion,
                                    base::Unretained(this)));

  // Bookmarks are kept by the coordinator: shown with one (SetUrl).
  star_ = bar->AddChildView(views::CreateVectorImageButtonWithNativeTheme(
      base::BindRepeating(&WindowView::ToggleBookmark, base::Unretained(this)),
      kStarOutlineIcon, kIconSize));
  star_->SetVisible(false);
  SetBookmarked(false);

  // Camera/microphone in use: red, like a recording light.
  constexpr SkColor kInUse = SkColorSetRGB(0xd9, 0x30, 0x25);
  camera_in_use_ = bar->AddChildView(std::make_unique<views::ImageView>(
      ui::ImageModel::FromVectorIcon(vector_icons::kVideocamIcon, kInUse,
                                     kIconSize)));
  camera_in_use_->SetTooltipText(u"This page is using your camera");
  camera_in_use_->SetVisible(false);
  microphone_in_use_ = bar->AddChildView(std::make_unique<views::ImageView>(
      ui::ImageModel::FromVectorIcon(vector_icons::kMicIcon, kInUse,
                                     kIconSize)));
  microphone_in_use_->SetTooltipText(u"This page is using your microphone");
  microphone_in_use_->SetVisible(false);

  download_ = bar->AddChildView(std::make_unique<views::LabelButton>(
      base::BindRepeating(&WindowView::ShowDownloadMenu,
                          base::Unretained(this))));
  download_->SetImageModel(
      views::Button::STATE_NORMAL,
      ui::ImageModel::FromVectorIcon(vector_icons::kDownloadIcon,
                                     ui::kColorIcon, 16));
  download_->SetMaxSize(gfx::Size(220, 0));
  download_->SetElideBehavior(gfx::ELIDE_MIDDLE);
  download_->SetVisible(false);

  zoom_ = bar->AddChildView(std::make_unique<views::LabelButton>(
      base::BindRepeating(&WindowView::Zoom, base::Unretained(this), 0)));
  zoom_->SetTooltipText(u"Zoom: click to reset (Ctrl+0)");
  zoom_->SetVisible(false);

  // The blocked count; a click explains it and offers turning blocking off
  // for the site (ShowBlockingBubble).
  blocked_ = bar->AddChildView(std::make_unique<views::LabelButton>(
      base::BindRepeating(&WindowView::ShowBlockingInfo,
                          base::Unretained(this))));
  blocked_->SetImageModel(
      views::Button::STATE_NORMAL,
      ui::ImageModel::FromVectorIcon(vector_icons::kShieldIcon,
                                     ui::kColorIcon, 16));
  blocked_->SetEnabledTextColors(ui::kColorLabelForegroundSecondary);
  blocked_->SetVisible(false);

  // A newer release (the coordinator's daily check, --lrb-latest-release).
  const std::string latest =
      base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
          switches::kLatestRelease);
  if (IsNewerRelease(latest, Version())) {
    update_ = bar->AddChildView(std::make_unique<views::LabelButton>(
        base::BindRepeating(&WindowView::OpenReleases, base::Unretained(this)),
        u"Update available"));
    update_->SetImageModel(
        views::Button::STATE_NORMAL,
        ui::ImageModel::FromVectorIcon(vector_icons::kDownloadIcon,
                                       ui::kColorIcon, 16));
    update_->SetTooltipText(base::UTF8ToUTF16(
        "lrb " + latest + " is out (this is " + std::string(Version()) +
        "): open the releases page"));
  }
  // A snap connection that matters is missing (the coordinator says so
  // once, on the start after it noticed).
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kSnapNotice)) {
    snap_notice_ = bar->AddChildView(std::make_unique<views::LabelButton>(
        base::BindRepeating(&WindowView::OpenSnapConnections,
                            base::Unretained(this)),
        u"Snap connections to make"));
    snap_notice_->SetImageModel(
        views::Button::STATE_NORMAL,
        ui::ImageModel::FromVectorIcon(vector_icons::kSettingsIcon,
                                       ui::kColorIcon, 16));
    snap_notice_->SetTooltipText(
        u"Features this snap can't use until connected: open Snap "
        u"connections");
    VLOG(1) << "lrb: snap notice: Snap connections to make";
  }

  bar->AddChildView(views::CreateVectorImageButtonWithNativeTheme(
                        base::BindRepeating(&WindowView::ShowMenu,
                                            base::Unretained(this)),
                        kMenuIcon, kIconSize))
      ->SetTooltipText(u"Menu");

  // Tabs: a thin row, only with 2+ (SetTabs).
  tab_row_ = AddChildView(std::make_unique<views::View>());
  tab_row_->SetLayoutManager(std::make_unique<views::FlexLayout>())
      ->SetOrientation(views::LayoutOrientation::kHorizontal)
      .SetCrossAxisAlignment(views::LayoutAlignment::kCenter)
      .SetInteriorMargin(gfx::Insets::TLBR(0, 6, 2, 6));
  tab_row_->SetVisible(false);

  AddChildView(std::make_unique<views::Separator>())->SetVisible(!hidden);

  // A permission question: a row under the bar, inside the window (no
  // separate popup window to cost memory or get lost behind others).
  question_row_ = AddChildView(std::make_unique<views::View>());
  question_row_->SetBackground(
      views::CreateSolidBackground(ui::kColorSubtleEmphasisBackground));
  question_row_->SetLayoutManager(std::make_unique<views::FlexLayout>())
      ->SetOrientation(views::LayoutOrientation::kHorizontal)
      .SetCrossAxisAlignment(views::LayoutAlignment::kCenter)
      .SetInteriorMargin(gfx::Insets::VH(6, 12))
      .SetDefault(views::kMarginsKey, gfx::Insets::VH(0, 4));
  question_label_ =
      question_row_->AddChildView(std::make_unique<views::Label>());
  question_label_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  // A page's message can be long: wrap, up to a few lines.
  question_label_->SetMultiLine(true);
  question_label_->SetMaxLines(8);
  question_label_->SetProperty(
      views::kFlexBehaviorKey,
      views::FlexSpecification(views::LayoutOrientation::kHorizontal,
                               views::MinimumFlexSizeRule::kScaleToMinimum,
                               views::MaximumFlexSizeRule::kUnbounded));
  question_input_ =
      question_row_->AddChildView(std::make_unique<views::Textfield>());
  question_input_->set_controller(this);
  question_input_->SetAccessibleName(u"Answer");
  question_input_->SetDefaultWidthInChars(24);
  question_password_ =
      question_row_->AddChildView(std::make_unique<views::Textfield>());
  question_password_->set_controller(this);
  question_password_->SetTextInputType(ui::TEXT_INPUT_TYPE_PASSWORD);
  question_password_->SetAccessibleName(u"Password");
  question_password_->SetPlaceholderText(u"Password");
  question_password_->SetDefaultWidthInChars(16);
  cancel_ = question_row_->AddChildView(std::make_unique<views::MdTextButton>(
      base::BindRepeating(&WindowView::Answer, base::Unretained(this), false)));
  accept_ = question_row_->AddChildView(std::make_unique<views::MdTextButton>(
      base::BindRepeating(&WindowView::Answer, base::Unretained(this), true)));
  accept_->SetStyle(ui::ButtonStyle::kProminent);
  question_row_->SetVisible(false);

  // Find in page (Ctrl+F).
  find_row_ = AddChildView(std::make_unique<views::View>());
  find_row_->SetBackground(
      views::CreateSolidBackground(ui::kColorSubtleEmphasisBackground));
  find_row_->SetLayoutManager(std::make_unique<views::FlexLayout>())
      ->SetOrientation(views::LayoutOrientation::kHorizontal)
      .SetCrossAxisAlignment(views::LayoutAlignment::kCenter)
      .SetInteriorMargin(gfx::Insets::VH(4, 12))
      .SetDefault(views::kMarginsKey, gfx::Insets::VH(0, 2));
  find_input_ = find_row_->AddChildView(std::make_unique<views::Textfield>());
  find_input_->set_controller(this);
  find_input_->SetAccessibleName(u"Find in page");
  find_input_->SetPlaceholderText(u"Find in page");
  find_input_->SetDefaultWidthInChars(24);
  find_count_ = find_row_->AddChildView(std::make_unique<views::Label>());
  find_count_->SetEnabledColor(ui::kColorLabelForegroundSecondary);
  find_count_->SetProperty(
      views::kFlexBehaviorKey,
      views::FlexSpecification(views::LayoutOrientation::kHorizontal,
                               views::MinimumFlexSizeRule::kScaleToMinimum,
                               views::MaximumFlexSizeRule::kUnbounded));
  find_count_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  find_row_
      ->AddChildView(views::CreateVectorImageButtonWithNativeTheme(
          base::BindRepeating(&WindowView::Find, base::Unretained(this),
                              false, false),
          vector_icons::kKeyboardArrowUpIcon, kIconSize))
      ->SetTooltipText(u"Previous (Shift+Enter)");
  find_row_
      ->AddChildView(views::CreateVectorImageButtonWithNativeTheme(
          base::BindRepeating(&WindowView::Find, base::Unretained(this), true,
                              false),
          vector_icons::kKeyboardArrowDownIcon, kIconSize))
      ->SetTooltipText(u"Next (Enter)");
  find_row_
      ->AddChildView(views::CreateVectorImageButtonWithNativeTheme(
          base::BindRepeating(&WindowView::CloseFind, base::Unretained(this)),
          vector_icons::kCloseIcon, kIconSize))
      ->SetTooltipText(u"Close (Escape)");
  find_row_->SetVisible(false);

  contents_ = AddChildView(std::make_unique<views::View>());
  contents_->SetUseDefaultFillLayout(true);
  layout->SetFlexForView(contents_, 1);

}

WindowView::Question::Question() = default;
WindowView::Question::Question(Question&&) = default;
WindowView::Question& WindowView::Question::operator=(Question&&) = default;
WindowView::Question::~Question() = default;

WindowView::~WindowView() {
  suggestions_.reset();
  DropQuestions();
  ClearPage();
}

void WindowView::SetWebContents(content::WebContents* web_contents,
                                std::optional<gfx::Size> size) {
  ClearPage();
  auto web_view =
      std::make_unique<views::WebView>(web_contents->GetBrowserContext());
  web_view->SetWebContents(web_contents);
  if (size) {
    web_view->SetPreferredSize(*size);
  }
  web_view_ = contents_->AddChildView(std::move(web_view));
  web_contents->Focus();
  if (size) {
    web_view_->SizeToPreferredSize();
  }

  zoom_subscription_ =
      content::HostZoomMap::GetForWebContents(web_contents)
          ->AddZoomLevelChangedCallback(base::BindRepeating(
              [](base::WeakPtr<WindowView> view,
                 const content::HostZoomMap::ZoomLevelChange&) {
                if (view) {
                  view->UpdateZoom();
                }
              },
              weak_factory_.GetWeakPtr()));
  UpdateZoom();

  BlockedCounter::CreateForWebContents(web_contents);
  BlockedCounter* counter = BlockedCounter::FromWebContents(web_contents);
  counter->SetOnChanged(base::BindRepeating(&WindowView::SetBlocked,
                                            weak_factory_.GetWeakPtr()));
  SetBlocked(counter->count());

  if (size) {
    // The window's first page: size the window around it, keeping its
    // origin.
    gfx::Rect bounds = GetWidget()->GetWindowBoundsInScreen();
    bounds.set_size(GetWidget()->GetRootView()->GetPreferredSize({}));
    GetWidget()->SetBounds(bounds);
  } else {
    contents_->InvalidateLayout();
  }
}

void WindowView::ShowTab(Shell* shell,
                         const GURL& back_url,
                         std::optional<gfx::Size> first_size) {
  // What belonged to the page shown until now goes with it.
  DropQuestions();
  CloseFind();
  shell_ = shell;
  back_url_ = back_url;
  content::WebContents* contents = shell->web_contents();
  SetWebContents(contents, first_size);
  // The bar follows this tab's page.
  SetUrl(contents->GetVisibleURL());
  SetLoading(contents->IsLoading());
  EnableBack(contents->GetController().CanGoBack());
  EnableForward(contents->GetController().CanGoForward());
}

void WindowView::ClearPage() {
  if (!web_view_) {
    return;
  }
  if (web_view_->web_contents()) {
    if (auto* counter =
            BlockedCounter::FromWebContents(web_view_->web_contents())) {
      counter->SetOnChanged({});
    }
  }
  zoom_subscription_ = {};
  contents_->RemoveChildViewT(web_view_.ExtractAsDangling().get());
}

void WindowView::SetTabs(std::vector<TabLabel> tabs, size_t active) {
  tab_row_->RemoveAllChildViews();
  // Hidden in fullscreen (F11) too.
  tab_row_->SetVisible(tabs.size() > 1 &&
                       !(GetWidget() && GetWidget()->IsFullscreen()));
  if (tabs.size() <= 1) {
    InvalidateLayout();
    return;
  }
  for (size_t i = 0; i < tabs.size(); ++i) {
    auto* tab = tab_row_->AddChildView(std::make_unique<views::View>());
    tab->SetLayoutManager(std::make_unique<views::FlexLayout>())
        ->SetOrientation(views::LayoutOrientation::kHorizontal)
        .SetCrossAxisAlignment(views::LayoutAlignment::kCenter);
    if (i == active) {
      tab->SetBackground(views::CreateRoundedRectBackground(
          ui::kColorSubtleEmphasisBackground, 8));
    }
    tab->SetProperty(views::kMarginsKey, gfx::Insets::VH(2, 2));
    tab->SetProperty(
        views::kFlexBehaviorKey,
        views::FlexSpecification(views::LayoutOrientation::kHorizontal,
                                 views::MinimumFlexSizeRule::kScaleToMinimum,
                                 views::MaximumFlexSizeRule::kPreferred));
    // A click shows the tab; a middle-click closes it.
    auto* title = tab->AddChildView(std::make_unique<views::LabelButton>(
        base::BindRepeating(
            [](const TabActions* actions, size_t index,
               const ui::Event& event) {
              if (event.IsMouseEvent() &&
                  event.AsMouseEvent()->IsOnlyMiddleMouseButton()) {
                actions->close.Run(index);
              } else {
                actions->select.Run(index);
              }
            },
            base::Unretained(&actions_), i),
        tabs[i].title));
    if (!tabs[i].icon.isNull()) {
      title->SetImageModel(views::Button::STATE_NORMAL,
                           ui::ImageModel::FromImageSkia(tabs[i].icon));
    }
    title->SetTriggerableEventFlags(ui::EF_LEFT_MOUSE_BUTTON |
                                    ui::EF_MIDDLE_MOUSE_BUTTON);
    title->SetMaxSize(gfx::Size(200, 0));
    title->SetElideBehavior(gfx::ELIDE_TAIL);
    title->SetTooltipText(tabs[i].title);
    title->SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(3, 8)));
    title->SetProperty(
        views::kFlexBehaviorKey,
        views::FlexSpecification(views::LayoutOrientation::kHorizontal,
                                 views::MinimumFlexSizeRule::kScaleToMinimum,
                                 views::MaximumFlexSizeRule::kPreferred));
    tab->AddChildView(views::CreateVectorImageButtonWithNativeTheme(
                          base::BindRepeating(actions_.close, i),
                          vector_icons::kCloseSmallIcon, 14))
        ->SetTooltipText(u"Close tab (Ctrl+W)");
  }
  tab_row_
      ->AddChildView(views::CreateVectorImageButtonWithNativeTheme(
          actions_.new_tab, vector_icons::kAddOldIcon, 14))
      ->SetTooltipText(u"New tab (Ctrl+T)");
  InvalidateLayout();
}

void WindowView::PageClosed() {
  DropQuestions();
  CloseFind();
  ClearPage();
  shell_ = nullptr;
}

GURL WindowView::NewTabUrl() const {
  // Standard mode shows every site: a new tab is for going anywhere.
  return site_.empty() || StandardMode() ? GURL(url::kAboutBlankURL)
                                         : GURL("https://" + site_ + "/");
}

void WindowView::SetUrl(const GURL& url) {
  // Questions belong to the page that asked: another page drops them.
  if (url.GetWithoutRef() != url_.GetWithoutRef()) {
    DropQuestions();
  }
  url_ = url;
  site_ = SiteForUrl(url);
  chip_->SetVisible(!site_.empty());
  if (!site_.empty()) {
    chip_->SetText(base::UTF8ToUTF16(site_));
    chip_->SetBackground(views::CreateRoundedRectBackground(
        SiteColor(site_), kBarHeight / 2 - 6));
    // Plain http: say so on the chip itself.
    chip_->SetImageModel(
        views::Button::STATE_NORMAL,
        url.SchemeIs(url::kHttpScheme)
            ? ui::ImageModel::FromVectorIcon(
                  vector_icons::kNotSecureWarningOldIcon, SK_ColorWHITE, 14)
            : ui::ImageModel());
    chip_->SetTooltipText(url.SchemeIs(url::kHttpScheme)
                              ? u"Not secure: this page isn't encrypted"
                              : u"Edit address (Ctrl+L)");
  }
  address_->Show(url, AddressRest(url, site_));
  UpdateBookmarked();
  UpdateZoom();
  if (find_row_->GetVisible()) {
    find_count_->SetText(std::u16string());  // a new page: search again
  }
}

void WindowView::SetLoading(bool loading) {
  loading_ = loading;
  views::SetImageFromVectorIconWithColor(
      reload_,
      loading ? vector_icons::kCloseIcon
              : vector_icons::kReloadChromeRefreshOldIcon,
      kIconSize, views::IconColors(ui::kColorIcon, ui::kColorIconDisabled));
  reload_->SetTooltipText(loading ? u"Stop" : u"Reload (F5)");
}

void WindowView::EnableBack(bool enabled) {
  back_->SetEnabled(enabled || back_url_.is_valid());
}

void WindowView::GoBack() {
  if (!shell_) {
    return;
  }
  if (shell_->web_contents()->GetController().CanGoBack() ||
      !back_url_.is_valid()) {
    shell_->GoBackOrForward(-1);
    return;
  }
  // The first page here came from another site's window, which closed:
  // that site's window opens in this one's place, restoring its history,
  // and this one closes.
  VLOG(1) << "lrb: back to " << back_url_;
  LrbContentBrowserClient::Get()->OpenSiteWindow(
      SiteForUrl(back_url_), back_url_,
      LrbPlatformDelegate::WindowBoundsOf(shell_->web_contents()),
      /*back=*/GURL(), /*restore=*/true);
  CloseSoon();
}

void WindowView::EnableForward(bool enabled) {
  forward_->SetEnabled(enabled);
}

bool WindowView::HandleShortcut(const ui::KeyEvent& event) {
  if (!shell_) {
    return false;  // no page shown yet
  }
  const int mods = event.flags() & (ui::EF_CONTROL_DOWN | ui::EF_ALT_DOWN |
                                    ui::EF_SHIFT_DOWN);
  const ui::KeyboardCode key = event.key_code();
  const bool ctrl = mods == ui::EF_CONTROL_DOWN;
  const bool alt = mods == ui::EF_ALT_DOWN;
  if ((ctrl && key == ui::VKEY_L) || (alt && key == ui::VKEY_D)) {
    FocusAddress();
  } else if (!mods && key == ui::VKEY_F6) {
    CycleFocus(+1);
  } else if (mods == ui::EF_SHIFT_DOWN && key == ui::VKEY_F6) {
    CycleFocus(-1);
  } else if ((alt && key == ui::VKEY_LEFT) || key == ui::VKEY_BROWSER_BACK) {
    GoBack();
  } else if ((alt && key == ui::VKEY_RIGHT) ||
             key == ui::VKEY_BROWSER_FORWARD) {
    shell_->GoBackOrForward(1);
  } else if ((!mods && key == ui::VKEY_F5) || (ctrl && key == ui::VKEY_R)) {
    shell_->Reload();
  } else if ((mods == ui::EF_SHIFT_DOWN && key == ui::VKEY_F5) ||
             (mods == (ui::EF_CONTROL_DOWN | ui::EF_SHIFT_DOWN) &&
              key == ui::VKEY_R)) {
    shell_->ReloadBypassingCache();
  } else if ((alt && key == ui::VKEY_F) || (!mods && key == ui::VKEY_F10)) {
    ShowMenu();  // as other browsers: the menu from the keyboard
  } else if (ctrl && key == ui::VKEY_F) {
    OpenFind();
  } else if (find_row_->GetVisible() &&
             ((!mods && key == ui::VKEY_F3) || (ctrl && key == ui::VKEY_G))) {
    Find(/*forward=*/true, /*new_session=*/false);
  } else if (find_row_->GetVisible() &&
             ((mods == ui::EF_SHIFT_DOWN && key == ui::VKEY_F3) ||
              (mods == (ui::EF_CONTROL_DOWN | ui::EF_SHIFT_DOWN) &&
               key == ui::VKEY_G))) {
    Find(/*forward=*/false, /*new_session=*/false);
  } else if ((ctrl || mods == (ui::EF_CONTROL_DOWN | ui::EF_SHIFT_DOWN)) &&
             (key == ui::VKEY_OEM_PLUS || key == ui::VKEY_ADD)) {
    Zoom(+1);
  } else if (ctrl && (key == ui::VKEY_OEM_MINUS || key == ui::VKEY_SUBTRACT)) {
    Zoom(-1);
  } else if (ctrl && (key == ui::VKEY_0 || key == ui::VKEY_NUMPAD0)) {
    Zoom(0);
  } else if ((ctrl && key == ui::VKEY_TAB) ||
             (ctrl && key == ui::VKEY_NEXT)) {
    actions_.cycle.Run(+1);
  } else if ((mods == (ui::EF_CONTROL_DOWN | ui::EF_SHIFT_DOWN) &&
              key == ui::VKEY_TAB) ||
             (ctrl && key == ui::VKEY_PRIOR)) {
    actions_.cycle.Run(-1);
  } else if (ctrl && key >= ui::VKEY_1 && key <= ui::VKEY_8) {
    actions_.select.Run(static_cast<size_t>(key - ui::VKEY_1));
  } else if (ctrl && key == ui::VKEY_9) {
    actions_.select.Run(SIZE_MAX);  // the last tab
  } else if (ctrl && key == ui::VKEY_T) {
    NewPage();
  } else if (mods == (ui::EF_CONTROL_DOWN | ui::EF_SHIFT_DOWN) &&
             key == ui::VKEY_T) {
    actions_.reopen_closed.Run();
  } else if (ctrl && key == ui::VKEY_N) {
    actions_.new_window.Run(GURL());
  } else if (ctrl && key == ui::VKEY_D && star_->GetVisible()) {
    ToggleBookmark();
  } else if (ctrl && key == ui::VKEY_P && kCanPrint) {
    PrintToPdf(shell_->web_contents());
  } else if ((ctrl && key == ui::VKEY_W) || (ctrl && key == ui::VKEY_F4)) {
    CloseSoon();
  } else if (!mods && key == ui::VKEY_F11) {
    ToggleFullscreen();
  } else if (alt && key == ui::VKEY_HOME) {
    if (const GURL home = NewTabUrl(); home.SchemeIsHTTPOrHTTPS()) {
      shell_->LoadURL(home);  // the site's home page
    }
  } else if (!mods && key == ui::VKEY_ESCAPE && loading_ && web_view_ &&
             GetFocusManager() &&
             GetFocusManager()->GetFocusedView() == web_view_) {
    // Only while loading and with the page focused: in the address, the
    // find row or a question (sign-in, a permission) Esc cancels those.
    shell_->Stop();
  } else {
    return false;
  }
  return true;
}

void WindowView::FocusAddress() {
  address_->RequestFocus();
}

void WindowView::CycleFocus(int direction) {
  views::View* focused =
      GetFocusManager() ? GetFocusManager()->GetFocusedView() : nullptr;
  const bool question = question_row_->GetVisible();
  // Stops: 0 the page, 1 the address, 2 the question.
  int at = 0;
  if (focused == address_) {
    at = 1;
  } else if (question && focused && question_row_->Contains(focused)) {
    at = 2;
  }
  const int stops = question ? 3 : 2;
  const int next = (at + direction + stops) % stops;
  if (next == 1) {
    FocusAddress();
  } else if (next == 2) {
    FocusQuestionControl();
  } else if (shell_) {
    shell_->web_contents()->Focus();
  }
}

void WindowView::FocusQuestionControl() {
  if (questions_.empty()) {
    return;
  }
  switch (questions_.front().focus) {
    case Question::Focus::kAccept:
      accept_->RequestFocus();
      return;
    case Question::Focus::kCancel:
      cancel_->RequestFocus();
      return;
    case Question::Focus::kInput:
      question_input_->RequestFocus();
      return;
    case Question::Focus::kNone:
      break;
  }
  // No preference: the first control, never Allow by default.
  if (question_input_->GetVisible()) {
    question_input_->RequestFocus();
  } else if (cancel_->GetVisible()) {
    cancel_->RequestFocus();
  } else {
    accept_->RequestFocus();
  }
}

void WindowView::Navigate(std::u16string_view input, bool new_window) {
  const std::string text(base::TrimWhitespaceASCII(
      base::UTF16ToUTF8(input), base::TRIM_ALL));
  if (text.empty()) {
    return;
  }
  GURL url(text);
  const bool known_scheme =
      url.is_valid() &&
      (url.SchemeIsHTTPOrHTTPS() || url.SchemeIs(url::kAboutScheme) ||
       url.SchemeIs(url::kDataScheme) || url.SchemeIsFile());
  if (!known_scheme) {
    const bool looks_like_address =
        text.find(' ') == std::string::npos &&
        (text.find('.') != std::string::npos ||
         base::StartsWith(text, "localhost"));
    // https first; a search otherwise, with the user's search engine.
    if (!looks_like_address) {
      Search(base::UTF8ToUTF16(text),
             base::BindOnce(
                 [](base::WeakPtr<WindowView> view, bool new_window, GURL url) {
                   if (view) {
                     view->Open(new_window, url);
                   }
                 },
                 weak_factory_.GetWeakPtr(), new_window));
      return;
    }
    url = GURL("https://" + text);
  }
  Open(new_window, url);
}

void WindowView::Open(bool new_window, const GURL& url) {
  if (!url.is_valid() || !shell_) {
    return;
  }
  if (!new_window) {
    shell_->LoadURL(url);
    shell_->web_contents()->Focus();
    return;
  }
  address_->Revert();  // this window keeps its page
  LrbContentBrowserClient* client = LrbContentBrowserClient::Get();
  const std::string site = SiteForUrl(url);
  if (client && !client->standard() && !site.empty() &&
      site != client->site()) {
    // Lean mode, another site: its own instance, beside this window.
    client->OpenSiteWindow(site, url);
  } else {
    actions_.new_window.Run(url);
  }
}

// static
void WindowView::Search(std::u16string terms,
                        base::OnceCallback<void(GURL)> done) {
  // The settings file is read for each search (rare): a change made in
  // any window applies to all of them, with nothing watching the file.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce(
          [](std::u16string terms) {
            return SearchFor(Settings::Read().search_url, terms);
          },
          std::move(terms)),
      std::move(done));
}

void WindowView::SetBlocked(int count) {
  if (count > 0 && !blocked_->GetVisible()) {
    // Where the shield is in the window, once laid out (for the tests,
    // which click it).
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(
            [](base::WeakPtr<WindowView> view) {
              if (!view || !view->blocked_->GetVisible() ||
                  !view->GetWidget()) {
                return;
              }
              gfx::Point center =
                  view->blocked_->GetLocalBounds().CenterPoint();
              views::View::ConvertPointToTarget(
                  view->blocked_, view->GetWidget()->GetRootView(), &center);
              VLOG(1) << "lrb: blocking shield at " << center.x() << ","
                      << center.y();
            },
            weak_factory_.GetWeakPtr()),
        base::Milliseconds(500));
  }
  blocked_->SetVisible(count > 0);
  blocked_->SetText(base::NumberToString16(count));
  blocked_->SetTooltipText(base::NumberToString16(count) +
                           u" requests blocked on this page");
}

void WindowView::Ask(Question question) {
  questions_.push_back(std::move(question));
  if (questions_.size() == 1) {
    ShowNextQuestion();
  }
}

void WindowView::AskPermission(
    const std::u16string& text,
    bool focus,
    base::OnceCallback<void(std::optional<bool>)> answer) {
  Question question;
  question.text = text;
  question.accept_label = u"Allow";
  question.cancel_label = u"Block";
  question.focus = focus ? Question::Focus::kCancel : Question::Focus::kNone;
  question.answer = base::BindOnce(
      [](base::OnceCallback<void(std::optional<bool>)> answer,
         std::optional<bool> accepted,
         const std::u16string&) { std::move(answer).Run(accepted); },
      std::move(answer));
  Ask(std::move(question));
}

bool WindowView::HasQuestionsFrom(const void* owner) const {
  for (const Question& question : questions_) {
    if (question.owner == owner) {
      return true;
    }
  }
  return false;
}

bool WindowView::AnswerFrom(const void* owner,
                            bool accept,
                            const std::u16string* input) {
  if (questions_.empty() || questions_.front().owner != owner) {
    return false;
  }
  if (input) {
    question_input_->SetText(*input);
  }
  Answer(accept);
  return true;
}

void WindowView::DropQuestionsFrom(const void* owner) {
  const bool on_screen = !questions_.empty() && questions_.front().owner == owner;
  base::circular_deque<Question> kept;
  std::vector<Question> dropped;
  for (Question& question : questions_) {
    (question.owner == owner ? dropped.emplace_back(std::move(question))
                             : kept.emplace_back(std::move(question)));
  }
  questions_ = std::move(kept);
  if (on_screen) {
    ShowNextQuestion();
  }
  for (Question& question : dropped) {
    Unanswered(question);
  }
}

// static
void WindowView::Unanswered(Question& question) {
  if (question.login) {
    std::move(question.credentials).Run(std::nullopt);
  } else {
    std::move(question.answer).Run(std::nullopt, std::u16string());
  }
}

void WindowView::ShowNextQuestion() {
  if (questions_.empty()) {
    question_row_->SetVisible(false);
    return;
  }
  const Question& question = questions_.front();
  question_label_->SetText(question.text);
  question_input_->SetVisible(question.input || question.login);
  question_input_->SetText(question.default_input);
  question_input_->SetPlaceholderText(question.login ? u"Username" : u"");
  question_password_->SetVisible(question.login);
  question_password_->SetText(std::u16string());
  cancel_->SetVisible(!question.cancel_label.empty());
  cancel_->SetText(question.cancel_label);
  accept_->SetText(question.accept_label);
  question_row_->SetVisible(true);
  FocusQuestion();
}

void WindowView::FocusQuestion() {
  if (questions_.empty()) {
    return;
  }
  switch (questions_.front().focus) {
    case Question::Focus::kNone:
      break;
    case Question::Focus::kAccept:
      accept_->RequestFocus();
      break;
    case Question::Focus::kCancel:
      cancel_->RequestFocus();
      break;
    case Question::Focus::kInput:
      question_input_->RequestFocus();
      question_input_->SelectAll(false);
      break;
  }
}

void WindowView::Answer(std::optional<bool> accepted) {
  if (questions_.empty()) {
    return;
  }
  Question question = std::move(questions_.front());
  questions_.pop_front();
  const std::u16string input = std::u16string(question_input_->GetText());
  const std::u16string password(question_password_->GetText());
  question_password_->SetText(std::u16string());  // never kept on screen
  const bool had_focus = question_row_->Contains(
      GetFocusManager() ? GetFocusManager()->GetFocusedView() : nullptr);
  ShowNextQuestion();
  if (had_focus && questions_.empty() && shell_->web_contents()) {
    shell_->web_contents()->Focus();  // back to the page
  }
  if (question.login) {
    std::move(question.credentials)
        .Run(accepted.value_or(false)
                 ? std::make_optional(std::make_pair(input, password))
                 : std::nullopt);
    return;
  }
  std::move(question.answer).Run(accepted, question.input ? input : u"");
}

void WindowView::DropQuestions() {
  // Unanswered: refused (a permission isn't remembered).
  base::circular_deque<Question> dropped = std::move(questions_);
  questions_.clear();
  if (question_row_) {
    question_row_->SetVisible(false);
  }
  for (Question& question : dropped) {
    Unanswered(question);
  }
}

void WindowView::ContentsChanged(views::Textfield* sender,
                                 const std::u16string& new_contents) {
  if (sender == find_input_) {
    Find(/*forward=*/true, /*new_session=*/true);  // as you type
  } else if (sender == address_) {
    OnAddressTyped(new_contents);
  }
}

void WindowView::OnAddressTyped(const std::u16string& text) {
  // Completed inline only while typing on at the end: not after deleting
  // (the completion would come back), not when editing inside.
  const bool complete =
      text.size() > typed_.size() && text.starts_with(typed_) &&
      address_->GetSelectedRange().end() == text.size();
  typed_ = text;
  completion_.reset();
  const std::u16string trimmed(
      base::TrimWhitespace(text, base::TRIM_ALL));
  if (trimmed.empty()) {
    suggestions_->Hide();
    return;
  }
  lrb::Suggest(trimmed, base::BindOnce(&WindowView::OnSuggestions,
                                       weak_factory_.GetWeakPtr(), text,
                                       complete));
}

void WindowView::OnSuggestions(std::u16string typed,
                               bool complete,
                               std::vector<Suggestion> found) {
  if (typed != typed_ || !address_->HasFocus()) {
    return;  // typed on since, or gone
  }
  if (complete && !found.empty()) {
    std::string address(coordinator::DisplayAddress(found.front().url.spec()));
    if (address.ends_with('/') && address.find('/') == address.size() - 1) {
      address.pop_back();  // "github.com/" completes to "github.com"
    }
    const std::u16string completion = base::UTF8ToUTF16(address);
    const std::u16string lowered = base::ToLowerASCII(typed);
    if (completion.size() > typed.size() &&
        base::StartsWith(base::ToLowerASCII(completion), lowered)) {
      // The rest, selected: typing on replaces it, Enter takes it.
      address_->SetText(typed + completion.substr(typed.size()));
      address_->SetSelectedRange(
          gfx::Range(address_->GetText().size(), typed.size()));
      completion_.emplace(std::u16string(address_->GetText()),
                          found.front().url);
    }
  }
  suggestions_->Show(std::move(found));
}

void WindowView::OpenSuggestion(GURL url) {
  HideSuggestions();  // `url` may be the completion's: taken by value
  if (shell_ && url.SchemeIsHTTPOrHTTPS()) {
    shell_->LoadURL(url);  // another site's: its window, as typed
    shell_->web_contents()->Focus();
  }
}

void WindowView::HideSuggestions() {
  typed_.clear();
  completion_.reset();
  if (suggestions_) {
    suggestions_->Hide();
  }
}

void WindowView::UpdateBookmarked() {
  const bool can = !site_.empty() && url_.SchemeIsHTTPOrHTTPS() &&
                   LrbContentBrowserClient::Get() &&
                   LrbContentBrowserClient::Get()->has_coordinator();
  star_->SetVisible(can);
  if (!can) {
    return;
  }
  IsBookmarked(url_, base::BindOnce(
                         [](base::WeakPtr<WindowView> view, GURL url,
                            bool bookmarked) {
                           if (view && view->url_ == url) {
                             view->SetBookmarked(bookmarked);
                           }
                         },
                         weak_factory_.GetWeakPtr(), url_));
}

void WindowView::ToggleBookmark() {
  if (!shell_ || !star_->GetVisible()) {
    return;
  }
  if (bookmarked_) {
    RemoveBookmark(url_);
  } else {
    AddBookmark(url_, shell_->web_contents()->GetTitle());
  }
  SetBookmarked(!bookmarked_);
  // The coordinator has the last word (a page of another site in a flow
  // can't be bookmarked here): asked again.
  UpdateBookmarked();
}

void WindowView::SetBookmarked(bool bookmarked) {
  bookmarked_ = bookmarked;
  if (bookmarked) {
    views::SetImageFromVectorIconWithColor(
        star_, vector_icons::kStarFilledIcon, kIconSize,
        views::IconColors(SkColorSetRGB(0x1a, 0x73, 0xe8),
                          ui::kColorIconDisabled));
  } else {
    views::SetImageFromVectorIconWithColor(
        star_, kStarOutlineIcon, kIconSize,
        views::IconColors(ui::kColorIcon, ui::kColorIconDisabled));
  }
  star_->SetTooltipText(bookmarked ? u"Remove bookmark (Ctrl+D)"
                                   : u"Bookmark this page (Ctrl+D)");
}

void WindowView::ShowDownload(const DownloadStatus& status) {
  using State = DownloadStatus::State;
  if (status.state == State::kCanceled) {
    std::erase_if(downloads_, [&status](const DownloadStatus& d) {
      return d.id == status.id;
    });
    if (download_status_ && download_status_->id == status.id) {
      download_status_.reset();
      download_->SetVisible(false);
    }
    return;
  }
  download_status_ = status;
  auto known = std::find_if(
      downloads_.begin(), downloads_.end(),
      [&status](const DownloadStatus& d) { return d.id == status.id; });
  if (known != downloads_.end()) {
    *known = status;
  } else {
    downloads_.insert(downloads_.begin(), status);
    if (downloads_.size() > kMaxDownloads) {
      downloads_.pop_back();
    }
  }
  std::u16string text = status.name;
  if (status.state == State::kInProgress && status.percent >= 0) {
    text += u" " + base::NumberToString16(status.percent) + u"%";
  } else if (status.state == State::kFailed) {
    text += u": failed";
  }
  download_->SetText(text);
  download_->SetTooltipText(
      (status.saved_without_asking
           ? u"Couldn't ask where to save (no file picker): saved to "
           : u"Saving to ") +
      status.path.LossyDisplayName());
  download_->SetVisible(true);
}

void WindowView::ShowBlockingInfo() {
  int blocked = 0;
  base::StringToInt(blocked_->GetText(), &blocked);
  std::optional<bool> enabled;
  if (ContentBlocker::loaded() && !site_.empty()) {
    // Standard mode: per site in the one instance; lean: the instance's.
    enabled = StandardMode() ? ContentBlocker::EnabledFor(url_)
                             : ContentBlocker::enabled();
  }
  ShowBlockingBubble(
      blocked_, blocked, site_, enabled,
      base::BindOnce(
          [](base::WeakPtr<WindowView> view) {
            if (view) {
              view->ToggleBlocking();
            }
          },
          weak_factory_.GetWeakPtr()),
      base::BindOnce(
          [](base::WeakPtr<WindowView> view) {
            if (view) {
              view->OpenSettings();
            }
          },
          weak_factory_.GetWeakPtr()));
}

void WindowView::OpenSettings() {
  // Confined under the coordinator, this instance can't write the
  // settings: the coordinator opens them in a process of their own.
  if (LrbContentBrowserClient* client = LrbContentBrowserClient::Get();
      client && client->has_coordinator()) {
    client->SendToCoordinator("open-settings");
  } else {
    ShowSettings(weak_factory_.GetWeakPtr());
  }
}

void WindowView::ToggleBlocking() {
  if (LrbContentBrowserClient* client = LrbContentBrowserClient::Get();
      client && client->standard()) {
    // One instance for every site: off (or on) for this site only. The
    // client saves the list and reloads the site's pages in new renderers
    // (theirs have the old list), on as well as off.
    ContentBlocker::SetSiteEnabled(site_, !ContentBlocker::EnabledFor(url_));
    client->ReloadSiteBypassingCache(site_);
    return;
  }
  const bool enable = !ContentBlocker::enabled();
  ContentBlocker::SetEnabled(enable);
  // Kept in the site's profile (read at startup: lrb_main_delegate.cc).
  base::ThreadPool::PostTask(
      FROM_HERE, {base::MayBlock(), base::TaskShutdownBehavior::BLOCK_SHUTDOWN},
      base::BindOnce(
          [](base::FilePath marker, bool enable) {
            if (enable) {
              base::DeleteFile(marker);
            } else {
              base::WriteFile(marker, "");
            }
          },
          shell_->web_contents()->GetBrowserContext()->GetPath().Append(
              kBlockingOffFile),
          enable));
  // Bypassing caches: a plain reload may take the page's scripts from
  // memory, never asking the (now different) blocker.
  shell_->ReloadBypassingCache();
}

void WindowView::AskForgetSite() {
  if (site_.empty() || !shell_) {
    return;
  }
  Question question;
  question.text =
      u"Forget what " + base::UTF8ToUTF16(site_) +
      u" stored on this computer? Its cookies (you will be signed out), "
      u"site data and cached files go, with the permissions you gave it and "
      u"its zoom. Bookmarks and history stay.";
  question.accept_label = u"Forget";
  question.cancel_label = u"Cancel";
  question.focus = Question::Focus::kCancel;  // a stray Enter keeps it all
  question.answer = base::BindOnce(
      [](base::WeakPtr<WindowView> view, std::string site,
         std::optional<bool> accepted, const std::u16string&) {
        if (view && accepted.value_or(false)) {
          view->ForgetSiteData(site);
        }
      },
      weak_factory_.GetWeakPtr(), site_);
  Ask(std::move(question));
}

void WindowView::ForgetSiteData(const std::string& site) {
  if (!shell_) {
    return;
  }
  content::BrowserContext* context =
      shell_->web_contents()->GetBrowserContext();
  // Everything under the registrable domain: its subdomains' cookies and
  // storage too (mail.google.com's for google.com).
  std::unique_ptr<content::BrowsingDataFilterBuilder> filter =
      content::BrowsingDataFilterBuilder::Create(
          content::BrowsingDataFilterBuilder::Mode::kDelete);
  filter->AddRegisterableDomain(site);
  content::BrowsingDataRemover* remover = context->GetBrowsingDataRemover();
  // The pages reload once it's gone (bypassing the cache, which went too).
  auto* observer = new RemovalObserver(
      remover, base::BindOnce(
                   [](std::string site) {
                     for (Shell* shell : Shell::windows()) {
                       if (SiteForUrl(shell->web_contents()
                                          ->GetLastCommittedURL()) == site) {
                         shell->ReloadBypassingCache();
                       }
                     }
                   },
                   site));
  remover->RemoveWithFilterAndReply(
      base::Time(), base::Time::Max(),
      content::BrowsingDataRemover::DATA_TYPE_COOKIES |
          content::BrowsingDataRemover::DATA_TYPE_DOM_STORAGE |
          content::BrowsingDataRemover::DATA_TYPE_CACHE,
      content::BrowsingDataRemover::ORIGIN_TYPE_UNPROTECTED_WEB,
      std::move(filter), observer);
  static_cast<LrbPermissionManager*>(context->GetPermissionControllerDelegate())
      ->answers()
      .ForgetSite(site);
  ZoomLevels::ForgetSite(context, site);
}

void WindowView::OpenReleases() {
  if (LrbContentBrowserClient* client = LrbContentBrowserClient::Get()) {
    const GURL url(kReleases);
    client->OpenSiteWindow(SiteForUrl(url), url);
  }
}

void WindowView::OpenSnapConnections() {
  if (snap_notice_) {
    // Seen: gone from this window. Hidden, not removed: this may run in
    // its own click.
    snap_notice_->SetVisible(false);
    VLOG(1) << "lrb: snap notice: seen";
  }
  ShowSnapConnections(weak_factory_.GetWeakPtr());
}

void WindowView::ShowInFolder(const DownloadStatus& download) {
  if (!download.broker_id.empty()) {
    if (LrbContentBrowserClient* client = LrbContentBrowserClient::Get()) {
      client->SendToCoordinator("show-saved " + download.broker_id);
    }
    return;
  }
  // The desktop's file manager, on the folder (never opens the file).
  base::ThreadPool::PostTask(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(
          [](base::FilePath folder) {
            base::Process process = base::LaunchProcess(
                {"xdg-open", folder.value()}, base::LaunchOptions());
            if (process.IsValid()) {
              base::EnsureProcessGetsReaped(std::move(process));
            }
          },
          download.path.DirName()));
}

void WindowView::ShowDownloadMenu() {
  if (!download_status_ && downloads_.empty()) {
    return;
  }
  download_menu_model_.Clear();
  if (download_status_ &&
      download_status_->state == DownloadStatus::State::kInProgress) {
    download_menu_model_.AddItem(kCancelDownload, u"Cancel download");
  } else if (download_status_) {
    download_menu_model_.AddItem(kShowDownload, u"Show in folder");
    download_menu_model_.AddItem(kDismissDownload, u"Dismiss");
  }
  // This window's last downloads: a click shows one in its folder.
  if (downloads_.size() > 1 || !download_status_) {
    download_menu_model_.AddSeparator(ui::NORMAL_SEPARATOR);
    download_menu_model_.AddTitle(u"Recent downloads: show in folder");
    for (size_t i = 0; i < downloads_.size(); ++i) {
      const DownloadStatus& download = downloads_[i];
      std::u16string label = download.name;
      if (download.state == DownloadStatus::State::kInProgress &&
          download.percent >= 0) {
        label += u" " + base::NumberToString16(download.percent) + u"%";
      } else if (download.state == DownloadStatus::State::kFailed) {
        label += u": failed";
      }
      download_menu_model_.AddItem(kDownloadFirst + static_cast<int>(i),
                                   label);
    }
  }
  menu_runner_ = std::make_unique<views::MenuRunner>(
      &download_menu_model_, views::MenuRunner::HAS_MNEMONICS);
  menu_runner_->RunMenuAt(GetWidget(), nullptr, download_->GetBoundsInScreen(),
                          views::MenuAnchorPosition::kTopRight,
                          ui::mojom::MenuSourceType::kMouse);
}

void WindowView::OpenFind() {
  find_row_->SetVisible(true);
  find_input_->RequestFocus();
  find_input_->SelectAll(false);
  if (!find_input_->GetText().empty()) {
    Find(/*forward=*/true, /*new_session=*/true);
  }
}

void WindowView::CloseFind() {
  if (!find_row_->GetVisible()) {
    return;
  }
  if (!shell_) {
    find_row_->SetVisible(false);
    return;
  }
  // The match found stays selected, as in other browsers.
  shell_->web_contents()->StopFinding(content::STOP_FIND_ACTION_KEEP_SELECTION);
  find_row_->SetVisible(false);
  find_count_->SetText(std::u16string());
  shell_->web_contents()->Focus();
}

void WindowView::Find(bool forward, bool new_session) {
  const std::u16string text(find_input_->GetText());
  content::WebContents* contents = shell_->web_contents();
  if (text.empty()) {
    contents->StopFinding(content::STOP_FIND_ACTION_CLEAR_SELECTION);
    find_count_->SetText(std::u16string());
    return;
  }
  auto options = blink::mojom::FindOptions::New();
  options->forward = forward;
  options->new_session = new_session;
  // The reply handler shows the counts of whatever session is current, so
  // the request id Find hands back is not needed.
  contents->Find(text, std::move(options), /*skip_delay=*/!new_session,
                 [](int /*request_id*/) {});
}

void WindowView::OnFindReply(int matches, int active, bool final_update) {
  if (!find_row_->GetVisible()) {
    return;
  }
  if (matches > 0) {
    find_count_->SetText(base::NumberToString16(std::max(active, 1)) +
                         u" of " + base::NumberToString16(matches));
  } else if (final_update) {
    find_count_->SetText(u"No matches");
  }
}

void WindowView::Zoom(int direction) {
  content::WebContents* contents = shell_->web_contents();
  if (direction == 0) {
    content::HostZoomMap::SetZoomLevel(contents, 0);
    return;
  }
  // The usual steps (as other browsers).
  static constexpr double kFactors[] = {0.25, 0.33, 0.5, 0.67, 0.75, 0.8,
                                        0.9,  1.0,  1.1, 1.25, 1.5,  1.75,
                                        2.0,  2.5,  3.0, 4.0,  5.0};
  const double current = blink::ZoomLevelToZoomFactor(
      content::HostZoomMap::GetZoomLevel(contents));
  double next = current;
  if (direction > 0) {
    for (double factor : kFactors) {
      if (factor > current + 0.001) {
        next = factor;
        break;
      }
    }
  } else {
    for (double factor : kFactors) {
      if (factor < current - 0.001) {
        next = factor;
      }
    }
  }
  content::HostZoomMap::SetZoomLevel(contents,
                                     blink::ZoomFactorToZoomLevel(next));
}

void WindowView::UpdateZoom() {
  content::WebContents* contents = shell_ ? shell_->web_contents() : nullptr;
  if (!contents) {
    return;
  }
  const int percent = static_cast<int>(std::lround(
      100 * blink::ZoomLevelToZoomFactor(
                content::HostZoomMap::GetZoomLevel(contents))));
  zoom_->SetVisible(percent != 100);
  zoom_->SetText(base::NumberToString16(percent) + u"%");
}

void WindowView::AddCapture(bool audio, bool video) {
  audio_captures_ += audio;
  video_captures_ += video;
  UpdateCapture();
}

void WindowView::RemoveCapture(bool audio, bool video) {
  audio_captures_ -= audio;
  video_captures_ -= video;
  UpdateCapture();
}

void WindowView::UpdateCapture() {
  camera_in_use_->SetVisible(video_captures_ > 0);
  microphone_in_use_->SetVisible(audio_captures_ > 0);
}

void WindowView::ShowMenu() {
  if (!shell_) {
    return;
  }
  ListBookmarks(base::BindOnce(&WindowView::ShowMenuWith,
                               weak_factory_.GetWeakPtr()));
}

void WindowView::ShowMenuWith(std::vector<Suggestion> bookmarks) {
  if (!shell_ || !GetWidget()) {
    return;
  }
  // Rebuilt each time: the bookmarks, the current site's remembered
  // permission answers, each a way to forget it (the site asks again next
  // time).
  menu_model_.Clear();
  menu_model_.AddItem(kNewPage, u"New tab");
  menu_model_.AddItem(kNewWindow, u"New window");
  menu_model_.AddItem(kClosePage, u"Close tab");
  if (LrbPlatformDelegate::CanReopenClosed()) {
    menu_model_.AddItem(kReopenClosed, u"Reopen closed tab");
  }
  bookmarks_menu_model_.Clear();
  bookmark_urls_.clear();
  for (const Suggestion& bookmark : bookmarks) {
    bookmarks_menu_model_.AddItem(
        kBookmarkFirst + static_cast<int>(bookmark_urls_.size()),
        bookmark.title.empty() ? base::UTF8ToUTF16(bookmark.url.spec())
                               : bookmark.title);
    bookmark_urls_.push_back(bookmark.url);
  }
  history_menu_model_.Clear();
  history_urls_.clear();
  if (History* history =
          History::For(shell_->web_contents()->GetBrowserContext())) {
    for (const History::Page& page : history->Recent(kMaxHistoryItems)) {
      history_menu_model_.AddItem(
          kHistoryFirst + static_cast<int>(history_urls_.size()),
          page.title.empty() ? base::UTF8ToUTF16(page.url.spec())
                             : page.title);
      history_urls_.push_back(page.url);
    }
  }
  if (!bookmark_urls_.empty() || !history_urls_.empty()) {
    menu_model_.AddSeparator(ui::NORMAL_SEPARATOR);
  }
  if (!bookmark_urls_.empty()) {
    menu_model_.AddSubMenu(kBookmarks, u"Bookmarks", &bookmarks_menu_model_);
  }
  if (!history_urls_.empty()) {
    menu_model_.AddSubMenu(kHistory, u"History", &history_menu_model_);
  }
  if (kCanPrint || !site_.empty()) {
    menu_model_.AddSeparator(ui::NORMAL_SEPARATOR);
  }
  if (kCanPrint) {
    menu_model_.AddItem(kPrint, u"Print to PDF...");
  }
  if (!site_.empty()) {
    menu_model_.AddCheckItem(kDarkPages,
                             u"Dark pages on " + base::UTF8ToUTF16(site_));
    if (url_.SchemeIsHTTPOrHTTPS()) {
      menu_model_.AddItem(kForgetSite, u"Forget this site's data...");
    }
  }
  forget_.clear();
  if (url_.SchemeIsHTTPOrHTTPS()) {
    LrbPermissionManager* permissions = static_cast<LrbPermissionManager*>(
        shell_->web_contents()
            ->GetBrowserContext()
            ->GetPermissionControllerDelegate());
    for (const auto& [type, allowed] :
         permissions->answers().List(url::Origin::Create(url_))) {
      if (forget_.empty()) {
        menu_model_.AddSeparator(ui::NORMAL_SEPARATOR);
      }
      menu_model_.AddItem(kForgetFirst + static_cast<int>(forget_.size()),
                          std::u16string(FindAskable(type)->name) +
                              (allowed ? u" allowed" : u" blocked") +
                              u": forget");
      forget_.push_back(type);
    }
  }
  const bool standard = StandardMode();
  if (ContentBlocker::loaded() && !site_.empty()) {
    // Standard mode: per site in the one instance; lean: the instance's.
    const bool blocking =
        standard ? ContentBlocker::EnabledFor(url_) : ContentBlocker::enabled();
    menu_model_.AddSeparator(ui::NORMAL_SEPARATOR);
    menu_model_.AddItem(kToggleBlocking,
                        (blocking ? u"Turn off content blocking on "
                                  : u"Turn on content blocking on ") +
                            base::UTF8ToUTF16(site_));
  }
  menu_model_.AddSeparator(ui::NORMAL_SEPARATOR);
  if (RunningAsSnap()) {
    menu_model_.AddItem(kSnapConnections, u"Snap connections...");
  }
  menu_model_.AddItem(kSettings, u"Settings...");
  menu_model_.AddItem(kAbout, u"About lrb");
  menu_model_.AddSeparator(ui::NORMAL_SEPARATOR);
  menu_model_.AddItem(kCloseSite, standard ? u"Close all windows"
                                           : u"Close all pages of this site");

  menu_runner_ = std::make_unique<views::MenuRunner>(
      &menu_model_, views::MenuRunner::HAS_MNEMONICS);
  const gfx::Rect bounds = GetBoundsInScreen();
  menu_runner_->RunMenuAt(
      GetWidget(), nullptr,
      gfx::Rect(bounds.right() - 4, bounds.y() + kBarHeight - 4, 0, 0),
      views::MenuAnchorPosition::kTopRight, ui::mojom::MenuSourceType::kMouse);
}

void WindowView::NewPage() {
  actions_.new_tab.Run();
}

gfx::Size WindowView::GetMinimumSize() const {
  return gfx::Size();  // windows may shrink below their first size
}

void WindowView::ToggleFullscreen() {
  views::Widget* widget = GetWidget();
  if (!widget) {
    return;
  }
  const bool fullscreen = !widget->IsFullscreen();
  widget->SetFullscreen(fullscreen);
  bar_->SetVisible(!fullscreen && !Shell::ShouldHideToolbar());
  tab_row_->SetVisible(!fullscreen && tab_row_->children().size() > 1);
  InvalidateLayout();
}

void WindowView::CloseSoon() {
  // Not from within the key or menu event: closing destroys this view and
  // the window the event is being dispatched in.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<WindowView> view) {
                       if (view) {
                         LrbPlatformDelegate::CloseByUser(view->shell_);
                       }
                     },
                     weak_factory_.GetWeakPtr()));
}

void WindowView::AddedToWidget() {
  shortcuts_ = std::make_unique<ShortcutHandler>(
      GetWidget()->GetNativeWindow(), this);
}

bool WindowView::HandleKeyEvent(views::Textfield* sender,
                                const ui::KeyEvent& key_event) {
  if (key_event.type() != ui::EventType::kKeyPressed) {
    return false;
  }
  if (sender == find_input_) {
    if (key_event.key_code() == ui::VKEY_RETURN) {
      Find(/*forward=*/!key_event.IsShiftDown(), /*new_session=*/false);
      return true;
    }
    if (key_event.key_code() == ui::VKEY_ESCAPE) {
      CloseFind();
      return true;
    }
    return false;
  }
  if (sender == question_input_ || sender == question_password_) {
    if (key_event.key_code() == ui::VKEY_RETURN) {
      // A sign-in: Enter in the username goes on to the password.
      if (sender == question_input_ && question_password_->GetVisible()) {
        question_password_->RequestFocus();
        return true;
      }
      Answer(true);
      return true;
    }
    if (key_event.key_code() == ui::VKEY_ESCAPE) {
      Answer(false);
      return true;
    }
    return false;
  }
  if (key_event.key_code() == ui::VKEY_RETURN) {
    // Shift+Enter or Alt+Enter: in a new window.
    const int mods = key_event.flags() & (ui::EF_SHIFT_DOWN | ui::EF_ALT_DOWN |
                                          ui::EF_CONTROL_DOWN);
    const bool new_window =
        mods == ui::EF_SHIFT_DOWN || mods == ui::EF_ALT_DOWN;
    const std::u16string text(address_->GetText());
    if (completion_ && completion_->first == text) {
      if (!new_window) {
        OpenSuggestion(completion_->second);
        return true;
      }
      const GURL url = completion_->second;
      HideSuggestions();
      Open(/*new_window=*/true, url);
      return true;
    }
    HideSuggestions();
    Navigate(text, new_window);
    return true;
  }
  if (key_event.key_code() == ui::VKEY_ESCAPE) {
    if (suggestions_->visible()) {
      // First what was typed, without the list or a completion.
      const std::u16string typed = typed_;
      HideSuggestions();
      address_->SetText(typed);
      return true;
    }
    address_->Revert();
    shell_->web_contents()->Focus();
    return true;
  }
  if ((key_event.key_code() == ui::VKEY_DOWN ||
       key_event.key_code() == ui::VKEY_UP) &&
      suggestions_->visible()) {
    const Suggestion* selected =
        suggestions_->Move(key_event.key_code() == ui::VKEY_DOWN ? 1 : -1);
    completion_.reset();
    address_->SetText(selected ? base::UTF8ToUTF16(selected->url.spec())
                               : typed_);
    address_->SetSelectedRange(gfx::Range(address_->GetText().size()));
    return true;
  }
  return false;
}

void WindowView::ExecuteCommand(int command_id, int event_flags) {
  if (command_id >= kBookmarkFirst &&
      command_id < kBookmarkFirst + static_cast<int>(bookmark_urls_.size())) {
    OpenSuggestion(bookmark_urls_[command_id - kBookmarkFirst]);
    return;
  }
  if (command_id >= kHistoryFirst &&
      command_id < kHistoryFirst + static_cast<int>(history_urls_.size())) {
    OpenSuggestion(history_urls_[command_id - kHistoryFirst]);
    return;
  }
  if (command_id >= kDownloadFirst &&
      command_id < kDownloadFirst + static_cast<int>(downloads_.size())) {
    ShowInFolder(downloads_[command_id - kDownloadFirst]);
    return;
  }
  if (command_id >= kForgetFirst &&
      command_id < kForgetFirst + static_cast<int>(forget_.size())) {
    static_cast<LrbPermissionManager*>(shell_->web_contents()
                                           ->GetBrowserContext()
                                           ->GetPermissionControllerDelegate())
        ->answers()
        .Forget(url::Origin::Create(url_), forget_[command_id - kForgetFirst]);
    return;
  }
  switch (command_id) {
    case kCancelDownload:
      if (download_status_) {
        LrbDownloadManagerDelegate::Cancel(
            shell_->web_contents()->GetBrowserContext(), download_status_->id);
      }
      break;
    case kShowDownload:
      if (download_status_) {
        ShowInFolder(*download_status_);
      }
      break;
    case kForgetSite:
      AskForgetSite();
      break;
    case kToggleBlocking:
      ToggleBlocking();
      break;
    case kSettings:
      OpenSettings();
      break;
    case kAbout:
      ShowAbout(weak_factory_.GetWeakPtr(),
                base::BindRepeating(
                    [](base::WeakPtr<WindowView> view, const GURL& url) {
                      if (view && view->shell_) {
                        LrbPlatformDelegate::OpenTab(view->shell_, url);
                      }
                    },
                    weak_factory_.GetWeakPtr()));
      break;
    case kSnapConnections:
      OpenSnapConnections();
      break;
    case kDismissDownload:
      download_status_.reset();
      download_->SetVisible(false);
      break;
    case kNewPage:
      NewPage();
      break;
    case kNewWindow:
      actions_.new_window.Run(GURL());
      break;
    case kReopenClosed:
      actions_.reopen_closed.Run();
      break;
    case kPrint:
      PrintToPdf(shell_->web_contents());
      break;
    case kDarkPages:
      SetDarkPages(shell_->web_contents()->GetBrowserContext(),
                   shell_->web_contents()->GetLastCommittedURL(),
                   !DarkPagesOn(shell_->web_contents()->GetLastCommittedURL()));
      break;
    case kClosePage:
      CloseSoon();
      break;
    case kCloseSite:
      // Every window of this instance (lean mode: all this site's; standard:
      // all); each page may ask "leave this page?". Posted: closing destroys
      // this view, inside its menu's callback.
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, base::BindOnce(&LrbPlatformDelegate::CloseAllByUser));
      break;
  }
}

bool WindowView::IsCommandIdChecked(int command_id) const {
  return command_id == kDarkPages &&
         DarkPagesOn(shell_->web_contents()->GetLastCommittedURL());
}

bool WindowView::IsCommandIdEnabled(int command_id) const {
  if (command_id >= kDownloadFirst &&
      command_id < kDownloadFirst + static_cast<int>(downloads_.size())) {
    // Only a finished file is in its folder.
    return downloads_[command_id - kDownloadFirst].state ==
           DownloadStatus::State::kDone;
  }
  return true;
}

bool WindowView::GetAcceleratorForCommandId(
    int command_id,
    ui::Accelerator* accelerator) const {
  switch (command_id) {
    case kNewPage:
      *accelerator = ui::Accelerator(ui::VKEY_T, ui::EF_CONTROL_DOWN);
      return true;
    case kClosePage:
      *accelerator = ui::Accelerator(ui::VKEY_W, ui::EF_CONTROL_DOWN);
      return true;
    case kNewWindow:
      *accelerator = ui::Accelerator(ui::VKEY_N, ui::EF_CONTROL_DOWN);
      return true;
    case kReopenClosed:
      *accelerator = ui::Accelerator(
          ui::VKEY_T, ui::EF_CONTROL_DOWN | ui::EF_SHIFT_DOWN);
      return true;
    case kPrint:
      *accelerator = ui::Accelerator(ui::VKEY_P, ui::EF_CONTROL_DOWN);
      return true;
  }
  return false;
}

BEGIN_METADATA(WindowView)
END_METADATA

}  // namespace lrb
