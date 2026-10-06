// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/ui/login_prompt.h"

#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "content/public/browser/web_contents.h"
#include "lrb/browser/shell.h"
#include "lrb/browser/site.h"
#include "lrb/browser/ui/lrb_platform_delegate.h"
#include "lrb/browser/ui/window_view.h"
#include "net/base/auth.h"
#include "url/gurl.h"
#include "url/url_constants.h"

namespace lrb {

namespace {

// Lives while the request waits; destroyed by content when it's done or
// gone, which withdraws the question.
class LoginPrompt : public content::LoginDelegate {
 public:
  LoginPrompt(WindowView* view,
              const std::u16string& text,
              content::LoginDelegate::LoginAuthRequiredCallback callback)
      : view_(view->GetWeakPtr()), callback_(std::move(callback)) {
    WindowView::Question question;
    question.owner = this;
    question.text = text;
    question.login = true;
    question.accept_label = u"Sign in";
    question.cancel_label = u"Cancel";
    question.focus = WindowView::Question::Focus::kInput;
    question.credentials = base::BindOnce(&LoginPrompt::OnAnswer,
                                          weak_factory_.GetWeakPtr());
    view->Ask(std::move(question));
  }

  ~LoginPrompt() override {
    if (view_) {
      view_->DropQuestionsFrom(this);
    }
  }

 private:
  void OnAnswer(
      std::optional<std::pair<std::u16string, std::u16string>> answer) {
    if (!callback_) {
      return;
    }
    // Posted: content deletes this delegate as soon as it has the answer,
    // and the question being answered still refers to it.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(
            std::move(callback_),
            answer ? std::make_optional(
                         net::AuthCredentials(answer->first, answer->second))
                   : std::nullopt));
  }

  base::WeakPtr<WindowView> view_;
  content::LoginDelegate::LoginAuthRequiredCallback callback_;
  base::WeakPtrFactory<LoginPrompt> weak_factory_{this};
};

}  // namespace

std::unique_ptr<content::LoginDelegate> AskToSignIn(
    const net::AuthChallengeInfo& auth_info,
    content::WebContents* web_contents,
    bool is_request_for_primary_main_frame_navigation,
    const GURL& url,
    content::LoginDelegate::LoginAuthRequiredCallback callback) {
  if (auth_info.is_proxy || !web_contents) {
    return nullptr;
  }
  const GURL page = web_contents->GetLastCommittedURL();
  if (!is_request_for_primary_main_frame_navigation &&
      SiteForUrl(url) != SiteForUrl(page)) {
    return nullptr;  // another site's embedded content: never asks
  }
  Shell* shell = Shell::FromWebContents(web_contents);
  WindowView* view = shell ? LrbPlatformDelegate::ShowTabFor(shell) : nullptr;
  if (!view) {
    return nullptr;
  }
  std::u16string text = base::UTF8ToUTF16(auth_info.challenger.host()) +
                        u" asks you to sign in";
  if (auth_info.challenger.scheme() != url::kHttpsScheme) {
    text += u" (not encrypted: the password can be read on the way)";
  }
  return std::make_unique<LoginPrompt>(view, text, std::move(callback));
}

}  // namespace lrb
