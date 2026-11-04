// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/ui/snap_connections_dialog.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "base/environment.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "lrb/browser/lrb_content_browser_client.h"
#include "ui/base/clipboard/scoped_clipboard_writer.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/base/mojom/ui_base_types.mojom-shared.h"
#include "ui/color/color_id.h"
#include "ui/gfx/font_list.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/controls/button/md_text_button.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/scroll_view.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/style/typography.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/widget/widget.h"
#include "ui/views/window/dialog_delegate.h"
#include "ui/views/window/non_client_view.h"

namespace lrb {

namespace {

// The snap's plugs that snapd doesn't connect by itself, in the order shown
// (the coordinator answers in this order too: lrb/coordinator), with the
// feature each enables.
struct Plug {
  const char* name;
  const char16_t* feature;
};
constexpr std::array<Plug, 6> kPlugs = {{
    {"browser-sandbox",
     u"Standard mode: Chromium's sandbox, one sign-in for every site"},
    {"password-manager-service",
     u"Saved passwords and autofill, in the desktop's keyring"},
    {"camera", u"Camera"},
    {"audio-record", u"Microphone"},
    {"removable-media", u"Files on removable drives (/media, /run/media)"},
    {"u2f-devices", u"Security keys (WebAuthn)"},
}};

// The coordinator's answer: each plug it named and whether it is connected.
// Empty: not a snap.
using Plugs = std::map<std::string, bool, std::less<>>;
using PlugsCallback = base::OnceCallback<void(Plugs)>;

// Requests waiting for their answer, by id.
struct Pending {
  Plugs plugs;
  PlugsCallback done;
};

std::map<uint64_t, Pending>& Requests() {
  static base::NoDestructor<std::map<uint64_t, Pending>> requests;
  return *requests;
}

// How long an answer may take: without one (an older coordinator, none),
// the window opens as outside a snap.
constexpr base::TimeDelta kAnswerTimeout = base::Seconds(5);

// Asks the coordinator which plugs are connected; `done` gets the answer,
// or an empty one after kAnswerTimeout. Without a coordinator there is
// nobody to ask: answered as outside a snap.
void AskPlugs(PlugsCallback done) {
  VLOG(1) << "lrb: snap connections: checking";
  LrbContentBrowserClient* client = LrbContentBrowserClient::Get();
  if (!client || !client->has_coordinator()) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(std::move(done), Plugs()));
    return;
  }
  static uint64_t next = 0;
  const uint64_t id = ++next;
  Requests()[id].done = std::move(done);
  client->SendToCoordinator("snap-plugs " + base::NumberToString(id));
  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE, base::BindOnce([](uint64_t id) {
        auto request = Requests().find(id);
        if (request == Requests().end()) {
          return;  // answered
        }
        PlugsCallback late = std::move(request->second.done);
        Requests().erase(request);
        std::move(late).Run(Plugs());
      }, id),
      kAnswerTimeout);
}

std::string SnapName() {
  std::optional<std::string> name =
      base::Environment::Create()->GetVar("SNAP_NAME");
  return name && !name->empty() ? *name : "low-ram-browser";
}

std::string Command(size_t i) {
  return "sudo snap connect " + SnapName() + ":" + kPlugs[i].name;
}

// The window's contents, built for one answer (`plugs`; empty: not a snap)
// and never changed after: a summary of what isn't working, a block per
// plug (with the command that connects it when it isn't), and the buttons
// to copy the commands and check again. `check_again`: asks again and
// replaces the window.
class SnapConnectionsView : public views::View {
  METADATA_HEADER(SnapConnectionsView, views::View)

