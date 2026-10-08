// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/ui/window_view.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/hash/hash.h"
#include "base/logging.h"
#include "base/process/kill.h"
#include "base/process/launch.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "components/vector_icons/vector_icons.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/host_zoom_map.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/stop_find_action.h"
#include "lrb/browser/shell.h"
#include "lrb/browser/blocked_counter.h"
#include "lrb/browser/download_manager_delegate.h"
#include "lrb/browser/lrb_content_browser_client.h"
#include "lrb/browser/permission_manager.h"
#include "lrb/browser/permissions/site_permissions.h"
#include "lrb/browser/site.h"
#include "lrb/browser/ui/lrb_platform_delegate.h"
#include "lrb/browser/ui/settings_dialog.h"
#include "lrb/common/content_blocker.h"
#include "lrb/common/settings.h"
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
  }

 private:
  GURL url_;
  std::u16string rest_;
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
  address_->SetProperty(
      views::kFlexBehaviorKey,
      views::FlexSpecification(views::LayoutOrientation::kHorizontal,
                               views::MinimumFlexSizeRule::kScaleToMinimum,
                               views::MaximumFlexSizeRule::kUnbounded));

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

  // The blocked count; a click offers turning blocking off for the site.
  blocked_ = bar->AddChildView(std::make_unique<views::LabelButton>(
      base::BindRepeating(&WindowView::ShowMenu, base::Unretained(this))));
  blocked_->SetImageModel(
      views::Button::STATE_NORMAL,
      ui::ImageModel::FromVectorIcon(vector_icons::kShieldIcon,
                                     ui::kColorIcon, 16));
  blocked_->SetEnabledTextColors(ui::kColorLabelForegroundSecondary);
  blocked_->SetVisible(false);

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

void WindowView::SetTabs(const std::vector<std::u16string>& titles,
                         size_t active) {
  tab_row_->RemoveAllChildViews();
  // Hidden in fullscreen (F11) too.
  tab_row_->SetVisible(titles.size() > 1 &&
                       !(GetWidget() && GetWidget()->IsFullscreen()));
  if (titles.size() <= 1) {
    InvalidateLayout();
    return;
  }
  for (size_t i = 0; i < titles.size(); ++i) {
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
        titles[i]));
    title->SetTriggerableEventFlags(ui::EF_LEFT_MOUSE_BUTTON |
                                    ui::EF_MIDDLE_MOUSE_BUTTON);
    title->SetMaxSize(gfx::Size(200, 0));
    title->SetElideBehavior(gfx::ELIDE_TAIL);
    title->SetTooltipText(titles[i]);
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
  return site_.empty() ? GURL(url::kAboutBlankURL)
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
  if ((ctrl && key == ui::VKEY_L) || (alt && key == ui::VKEY_D) ||
      (!mods && key == ui::VKEY_F6)) {
    FocusAddress();
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
    actions_.new_window.Run();
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

void WindowView::Navigate(std::u16string_view input) {
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
                 [](base::WeakPtr<WindowView> view, GURL url) {
                   if (view) {
                     view->shell_->LoadURL(url);
                     view->shell_->web_contents()->Focus();
                   }
                 },
                 weak_factory_.GetWeakPtr()));
      return;
    }
    url = GURL("https://" + text);
  }
  if (url.is_valid()) {
    shell_->LoadURL(url);
    shell_->web_contents()->Focus();
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
  }
}

void WindowView::ShowDownload(const DownloadStatus& status) {
  using State = DownloadStatus::State;
  if (status.state == State::kCanceled) {
    if (download_status_ && download_status_->id == status.id) {
      download_status_.reset();
      download_->SetVisible(false);
    }
    return;
  }
  download_status_ = status;
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

void WindowView::ToggleBlocking() {
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

void WindowView::ShowDownloadMenu() {
  if (!download_status_) {
    return;
  }
  download_menu_model_.Clear();
  if (download_status_->state == DownloadStatus::State::kInProgress) {
    download_menu_model_.AddItem(kCancelDownload, u"Cancel download");
  } else {
    download_menu_model_.AddItem(kShowDownload, u"Show in folder");
    download_menu_model_.AddItem(kDismissDownload, u"Dismiss");
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
  contents->Find(++find_request_id_, text, std::move(options),
                 /*skip_delay=*/!new_session);
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
  // Rebuilt each time: the current site's remembered permission answers,
  // each a way to forget it (the site asks again next time).
  menu_model_.Clear();
  menu_model_.AddItem(kNewPage, u"New tab");
  menu_model_.AddItem(kNewWindow, u"New window");
  menu_model_.AddItem(kClosePage, u"Close tab");
  if (LrbPlatformDelegate::CanReopenClosed()) {
    menu_model_.AddItem(kReopenClosed, u"Reopen closed tab");
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
  if (ContentBlocker::loaded() && !site_.empty()) {
    menu_model_.AddSeparator(ui::NORMAL_SEPARATOR);
    menu_model_.AddItem(
        kToggleBlocking,
        (ContentBlocker::enabled() ? u"Turn off content blocking on "
                                   : u"Turn on content blocking on ") +
            base::UTF8ToUTF16(site_));
  }
  menu_model_.AddSeparator(ui::NORMAL_SEPARATOR);
  menu_model_.AddItem(kSettings, u"Settings...");
  menu_model_.AddSeparator(ui::NORMAL_SEPARATOR);
  menu_model_.AddItem(kCloseSite, u"Close all pages of this site");

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
    Navigate(address_->GetText());
    return true;
  }
  if (key_event.key_code() == ui::VKEY_ESCAPE) {
    address_->Revert();
    shell_->web_contents()->Focus();
    return true;
  }
  return false;
}

void WindowView::ExecuteCommand(int command_id, int event_flags) {
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
      if (download_status_ && !download_status_->broker_id.empty()) {
        if (LrbContentBrowserClient* client = LrbContentBrowserClient::Get()) {
          client->SendToCoordinator("show-saved " +
                                    download_status_->broker_id);
        }
      } else if (download_status_) {
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
                download_status_->path.DirName()));
      }
      break;
    case kToggleBlocking:
      ToggleBlocking();
      break;
    case kSettings:
      // Confined under the coordinator, this instance can't write the
      // settings: the coordinator opens them in a process of their own.
      if (LrbContentBrowserClient* client = LrbContentBrowserClient::Get();
          client && client->has_coordinator()) {
        client->SendToCoordinator("open-settings");
      } else {
        ShowSettings(weak_factory_.GetWeakPtr());
      }
      break;
    case kDismissDownload:
      download_status_.reset();
      download_->SetVisible(false);
      break;
    case kNewPage:
      NewPage();
      break;
    case kNewWindow:
      actions_.new_window.Run();
      break;
    case kReopenClosed:
      actions_.reopen_closed.Run();
      break;
    case kClosePage:
      CloseSoon();
      break;
    case kCloseSite:
      // Every window of this instance shows this site; each page may ask
      // "leave this page?". Posted: closing destroys this view, inside its
      // menu's callback.
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, base::BindOnce(&LrbPlatformDelegate::CloseAllByUser));
      break;
  }
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
  }
  return false;
}

BEGIN_METADATA(WindowView)
END_METADATA

}  // namespace lrb
