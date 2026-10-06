// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/blocked_counter.h"

#include "content/public/browser/page.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"

namespace lrb {

// static
void BlockedCounter::OnBlocked(content::GlobalRenderFrameHostId frame) {
  content::RenderFrameHost* host = content::RenderFrameHost::FromID(frame);
  if (!host || !host->GetPage().IsPrimary()) {
    return;  // gone, or a page being left (bfcache, prerender)
  }
  content::WebContents* contents =
      content::WebContents::FromRenderFrameHost(host);
  CreateForWebContents(contents);
  BlockedCounter* counter = FromWebContents(contents);
  counter->Set(counter->count_ + 1);
}

BlockedCounter::BlockedCounter(content::WebContents* web_contents)
    : content::WebContentsObserver(web_contents),
      content::WebContentsUserData<BlockedCounter>(*web_contents) {}

BlockedCounter::~BlockedCounter() = default;

void BlockedCounter::SetOnChanged(
    base::RepeatingCallback<void(int)> on_changed) {
  on_changed_ = std::move(on_changed);
}

void BlockedCounter::PrimaryPageChanged(content::Page& page) {
  Set(0);
}

void BlockedCounter::Set(int count) {
  if (count == count_) {
    return;
  }
  count_ = count;
  if (on_changed_) {
    on_changed_.Run(count_);
  }
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(BlockedCounter);

}  // namespace lrb