 public:
  SnapConnectionsView(const Plugs& plugs, base::RepeatingClosure check_again)
      : check_again_(std::move(check_again)) {
    SetLayoutManager(std::make_unique<views::BoxLayout>(
                         views::BoxLayout::Orientation::kVertical,
                         gfx::Insets::VH(16, 20), 14))
        ->set_cross_axis_alignment(
            views::BoxLayout::CrossAxisAlignment::kStretch);

    const bool snap = !plugs.empty();
    if (!snap) {
      VLOG(1) << "lrb: snap connections: Not running as a snap.";
      auto* not_snap = AddChildView(std::make_unique<views::Label>(
          u"Not running as a snap.", views::style::CONTEXT_LABEL,
          views::style::STYLE_SECONDARY));
      not_snap->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    } else {
      // What isn't working right now, in plain words (filled below).
      auto* summary = AddChildView(std::make_unique<views::Label>(
          std::u16string(), views::style::CONTEXT_LABEL,
          views::style::STYLE_PRIMARY));
      summary->SetHorizontalAlignment(gfx::ALIGN_LEFT);
      summary->SetMultiLine(true);

      // The blocks scroll when the window is too short for them all
      // (ShrinkBlocksBy).
      scroll_ = AddChildView(std::make_unique<views::ScrollView>());
      scroll_->SetHorizontalScrollBarMode(
          views::ScrollView::ScrollBarMode::kDisabled);
      // Opaque, as the dialog: text on a transparent layer draws heavier.
      scroll_->SetBackgroundColor(ui::kColorDialogBackground);
      views::View* blocks =
          scroll_->SetContents(std::make_unique<views::View>());
      // Room on the right for the scroll bar, drawn over the contents.
      blocks->SetLayoutManager(std::make_unique<views::BoxLayout>(
                                   views::BoxLayout::Orientation::kVertical,
                                   gfx::Insets::TLBR(0, 0, 0, 12), 12))
          ->set_cross_axis_alignment(
              views::BoxLayout::CrossAxisAlignment::kStretch);
      for (size_t i = 0; i < kPlugs.size(); ++i) {
        // A plug the coordinator didn't name counts as not connected.
        auto it = plugs.find(kPlugs[i].name);
        const bool connected = it != plugs.end() && it->second;
        AddBlock(blocks, i, connected);
        VLOG(1) << "lrb: snap connection " << kPlugs[i].name << ": "
                << (connected ? "Working" : "Not working");
        if (!connected) {
          missing_.push_back(Command(i));
        }
      }
      blocks_height_ = blocks->GetPreferredSize().height();
      scroll_->ClipHeightTo(0, blocks_height_);
      const size_t n = missing_.size();
      summary->SetText(
          n == 0 ? u"Everything is connected."
          : n == 1
              ? u"1 feature is not working: its snap connection is missing. "
                u"Run the command under it in a terminal, then Check again."
              : base::NumberToString16(n) +
                    u" features are not working: their snap connections are "
                    u"missing. Run the command under each in a terminal, "
                    u"then Check again.");
      VLOG(1) << "lrb: snap connections commands: "
              << (missing_.empty() ? "All connected."
                                   : base::JoinString(missing_, "; "));
    }

    auto* buttons = AddChildView(std::make_unique<views::View>());
    buttons->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 8));
    if (snap) {
      auto* copy = buttons->AddChildView(std::make_unique<views::MdTextButton>(
          base::BindRepeating(&SnapConnectionsView::Copy,
                              base::Unretained(this)),
          u"Copy all commands"));
      copy->SetEnabled(!missing_.empty());
    }
    check_ = buttons->AddChildView(std::make_unique<views::MdTextButton>(
        base::BindRepeating(&SnapConnectionsView::CheckAgain,
                            base::Unretained(this)),
        u"Check again"));
  }

 // The blocks `pixels` shorter (they scroll), down to kMinBlocksHeight so
  // the summary and the buttons always stay. False if there are none.
  bool ShrinkBlocksBy(int pixels) {
    if (!scroll_) {
      return false;
    }
    blocks_height_ = std::max(blocks_height_ - pixels, kMinBlocksHeight);
    scroll_->ClipHeightTo(0, blocks_height_);
    return true;
  }

 private:
  static constexpr int kMinBlocksHeight = 60;

  // The feature and its state, the plug's name, and when not connected the
  // command that connects it.
  static void AddBlock(views::View* blocks, size_t i, bool connected) {
    auto* block = blocks->AddChildView(std::make_unique<views::View>());
    block->SetLayoutManager(std::make_unique<views::BoxLayout>(
                                views::BoxLayout::Orientation::kVertical,
                                gfx::Insets(), 2))
        ->set_cross_axis_alignment(
            views::BoxLayout::CrossAxisAlignment::kStretch);

    // What is broken stands out: bold, and a cross at the line's right end
    // (a check when it works), the word as its tooltip and name.
    auto* line = block->AddChildView(std::make_unique<views::View>());
    auto* line_layout =
        line->SetLayoutManager(std::make_unique<views::BoxLayout>(
            views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 16));
    const int style = connected ? views::style::STYLE_PRIMARY
                                : views::style::STYLE_EMPHASIZED;
    auto* feature = line->AddChildView(std::make_unique<views::Label>(
        kPlugs[i].feature, views::style::CONTEXT_LABEL, style));
    feature->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    line_layout->SetFlexForView(feature, 1);
    const std::u16string word = connected ? u"Working" : u"Not working";
    auto* state = line->AddChildView(std::make_unique<views::Label>(
        connected ? u"\u2705" : u"\u274C", views::style::CONTEXT_LABEL,
        style));
    state->SetCustomTooltipText(word);
    state->GetViewAccessibility().SetName(word);

    auto* name = block->AddChildView(std::make_unique<views::Label>(
        base::ASCIIToUTF16(kPlugs[i].name), views::style::CONTEXT_LABEL,
        views::style::STYLE_SECONDARY));
    name->SetHorizontalAlignment(gfx::ALIGN_LEFT);

    if (connected) {
      return;
    }
    // Read-only, to select and copy; the text in the normal colour (read-
    // only draws it greyed by default).
    auto* command = block->AddChildView(std::make_unique<views::Textfield>());
    command->SetReadOnly(true);
    command->SetTextColorId(ui::kColorTextfieldForeground);
    command->SetFontList(gfx::FontList({"monospace"}, gfx::Font::NORMAL, 13,
                                       gfx::Font::Weight::NORMAL));
    command->SetText(base::UTF8ToUTF16(Command(i)));
    command->GetViewAccessibility().SetName(
        u"Command to connect " + base::ASCIIToUTF16(kPlugs[i].name));
    command->SetProperty(views::kMarginsKey, gfx::Insets::TLBR(4, 16, 0, 0));
  }

  // Once: the window is replaced when the answer arrives.
  void CheckAgain() {
    check_->SetEnabled(false);
    check_again_.Run();
  }

  // Every missing command, one per line.
  void Copy() {
    ui::ScopedClipboardWriter(ui::ClipboardBuffer::kCopyPaste)
        .WriteText(base::UTF8ToUTF16(base::JoinString(missing_, "\n")));
  }

  base::RepeatingClosure check_again_;
  raw_ptr<views::ScrollView> scroll_ = nullptr;  // the blocks (in a snap)
  int blocks_height_ = 0;
  raw_ptr<views::MdTextButton> check_ = nullptr;
  std::vector<std::string> missing_;  // the commands Copy copies
};

