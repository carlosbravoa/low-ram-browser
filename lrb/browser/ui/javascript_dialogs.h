// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_UI_JAVASCRIPT_DIALOGS_H_
#define LRB_BROWSER_UI_JAVASCRIPT_DIALOGS_H_

#include "base/memory/raw_ptr.h"
#include "content/public/browser/javascript_dialog_manager.h"

namespace lrb {

class Shell;

// A page's alert(), confirm(), prompt() and "leave this page?", shown in
// its window's question row (WindowView::Ask). content_shell shows none on
// Linux: confirm() answered "no" and prompt() nothing, silently.
//
// One dialog per page at a time; while one is open, further ones are
// suppressed (the page gets the default answer at once), so a page can't
// queue up a wall of them.
class LrbJavaScriptDialogManager : public content::JavaScriptDialogManager {
 public:
  explicit LrbJavaScriptDialogManager(Shell* shell);
  LrbJavaScriptDialogManager(const LrbJavaScriptDialogManager&) = delete;
  LrbJavaScriptDialogManager& operator=(const LrbJavaScriptDialogManager&) =
      delete;
  ~LrbJavaScriptDialogManager() override;

  // content::JavaScriptDialogManager:
  void RunJavaScriptDialog(content::WebContents* web_contents,
                           content::RenderFrameHost* render_frame_host,
                           content::JavaScriptDialogType dialog_type,
                           const std::u16string& message_text,
                           const std::u16string& default_prompt_text,
                           DialogClosedCallback callback,
                           bool* did_suppress_message) override;
  void RunBeforeUnloadDialog(content::WebContents* web_contents,
                             content::RenderFrameHost* render_frame_host,
                             bool is_reload,
                             DialogClosedCallback callback) override;
  bool HandleJavaScriptDialog(content::WebContents* web_contents,
                              bool accept,
                              const std::u16string* prompt_override) override;
  void CancelDialogs(content::WebContents* web_contents,
                     bool reset_state) override;

 private:
  const raw_ptr<Shell> shell_;
};

}  // namespace lrb

#endif  // LRB_BROWSER_UI_JAVASCRIPT_DIALOGS_H_
