// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/ui/snap_connections_dialog.h"

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
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/controls/button/md_text_button.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/textarea/textarea.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/layout/layout_provider.h"
#include "ui/views/layout/table_layout.h"
#include "ui/views/style/typography.h"
#include "ui/views/style/typography_provider.h"
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

// The window's contents: the plugs and their states, the commands that
// connect the missing ones, and the buttons to copy them and check again.
class SnapConnectionsView : public views::View {
  METADATA_HEADER(SnapConnectionsView, views::View)

 public:
  SnapConnectionsView() {
    SetLayoutManager(std::make_unique<views::BoxLayout>(
                         views::BoxLayout::Orientation::kVertical,
                         gfx::Insets::VH(16, 20), 10))
        ->set_cross_axis_alignment(
            views::BoxLayout::CrossAxisAlignment::kStretch);

    auto* explanation = AddChildView(std::make_unique<views::Label>(
        u"This snap can use these only once their connections are made. "
        u"Connecting takes a terminal command; nothing else changes.",
        views::style::CONTEXT_LABEL, views::style::STYLE_PRIMARY));
    explanation->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    explanation->SetMultiLine(true);

    not_snap_ = AddChildView(std::make_unique<views::Label>(
        u"Not running as a snap.", views::style::CONTEXT_LABEL,
        views::style::STYLE_SECONDARY));
    not_snap_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    not_snap_->SetVisible(false);

    // One row per plug: the feature over the plug's name, then the state.
    table_ = AddChildView(std::make_unique<views::View>());
    views::TableLayout* rows =
        table_->SetLayoutManager(std::make_unique<views::TableLayout>());
    rows->AddColumn(views::LayoutAlignment::kStretch,
                    views::LayoutAlignment::kCenter, 1.0f,
                    views::TableLayout::ColumnSize::kUsePreferred, 0, 0)
        .AddPaddingColumn(views::TableLayout::kFixedSize, 16)
        .AddColumn(views::LayoutAlignment::kStart,
                   views::LayoutAlignment::kCenter,
                   views::TableLayout::kFixedSize,
                   views::TableLayout::ColumnSize::kUsePreferred, 0, 0);
    for (size_t i = 0; i < kPlugs.size(); ++i) {
      if (i) {
        rows->AddPaddingRow(views::TableLayout::kFixedSize, 6);
      }
      rows->AddRows(1, views::TableLayout::kFixedSize);
      auto* cell = table_->AddChildView(std::make_unique<views::View>());
      cell->SetLayoutManager(std::make_unique<views::BoxLayout>(
                                 views::BoxLayout::Orientation::kVertical))
          ->set_cross_axis_alignment(
              views::BoxLayout::CrossAxisAlignment::kStretch);
      auto* feature = cell->AddChildView(std::make_unique<views::Label>(
          kPlugs[i].feature, views::style::CONTEXT_LABEL,
          views::style::STYLE_PRIMARY));
      feature->SetHorizontalAlignment(gfx::ALIGN_LEFT);
      auto* name = cell->AddChildView(std::make_unique<views::Label>(
          base::ASCIIToUTF16(kPlugs[i].name), views::style::CONTEXT_LABEL,
          views::style::STYLE_SECONDARY));
      name->SetHorizontalAlignment(gfx::ALIGN_LEFT);
      states_[i] = table_->AddChildView(std::make_unique<views::Label>(
          u"Checking...", views::style::CONTEXT_LABEL,
          views::style::STYLE_PRIMARY));
      states_[i]->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    }

    // The commands for the plugs not connected, one per line, to copy.
    commands_ = AddChildView(std::make_unique<views::Textarea>());
    commands_->SetReadOnly(true);
    // Read-only draws greyed text by default: these are to be read.
    commands_->SetTextColorId(ui::kColorTextfieldForeground);
    commands_->GetViewAccessibility().SetName(u"Commands to connect");
    const int line_height = views::TypographyProvider::Get().GetLineHeight(
        views::style::CONTEXT_TEXTFIELD, views::style::STYLE_PRIMARY);
    commands_->SetPreferredSize(gfx::Size(
        0, static_cast<int>(kPlugs.size()) * line_height +
               2 * views::LayoutProvider::Get()->GetDistanceMetric(
                       views::DISTANCE_CONTROL_VERTICAL_TEXT_PADDING)));

    auto* buttons = AddChildView(std::make_unique<views::View>());
    buttons->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 8));
    copy_ = buttons->AddChildView(std::make_unique<views::MdTextButton>(
        base::BindRepeating(&SnapConnectionsView::Copy,
                            base::Unretained(this)),
        u"Copy commands"));
    copy_->SetEnabled(false);
    buttons->AddChildView(std::make_unique<views::MdTextButton>(
        base::BindRepeating(&SnapConnectionsView::Check,
                            base::Unretained(this)),
        u"Check again"));

    Check();
  }

 private:
  // Asks the coordinator again; the rows read "Checking..." until it
  // answers.
  void Check() {
    weak_factory_.InvalidateWeakPtrs();  // an earlier answer is stale
    for (views::Label* state : states_) {
      state->SetText(u"Checking...");
    }
    VLOG(1) << "lrb: snap connections: checking";
    AskPlugs(base::BindOnce(&SnapConnectionsView::Show,
                            weak_factory_.GetWeakPtr()));
  }

  void Show(Plugs plugs) {
    const bool snap = !plugs.empty();
    not_snap_->SetVisible(!snap);
    table_->SetVisible(snap);
    commands_->SetVisible(snap);
    copy_->SetVisible(snap);
    std::vector<std::string> commands;
    if (snap) {
      const std::string snap_name = SnapName();
      for (size_t i = 0; i < kPlugs.size(); ++i) {
        // A plug the coordinator didn't name counts as not connected.
        auto it = plugs.find(kPlugs[i].name);
        const bool connected = it != plugs.end() && it->second;
        states_[i]->SetText(connected ? u"Connected" : u"Not connected");
        VLOG(1) << "lrb: snap connection " << kPlugs[i].name << ": "
                << (connected ? "Connected" : "Not connected");
        if (!connected) {
          commands.push_back("sudo snap connect " + snap_name + ":" +
                             kPlugs[i].name);
        }
      }
    } else {
      VLOG(1) << "lrb: snap connections: Not running as a snap.";
    }
    copy_->SetEnabled(snap && !commands.empty());
    commands_->SetText(commands.empty()
                           ? u"All connected."
                           : base::UTF8ToUTF16(base::JoinString(commands, "\n")));
    if (snap) {
      VLOG(1) << "lrb: snap connections commands: "
              << (commands.empty() ? "All connected."
                                   : base::JoinString(commands, "; "));
    }
    // The table or the note: the window's height follows.
    if (views::Widget* widget = GetWidget()) {
      widget->SetSize(widget->non_client_view()->GetPreferredSize());
    }
  }

  void Copy() {
    ui::ScopedClipboardWriter(ui::ClipboardBuffer::kCopyPaste)
        .WriteText(commands_->GetText());
  }

  std::array<raw_ptr<views::Label>, kPlugs.size()> states_;
  raw_ptr<views::Label> not_snap_ = nullptr;
  raw_ptr<views::View> table_ = nullptr;
  raw_ptr<views::Textarea> commands_ = nullptr;
  raw_ptr<views::MdTextButton> copy_ = nullptr;
  base::WeakPtrFactory<SnapConnectionsView> weak_factory_{this};
};

BEGIN_METADATA(SnapConnectionsView)
END_METADATA

// Owns the dialog's delegate and widget (CLIENT_OWNS_WIDGET, as lrb's
// windows), and itself, until the window closes. As About's.
class SnapConnectionsWindow {
 public:
  explicit SnapConnectionsWindow(gfx::NativeView parent) {
    delegate_ = std::make_unique<views::DialogDelegate>();
    delegate_->SetContentsView(std::make_unique<SnapConnectionsView>());
    delegate_->SetTitle(u"Snap connections");
    // A fixed width, so the wrapped explanation's height counts.
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
  new SnapConnectionsWindow(parent->GetWidget()->GetNativeView());
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