BEGIN_METADATA(SnapConnectionsView)
END_METADATA

// Owns the dialog's delegate and widget (CLIENT_OWNS_WIDGET, as lrb's
// windows), and itself, until the window closes. As About's, it is built
// for its contents and never resized once shown: on Wayland the part a
// window grows into afterwards may not take clicks.
class SnapConnectionsWindow {
 public:
  // A window for `plugs` over `parent`'s, unless that is gone.
  static void Open(base::WeakPtr<views::View> parent, Plugs plugs) {
    if (!parent || !parent->GetWidget()) {
      return;
    }
    new SnapConnectionsWindow(parent, plugs);
  }

  ~SnapConnectionsWindow() {
    VLOG(1) << "lrb: snap connections: closed";
    widget_->CloseNow();  // the native window too, now (see lrb windows)
    widget_.reset();      // the widget before its delegate
  }

 private:
  // Kept free around the dialog inside the browser window.
  static constexpr int kMargin = 24;

  SnapConnectionsWindow(base::WeakPtr<views::View> parent, const Plugs& plugs)
      : parent_(parent) {
    views::Widget* parent_widget = parent->GetWidget();
    // The dialog is drawn inside the browser window, and must fit in it: on
    // Wayland a child window takes no clicks beyond its parent's edge.
    const gfx::Size room =
        parent_widget->GetClientAreaBoundsInScreen().size() -
        gfx::Size(kMargin, kMargin);
    delegate_ = std::make_unique<views::DialogDelegate>();
    auto* view = delegate_->SetContentsView(
        std::make_unique<SnapConnectionsView>(
            plugs, base::BindRepeating(&SnapConnectionsWindow::CheckAgain,
                                       weak_factory_.GetWeakPtr())));
    delegate_->SetTitle(u"Snap connections");
    // A fixed width (fits a 1024-pixel screen, and the browser window), so
    // the wrapped summary's height counts.
    delegate_->set_fixed_width(std::max(std::min(560, room.width()), 360));
    delegate_->SetModalType(ui::mojom::ModalType::kWindow);
    delegate_->SetButtons(static_cast<int>(ui::mojom::DialogButton::kOk));
    delegate_->SetButtonLabel(ui::mojom::DialogButton::kOk, u"Close");
    views::Widget::InitParams params =
        views::DialogDelegate::GetDialogWidgetInitParams(
            delegate_.get(), gfx::NativeWindow(),
            parent_widget->GetNativeView(), gfx::Rect());
    params.ownership = views::Widget::InitParams::CLIENT_OWNS_WIDGET;
    widget_ = std::make_unique<views::Widget>();
    widget_->Init(std::move(params));
    // Too tall for the browser window: the blocks give up the excess (they
    // scroll), before the window is shown.
    const int excess = widget_->GetWindowBoundsInScreen().height() -
                       room.height();
    if (excess > 0 && view->ShrinkBlocksBy(excess)) {
      const gfx::Size size = widget_->non_client_view()->GetPreferredSize();
      widget_->SetSize(size);
      widget_->CenterWindow(size);
    }
    // Closing (Close, Escape, the close button) deletes all this, after the
    // event that closed it: not inside its own window's event.
    widget_->MakeCloseSynchronous(base::BindOnce(
        [](SnapConnectionsWindow* window, views::Widget::ClosedReason) {
          base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
              FROM_HERE, window);
        },
        base::Unretained(this)));
    widget_->Show();
    // Where Close is in the browser window, once laid out (for the tests,
    // which click it).
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&SnapConnectionsWindow::LogShown,
                       weak_factory_.GetWeakPtr()),
        base::Milliseconds(500));
  }

  void LogShown() {
    views::View* close = delegate_->GetOkButton();
    if (!close || !parent_ || !parent_->GetWidget()) {
      return;
    }
    gfx::Point center = close->GetLocalBounds().CenterPoint();
    views::View::ConvertPointToScreen(close, &center);
    views::View::ConvertPointFromScreen(parent_->GetWidget()->GetRootView(),
                                        &center);
    const gfx::Size window =
        parent_->GetWidget()->GetClientAreaBoundsInScreen().size();
    VLOG(1) << "lrb: snap connections: shown, Close at " << center.x() << ","
            << center.y() << " in " << window.width() << "x"
            << window.height();
  }

  // Asks again; the answer replaces this window with one built for it.
  void CheckAgain() {
    AskPlugs(base::BindOnce(&SnapConnectionsWindow::Replace,
                            weak_factory_.GetWeakPtr()));
  }

  void Replace(Plugs plugs) {
    Open(parent_, std::move(plugs));
    widget_->Close();  // deletes this, as above
  }

  base::WeakPtr<views::View> parent_;
  std::unique_ptr<views::DialogDelegate> delegate_;
  std::unique_ptr<views::Widget> widget_;
  base::WeakPtrFactory<SnapConnectionsWindow> weak_factory_{this};
};

}  // namespace

