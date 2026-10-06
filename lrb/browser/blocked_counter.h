// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_BLOCKED_COUNTER_H_
#define LRB_BROWSER_BLOCKED_COUNTER_H_

#include "base/functional/callback.h"
#include "content/public/browser/global_routing_id.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"

namespace lrb {

// Requests the content blocker stopped on the page a WebContents shows, for
// the window's bar. Reset whenever a new page commits. UI thread only.
class BlockedCounter : public content::WebContentsObserver,
                       public content::WebContentsUserData<BlockedCounter> {
 public:
  // Counts one blocked request from `frame`'s page, if it still exists.
  static void OnBlocked(content::GlobalRenderFrameHostId frame);

  ~BlockedCounter() override;

  int count() const { return count_; }
  // Called with the new count whenever it changes.
  void SetOnChanged(base::RepeatingCallback<void(int)> on_changed);

 private:
  friend class content::WebContentsUserData<BlockedCounter>;
  explicit BlockedCounter(content::WebContents* web_contents);

  // content::WebContentsObserver:
  void PrimaryPageChanged(content::Page& page) override;

  void Set(int count);

  int count_ = 0;
  base::RepeatingCallback<void(int)> on_changed_;

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

}  // namespace lrb

#endif  // LRB_BROWSER_BLOCKED_COUNTER_H_
