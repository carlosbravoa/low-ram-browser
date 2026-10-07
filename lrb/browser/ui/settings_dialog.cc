// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/ui/settings_dialog.h"

#include <memory>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "lrb/common/settings.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/base/mojom/ui_base_types.mojom-shared.h"
#include "ui/base/models/simple_combobox_model.h"
#include "ui/color/color_id.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/controls/button/radio_button.h"
#include "ui/views/controls/combobox/combobox.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/style/typography.h"
#include "ui/views/widget/widget.h"
#include "ui/views/window/dialog_delegate.h"

namespace lrb {

namespace {

// The window's contents.
class SettingsView : public views::View {
  METADATA_HEADER(SettingsView, views::View)

 public:
  SettingsView(const Settings& settings, bool first_start)
      : settings_(settings), first_start_(first_start) {
    SetLayoutManager(std::make_unique<views::BoxLayout>(
                         views::BoxLayout::Orientation::kVertical,
                         gfx::Insets::VH(16, 20), 8))
        ->set_cross_axis_alignment(
            views::BoxLayout::CrossAxisAlignment::kStretch);
    if (first_start) {
      // The first start on a machine with a GPU (decided 2026-10-05): show
      // the trade-off and ask; changeable later here.
      Heading(u"Welcome to lrb");
      Note(u"This computer has a graphics chip (GPU). lrb can draw pages "
           u"with it, or without it in software. The GPU costs memory in "
           u"every window: measured 7 to 55 MB more per window on a "
           u"Raspberry Pi 3, about 12 MB on a desktop with NVIDIA graphics. "
           u"How much smoother it makes scrolling and video hasn't been "
           u"measured yet. Choose one below; you can change it later in "
           u"Settings.");
    }

    Heading(u"Search engine");
    std::vector<ui::SimpleComboboxModel::Item> items;
    size_t selected = 0;
    bool preset = false;
    for (size_t i = 0; i < SearchEngines().size(); ++i) {
      items.emplace_back(std::u16string(SearchEngines()[i].name));
      if (settings.search_url == SearchEngines()[i].url) {
        selected = i;
        preset = true;
      }
    }
    items.emplace_back(u"Other...");
    custom_index_ = items.size() - 1;
    model_ = std::make_unique<ui::SimpleComboboxModel>(std::move(items));
    engine_ = AddChildView(std::make_unique<views::Combobox>(model_.get()));
    engine_->GetViewAccessibility().SetName(u"Search engine");
    engine_->SetSelectedIndex(preset ? selected : custom_index_);
    engine_->SetCallback(base::BindRepeating(&SettingsView::UpdateCustom,
                                             base::Unretained(this)));
    custom_ = AddChildView(std::make_unique<views::Textfield>());
    custom_->GetViewAccessibility().SetName(u"Search address");
    custom_->SetPlaceholderText(u"https://example.com/search?q=%s");
    custom_->SetText(preset ? u"" : base::UTF8ToUTF16(settings.search_url));
    custom_hint_ = Note(u"The address of a search, with %s where the words "
                        u"go.");
    UpdateCustom();

    Heading(u"Rendering");
    software_ = AddChildView(std::make_unique<views::RadioButton>(
        u"Use less memory (software rendering)", /*group_id=*/1));
    gpu_ = AddChildView(std::make_unique<views::RadioButton>(
        u"Use the GPU: smoother scrolling and video, more memory per window",
        /*group_id=*/1));
    // The first start asks without suggesting an answer: neither is
    // chosen until the user picks one (memory is measured, smoothness not).
    if (!first_start) {
      (settings.gpu.value_or(false) ? gpu_ : software_)->SetChecked(true);
    }
    Note(u"Without the GPU, pages can't use WebGL (3-D graphics: some maps, "
         u"games and visualizations). Applies to windows opened from now "
         u"on.");
  }

  void Heading(const std::u16string& text) {
    auto* label = AddChildView(std::make_unique<views::Label>(
        text, views::style::CONTEXT_LABEL, views::style::STYLE_EMPHASIZED));
    label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  }

  views::Label* Note(const std::u16string& text) {
    auto* label = AddChildView(std::make_unique<views::Label>(
        text, views::style::CONTEXT_LABEL, views::style::STYLE_SECONDARY));
    label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    label->SetMultiLine(true);
    return label;
  }

  views::View* first_setting() { return engine_; }

