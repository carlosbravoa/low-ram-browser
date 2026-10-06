// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/ui/javascript_dialogs.h"

#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/common/javascript_dialog_type.h"
#include "lrb/browser/shell.h"
#include "lrb/browser/ui/lrb_platform_delegate.h"
#include "lrb/browser/ui/window_view.h"
#include "url/origin.h"

namespace lrb {

namespace {

// "example.com says", or for a frame of another origin, "An embedded page at
// ads.example says": the user should know who is talking.
std::u16string Speaker(content::RenderFrameHost* frame) {
  const url::Origin origin = frame->GetLastCommittedOrigin();
  const std::u16string host = base::UTF8ToUTF16(
      origin.opaque() ? std::string("this page") : origin.host());
  if (origin == frame->GetMainFrame()->GetLastCommittedOrigin()) {
    return host + u" says";
  }
  return u"An embedded page at " + host + u" says";
}

// The page's callback from the row's answer; unanswered is "cancel".
base::OnceCallback<void(std::optional<bool>, const std::u16string&)> Reply(
    content::JavaScriptDialogManager::DialogClosedCallback callback) {
  return base::BindOnce(
      [](content::JavaScriptDialogManager::DialogClosedCallback callback,
         std::optional<bool> accepted, const std::u16string& input) {
        std::move(callback).Run(accepted.value_or(false), input);
      },
      std::move(callback));
}

}  // namespace

LrbJavaScriptDialogManager::LrbJavaScriptDialogManager(Shell* shell)
    : shell_(shell) {}

LrbJavaScriptDialogManager::~LrbJavaScriptDialogManager() = default;

void LrbJavaScriptDialogManager::RunJavaScriptDialog(
    content::WebContents* web_contents,
    content::RenderFrameHost* render_frame_host,
    content::JavaScriptDialogType dialog_type,
    const std::u16string& message_text,
    const std::u16string& default_prompt_text,
    DialogClosedCallback callback,
    bool* did_suppress_message) {
  WindowView* view = LrbPlatformDelegate::ShowTabFor(shell_);
  if (!view || view->HasQuestionsFrom(this)) {
    // No window, or one dialog open already: the default answer at once.
    *did_suppress_message = true;
    return;
  }
  *did_suppress_message = false;
  WindowView::Question question;
  question.owner = this;
  question.text = Speaker(render_frame_host) + u":\n" + message_text;
  question.accept_label = u"OK";
  switch (dialog_type) {
    case content::JAVASCRIPT_DIALOG_TYPE_ALERT:
      question.focus = WindowView::Question::Focus::kAccept;
      break;
    case content::JAVASCRIPT_DIALOG_TYPE_CONFIRM:
      question.cancel_label = u"Cancel";
      question.focus = WindowView::Question::Focus::kAccept;
      break;
    case content::JAVASCRIPT_DIALOG_TYPE_PROMPT:
      question.cancel_label = u"Cancel";
      question.input = true;
      question.default_input = default_prompt_text;
      question.focus = WindowView::Question::Focus::kInput;
      break;
  }
  question.answer = Reply(std::move(callback));
  view->Ask(std::move(question));
}

void LrbJavaScriptDialogManager::RunBeforeUnloadDialog(
    content::WebContents* web_contents,
    content::RenderFrameHost* render_frame_host,
    bool is_reload,
    DialogClosedCallback callback) {
  WindowView* view = LrbPlatformDelegate::ShowTabFor(shell_);
  if (!view) {
    std::move(callback).Run(true, std::u16string());  // nowhere to ask: leave
    return;
  }
  WindowView::Question question;
  question.owner = this;
  // Pages can't set the text any more (browsers ignore it): ours.
  question.text = is_reload ? u"Reload this page? Changes you made may not be "
                              u"saved."
                            : u"Leave this page? Changes you made may not be "
                              u"saved.";
  question.accept_label = is_reload ? u"Reload" : u"Leave";
  question.cancel_label = u"Stay";
  // Enter stays: losing someone's typing by accident is worse.
  question.focus = WindowView::Question::Focus::kCancel;
  question.answer = Reply(std::move(callback));
  view->Ask(std::move(question));
}

bool LrbJavaScriptDialogManager::HandleJavaScriptDialog(
    content::WebContents* web_contents,
    bool accept,
    const std::u16string* prompt_override) {
  // DevTools (Page.handleJavaScriptDialog): answer the open dialog.
  WindowView* view = LrbPlatformDelegate::ViewFor(shell_);
  return view && view->AnswerFrom(this, accept, prompt_override);
}

void LrbJavaScriptDialogManager::CancelDialogs(
    content::WebContents* web_contents,
    bool reset_state) {
  if (WindowView* view = LrbPlatformDelegate::ViewFor(shell_)) {
    view->DropQuestionsFrom(this);
  }
}

}  // namespace lrb
