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

// Asks the coordinator which plugs are connected. Without a coordinator
// there is nobody to ask: answered as outside a snap.
void AskPlugs(PlugsCallback done) {
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
}

std::string SnapName() {
  std::optional<std::string> name =
      base::Environment::Create()->GetVar("SNAP_NAME");
  return name && !name->empty() ? *name : "low-ram-browser";
}

// The window's contents: a summary of what isn't working, a block per plug
// (with the command that connects it when it isn't), and the buttons to
// copy the commands and check again.
class SnapConnectionsView : public views::View {
  METADATA_HEADER(SnapConnectionsView, views::View)

 public:
  // `max_blocks_height`: the blocks scroll beyond it (the dialog is drawn
  // inside its browser window, and cut off where that ends).
  explicit SnapConnectionsView(int max_blocks_height) {
    SetLayoutManager(std::make_unique<views::BoxLayout>(
                         views::BoxLayout::Orientation::kVertical,
                         gfx::Insets::VH(16, 20), 14))
        ->set_cross_axis_alignment(
            views::BoxLayout::CrossAxisAlignment::kStretch);

    // What isn't working right now, in plain words.
    summary_ = AddChildView(std::make_unique<views::Label>(
        u"Checking...", views::style::CONTEXT_LABEL,
        views::style::STYLE_PRIMARY));
    summary_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    summary_->SetMultiLine(true);

    not_snap_ = AddChildView(std::make_unique<views::Label>(
        u"Not running as a snap.", views::style::CONTEXT_LABEL,
        views::style::STYLE_SECONDARY));
    not_snap_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    not_snap_->SetVisible(false);

    // One block per plug: the feature and its state, the plug's name, and
    // when not connected the command that connects it.
    scroll_ = AddChildView(std::make_unique<views::ScrollView>());
    scroll_->SetHorizontalScrollBarMode(
        views::ScrollView::ScrollBarMode::kDisabled);
    // Opaque, as the dialog: text on a transparent layer draws heavier.
    scroll_->SetBackgroundColor(ui::kColorDialogBackground);
    scroll_->ClipHeightTo(0, max_blocks_height);
    views::View* blocks =
        scroll_->SetContents(std::make_unique<views::View>());
    // Room on the right for the scroll bar, drawn over the contents.
    blocks->SetLayoutManager(std::make_unique<views::BoxLayout>(
                                 views::BoxLayout::Orientation::kVertical,
                                 gfx::Insets::TLBR(0, 0, 0, 12), 12))
        ->set_cross_axis_alignment(
            views::BoxLayout::CrossAxisAlignment::kStretch);
    const gfx::FontList monospace({"monospace"}, gfx::Font::NORMAL, 13,
                                  gfx::Font::Weight::NORMAL);
    for (size_t i = 0; i < kPlugs.size(); ++i) {
      Row& row = rows_[i];
      auto* block = blocks->AddChildView(std::make_unique<views::View>());
      block->SetLayoutManager(std::make_unique<views::BoxLayout>(
                                  views::BoxLayout::Orientation::kVertical,
                                  gfx::Insets(), 2))
          ->set_cross_axis_alignment(
              views::BoxLayout::CrossAxisAlignment::kStretch);

      // The feature, and its state at the line's right end.
      auto* line = block->AddChildView(std::make_unique<views::View>());
      auto* line_layout =
          line->SetLayoutManager(std::make_unique<views::BoxLayout>(
              views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 16));
      row.feature = line->AddChildView(std::make_unique<views::Label>(
          kPlugs[i].feature, views::style::CONTEXT_LABEL,
          views::style::STYLE_PRIMARY));
      line_layout->SetFlexForView(row.feature, 1);
      row.feature->SetHorizontalAlignment(gfx::ALIGN_LEFT);
      row.state = line->AddChildView(std::make_unique<views::Label>(
          u"Checking...", views::style::CONTEXT_LABEL,
          views::style::STYLE_SECONDARY));

      auto* name = block->AddChildView(std::make_unique<views::Label>(
          base::ASCIIToUTF16(kPlugs[i].name), views::style::CONTEXT_LABEL,
          views::style::STYLE_SECONDARY));
      name->SetHorizontalAlignment(gfx::ALIGN_LEFT);

      // Read-only, to select and copy; the text in the normal colour (read-
      // only draws it greyed by default).
      row.command = block->AddChildView(std::make_unique<views::Textfield>());
      row.command->SetReadOnly(true);
      row.command->SetTextColorId(ui::kColorTextfieldForeground);
      row.command->SetFontList(monospace);
      row.command->SetText(base::UTF8ToUTF16(Command(i)));
      row.command->GetViewAccessibility().SetName(
          u"Command to connect " + base::ASCIIToUTF16(kPlugs[i].name));
      row.command->SetProperty(views::kMarginsKey,
                               gfx::Insets::TLBR(4, 16, 0, 0));
      row.command->SetVisible(false);
    }

    auto* buttons = AddChildView(std::make_unique<views::View>());
    buttons->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 8));
    copy_ = buttons->AddChildView(std::make_unique<views::MdTextButton>(
        base::BindRepeating(&SnapConnectionsView::Copy,
                            base::Unretained(this)),
        u"Copy all commands"));
    copy_->SetEnabled(false);
    buttons->AddChildView(std::make_unique<views::MdTextButton>(
        base::BindRepeating(&SnapConnectionsView::Check,
                            base::Unretained(this)),
        u"Check again"));

    Check();
  }

 private:
  struct Row {
    raw_ptr<views::Label> feature = nullptr;
    raw_ptr<views::Label> state = nullptr;
    raw_ptr<views::Textfield> command = nullptr;
  };

  static std::string Command(size_t i) {
    return "sudo snap connect " + SnapName() + ":" + kPlugs[i].name;
  }

  // Asks the coordinator again; the summary and the states read
  // "Checking..." until it answers.
  void Check() {
    weak_factory_.InvalidateWeakPtrs();  // an earlier answer is stale
    summary_->SetText(u"Checking...");
    for (Row& row : rows_) {
      row.state->SetText(u"Checking...");
      row.state->SetTextStyle(views::style::STYLE_SECONDARY);
      row.state->SetEnabledColor(ui::kColorLabelForegroundSecondary);
    }
    VLOG(1) << "lrb: snap connections: checking";
    AskPlugs(base::BindOnce(&SnapConnectionsView::Show,
                            weak_factory_.GetWeakPtr()));
  }

  void Show(Plugs plugs) {
    const bool snap = !plugs.empty();
    not_snap_->SetVisible(!snap);
    scroll_->SetVisible(snap);
    copy_->SetVisible(snap);
    summary_->SetVisible(snap);
    missing_.clear();
    if (snap) {
      for (size_t i = 0; i < kPlugs.size(); ++i) {
        // A plug the coordinator didn't name counts as not connected.
        auto it = plugs.find(kPlugs[i].name);
        const bool connected = it != plugs.end() && it->second;
        Row& row = rows_[i];
        // What is broken stands out: bold, the state in the alert colour.
        row.feature->SetTextStyle(connected ? views::style::STYLE_PRIMARY
                                            : views::style::STYLE_EMPHASIZED);
        row.state->SetText(connected ? u"Working" : u"Not working");
        row.state->SetTextStyle(connected ? views::style::STYLE_PRIMARY
                                          : views::style::STYLE_EMPHASIZED);
        row.state->SetEnabledColor(connected ? ui::kColorLabelForeground
                                             : ui::kColorAlertHighSeverity);
        row.command->SetVisible(!connected);
        VLOG(1) << "lrb: snap connection " << kPlugs[i].name << ": "
                << (connected ? "Working" : "Not working");
        if (!connected) {
          missing_.push_back(Command(i));
        }
      }
      const size_t n = missing_.size();
      summary_->SetText(
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
    } else {
      VLOG(1) << "lrb: snap connections: Not running as a snap.";
    }
    copy_->SetEnabled(!missing_.empty());
    // The blocks or the note: the window's height follows.
    if (views::Widget* widget = GetWidget()) {
      widget->SetSize(widget->non_client_view()->GetPreferredSize());
    }
  }

  // Every missing command, one per line.
  void Copy() {
    ui::ScopedClipboardWriter(ui::ClipboardBuffer::kCopyPaste)
        .WriteText(base::UTF8ToUTF16(base::JoinString(missing_, "\n")));
  }

  raw_ptr<views::Label> summary_ = nullptr;
  raw_ptr<views::Label> not_snap_ = nullptr;
  raw_ptr<views::ScrollView> scroll_ = nullptr;  // the blocks
  std::array<Row, kPlugs.size()> rows_;
  raw_ptr<views::MdTextButton> copy_ = nullptr;
  std::vector<std::string> missing_;  // the commands Copy copies
  base::WeakPtrFactory<SnapConnectionsView> weak_factory_{this};
};

