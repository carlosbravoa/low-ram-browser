// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_UI_CONTEXT_MENU_H_
#define LRB_BROWSER_UI_CONTEXT_MENU_H_

#include <memory>

#include "base/memory/raw_ptr.h"
#include "content/public/browser/context_menu_params.h"
#include "content/public/browser/global_routing_id.h"
#include "content/public/browser/web_contents_view_delegate.h"
#include "ui/menus/simple_menu_model.h"

namespace content {
class WebContents;
}

namespace views {
class MenuRunner;
}

namespace lrb {

// The page's right-click menu. content_shell's had only "Inspect Element".
// By what was clicked:
//   a link:      Open link in new window (another site: that site's
//                window), Copy link address
//   an image:    Save image as... (the download picker), Copy image address
//   a selection: Copy, Search for "..."
//   a text field: Undo, Cut, Copy, Paste, Select all (as the page allows)
//   elsewhere:   Back, Forward, Reload
class LrbWebContentsViewDelegate : public content::WebContentsViewDelegate,
                                   public ui::SimpleMenuModel::Delegate {
 public:
  explicit LrbWebContentsViewDelegate(content::WebContents* web_contents);
  LrbWebContentsViewDelegate(const LrbWebContentsViewDelegate&) = delete;
  LrbWebContentsViewDelegate& operator=(const LrbWebContentsViewDelegate&) =
      delete;
  ~LrbWebContentsViewDelegate() override;

  // content::WebContentsViewDelegate:
  void ShowContextMenu(content::RenderFrameHost& render_frame_host,
                       const content::ContextMenuParams& params) override;
  void DismissContextMenu() override;
  bool IsContextMenuShowingForTesting() override;

  // ui::SimpleMenuModel::Delegate:
  void ExecuteCommand(int command_id, int event_flags) override;
  bool IsCommandIdEnabled(int command_id) const override;

 private:
  raw_ptr<content::WebContents> web_contents_;
  content::ContextMenuParams params_;
  content::GlobalRenderFrameHostId frame_;
  ui::SimpleMenuModel model_{this};
  std::unique_ptr<views::MenuRunner> runner_;
};

}  // namespace lrb

#endif  // LRB_BROWSER_UI_CONTEXT_MENU_H_