bool RunningAsSnap() {
  std::optional<std::string> name =
      base::Environment::Create()->GetVar("SNAP_NAME");
  return name && !name->empty();
}

void ShowSnapConnections(base::WeakPtr<views::View> parent) {
  // The answer first, then the window built for it (as About reads the
  // credits first): never resized once shown.
  AskPlugs(base::BindOnce(&SnapConnectionsWindow::Open, parent));
}

bool OnSnapPlugsLine(const std::string& line) {
  const std::vector<std::string_view> words = base::SplitStringPiece(
      line, " ", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  if (words.empty() ||
      (words[0] != "snap-plug" && words[0] != "snap-plugs-done")) {
    return false;
  }
  uint64_t id = 0;
  auto request = words.size() > 1 && base::StringToUint64(words[1], &id)
                     ? Requests().find(id)
                     : Requests().end();
  if (request == Requests().end()) {
    return true;  // another request's, or one already answered
  }
  if (words[0] == "snap-plug") {
    // <id> <plug> 0|1
    if (words.size() == 4) {
      request->second.plugs[std::string(words[2])] = words[3] == "1";
    }
    return true;
  }
  Pending finished = std::move(request->second);
  Requests().erase(request);
  if (finished.done) {
    std::move(finished.done).Run(std::move(finished.plugs));
  }
  return true;
}

}  // namespace lrb