BEGIN_METADATA(SnapConnectionsView)
END_METADATA

// Owns the dialog's delegate and widget (CLIENT_OWNS_WIDGET, as lrb's
// windows), and itself, until the window closes. As About's.
class SnapConnectionsWindow {
 public:
  SnapConnectionsWindow(gfx::NativeView parent, int parent_height) {
    delegate_ = std::make_unique<views::DialogDelegate>();
    // The title, summary and buttons take about 320 pixels of the window.
    delegate_->SetContentsView(std::make_unique<SnapConnectionsView>(
        std::max(parent_height - 320, 160)));
    delegate_->SetTitle(u"Snap connections");
    // A fixed width (fits a 1024-pixel screen), so the wrapped summary's
    // height counts.
    delegate_->set_fixed_width(560);
    delegate_->SetModalType(ui::mojom::ModalType::kWindow);
    delegate_->SetButtons(static_cast<int>(ui::mojom::DialogButton::kOk));
    delegate_->SetButtonLabel(ui::mojom::DialogButton::kOk, u"Close");
    views::Widget::InitParams params =
        views::DialogDelegate::GetDialogWidgetInitParams(
            delegate_.get(), gfx::NativeWindow(), parent, gfx::Rect());
    params.ownership = views::Widget::InitParams::CLIENT_OWNS_WIDGET;
    widget_ = std::make_unique<views::Widget>();
    widget_->Init(std::move(params));
    // Closing (Close, Escape, the close button) deletes all this, after the
    // event that closed it: not inside its own window's event.
    widget_->MakeCloseSynchronous(base::BindOnce(
        [](SnapConnectionsWindow* window, views::Widget::ClosedReason) {
          base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
              FROM_HERE, window);
        },
        base::Unretained(this)));
    widget_->Show();
  }

  ~SnapConnectionsWindow() {
    VLOG(1) << "lrb: snap connections: closed";
    widget_->CloseNow();  // the native window too, now (see lrb windows)
    widget_.reset();      // the widget before its delegate
  }

 private:
  std::unique_ptr<views::DialogDelegate> delegate_;
  std::unique_ptr<views::Widget> widget_;
};

}  // namespace

bool RunningAsSnap() {
  std::optional<std::string> name =
      base::Environment::Create()->GetVar("SNAP_NAME");
  return name && !name->empty();
}

void ShowSnapConnections(base::WeakPtr<views::View> parent) {
  if (!parent || !parent->GetWidget()) {
    return;
  }
  new SnapConnectionsWindow(
      parent->GetWidget()->GetNativeView(),
      parent->GetWidget()->GetWindowBoundsInScreen().height());
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