  // Returns false (the window stays open) for an unusable custom address.
  bool Save() {
    Settings settings = settings_;
    const size_t index = engine_->GetSelectedIndex().value_or(0);
    if (index == custom_index_) {
      const std::string url = base::UTF16ToUTF8(custom_->GetText());
      if (!IsValidSearchUrl(url)) {
        custom_->SetInvalid(true);
        custom_->RequestFocus();
        return false;
      }
      settings.search_url = url;
    } else {
      settings.search_url = SearchEngines()[index].url;
    }
    if (first_start_ && !software_->GetChecked() && !gpu_->GetChecked()) {
      software_->RequestFocus();  // the question needs an answer
      return false;
    }
    // Rendering left as it was, never chosen: still unset (the first-start
    // question asks then). Answering that question sets it either way.
    if (first_start_ || gpu_->GetChecked() || settings_.gpu) {
      settings.gpu = gpu_->GetChecked();
    }
    base::ThreadPool::PostTask(
        FROM_HERE,
        {base::MayBlock(), base::TaskShutdownBehavior::BLOCK_SHUTDOWN},
        base::BindOnce(base::IgnoreResult(&Settings::Write), settings));
    return true;
  }

 private:
  void UpdateCustom() {
    const bool custom = engine_->GetSelectedIndex() == custom_index_;
    custom_->SetVisible(custom);
    custom_hint_->SetVisible(custom);
    if (GetWidget()) {
      GetWidget()->SetSize(GetWidget()->non_client_view()->GetPreferredSize());
    }
  }

  const Settings settings_;
  const bool first_start_;
  std::unique_ptr<ui::SimpleComboboxModel> model_;
  size_t custom_index_ = 0;
  raw_ptr<views::Combobox> engine_ = nullptr;
  raw_ptr<views::Textfield> custom_ = nullptr;
  raw_ptr<views::Label> custom_hint_ = nullptr;
  raw_ptr<views::RadioButton> software_ = nullptr;
  raw_ptr<views::RadioButton> gpu_ = nullptr;
};

BEGIN_METADATA(SettingsView)
END_METADATA

// Owns the dialog's delegate and widget (CLIENT_OWNS_WIDGET, as lrb's
// windows), and itself, until the window closes.
class SettingsWindow {
 public:
  SettingsWindow(const Settings& settings,
                 gfx::NativeView parent,
                 bool first_start = false,
                 base::OnceClosure closed = base::OnceClosure())
      : closed_(std::move(closed)) {
    delegate_ = std::make_unique<views::DialogDelegate>();
    auto* view = delegate_->SetContentsView(
        std::make_unique<SettingsView>(settings, first_start));
    delegate_->SetTitle(first_start ? u"lrb" : u"Settings");
    // Alone (no parent): a window of its own, not modal to anything.
    if (parent) {
      delegate_->SetModalType(ui::mojom::ModalType::kWindow);
    }
    delegate_->SetButtonLabel(ui::mojom::DialogButton::kOk, u"Save");
    delegate_->SetAcceptCallbackWithClose(base::BindRepeating(
        &SettingsView::Save, base::Unretained(view)));
    // Start on the first setting; Enter still saves.
    delegate_->SetInitiallyFocusedView(view->first_setting());
    views::Widget::InitParams params =
        views::DialogDelegate::GetDialogWidgetInitParams(
            delegate_.get(), gfx::NativeWindow(), parent, gfx::Rect());
    params.ownership = views::Widget::InitParams::CLIENT_OWNS_WIDGET;
    widget_ = std::make_unique<views::Widget>();
    widget_->Init(std::move(params));
    // Closing (Save, Cancel, the close button) deletes all this, after the
    // event that closed it: not inside its own window's event.
    widget_->MakeCloseSynchronous(base::BindOnce(
        [](SettingsWindow* window, views::Widget::ClosedReason) {
          base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
              FROM_HERE, window);
        },
        base::Unretained(this)));
    widget_->Show();
  }

  ~SettingsWindow() {
    widget_->CloseNow();  // the native window too, now (see lrb windows)
    widget_.reset();      // the widget before its delegate
    if (closed_) {
      std::move(closed_).Run();
    }
  }

 private:
  base::OnceClosure closed_;
  std::unique_ptr<views::DialogDelegate> delegate_;
  std::unique_ptr<views::Widget> widget_;
};

}  // namespace

void ShowSettings(base::WeakPtr<views::View> parent) {
  // The file first (off the UI thread), then the window.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce(&Settings::Read),
      base::BindOnce(
          [](base::WeakPtr<views::View> parent, Settings settings) {
            if (!parent || !parent->GetWidget()) {
              return;  // its window closed meanwhile
            }
            new SettingsWindow(settings,
                               parent->GetWidget()->GetNativeView());
          },
          parent));
}

void ShowSettingsAlone(bool first_start, base::OnceClosure closed) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce(&Settings::Read),
      base::BindOnce(
          [](bool first_start, base::OnceClosure closed, Settings settings) {
            new SettingsWindow(settings, gfx::NativeView(), first_start,
                               std::move(closed));
          },
          first_start, std::move(closed)));
}

}  // namespace lrb
