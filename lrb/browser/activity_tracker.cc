// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/activity_tracker.h"

#include <utility>

#include "base/logging.h"
#include "content/public/browser/back_forward_cache.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/referrer.h"
#include "lrb/browser/lrb_content_browser_client.h"
#include "ui/base/page_transition_types.h"
#include "url/gurl.h"
#include "url/url_constants.h"

namespace lrb {

ActivityTracker::ActivityTracker(content::WebContents* web_contents,
                                 LrbContentBrowserClient& client)
    : content::WebContentsObserver(web_contents),
      content::WebContentsUserData<ActivityTracker>(*web_contents),
      client_(client) {}

ActivityTracker::~ActivityTracker() = default;

bool ActivityTracker::Discard() {
  content::NavigationController& controller = web_contents()->GetController();
  if (state_ != State::kLive || !controller.GetLastCommittedEntry() ||
      web_contents()->GetLastCommittedURL().IsAboutBlank()) {
    return false;
  }
  VLOG(1) << "lrb: discarding " << web_contents()->GetLastCommittedURL();
  left_ = false;
  // Otherwise the back-forward cache could keep the whole page alive.
  content::BackForwardCache::DisableForRenderFrameHost(
      web_contents()->GetPrimaryMainFrame(),
      content::BackForwardCache::DisabledReason(
          content::BackForwardCache::DisabledSource::kEmbedder,
          /*id=*/0, "lrb discard", /*context=*/"", "lrb discard"));
  state_ = State::kDiscarding;
  controller.LoadURL(GURL(url::kAboutBlankURL), content::Referrer(),
                     ui::PAGE_TRANSITION_AUTO_TOPLEVEL, std::string());
  return true;
}

void ActivityTracker::Restore() {
  content::NavigationController& controller = web_contents()->GetController();
  if (state_ == State::kDiscarding) {
    restore_when_discarded_ = true;
    return;
  }
  if (state_ != State::kDiscarded || !controller.CanGoToOffset(-1)) {
    return;
  }
  VLOG(1) << "lrb: restoring a discarded page";
  state_ = State::kRestoring;
  controller.GoToOffset(-1);
}

void ActivityTracker::OnUserPresent() {
  VLOG(1) << "lrb: user present, state " << static_cast<int>(state_);
  client_->ReportActive();
  Restore();
}

// Signs the user is at this window (reported to the coordinator, and they
// bring a discarded page back): any input in it, or focus or visibility
// returning after they had left it.
// Focus alone isn't trusted: page commits move it without the user, and a
// site's only window may never lose it within its own instance.
//
// Our own discard and restore navigations move focus and visibility too;
// taken as the user leaving and coming back, they made every discard undo
// itself at once (and the discarded window claim to be in use). They are
// ignored while one of those navigations runs.
bool ActivityTracker::InOwnNavigation() const {
  return state_ == State::kDiscarding || state_ == State::kRestoring;
}

void ActivityTracker::OnWebContentsFocused(content::RenderWidgetHost*) {
  if (!InOwnNavigation() && std::exchange(left_, false)) {
    OnUserPresent();
  }
}

void ActivityTracker::OnWebContentsLostFocus(content::RenderWidgetHost*) {
  if (!InOwnNavigation()) {
    left_ = true;
  }
}

void ActivityTracker::DidGetUserInteraction(const blink::WebInputEvent&) {
  OnUserPresent();
}

void ActivityTracker::OnVisibilityChanged(content::Visibility visibility) {
  if (InOwnNavigation()) {
    return;
  }
  if (visibility != content::Visibility::VISIBLE) {
    left_ = true;
  } else if (std::exchange(left_, false)) {
    OnUserPresent();
  }
}

void ActivityTracker::DidStartNavigation(content::NavigationHandle* handle) {
  if (!handle->IsInPrimaryMainFrame()) {
    return;
  }
  // (Navigations that stay in this window are reported as activity by
  // SiteWindowThrottle, which knows whether one is handed to another site.)
  if (state_ == State::kDiscarded) {
    // The user navigated elsewhere from the blank page: the page stays in
    // history as an ordinary back entry.
    state_ = State::kLive;
  }
}

void ActivityTracker::DidFinishNavigation(content::NavigationHandle* handle) {
  if (!handle->IsInPrimaryMainFrame()) {
    return;
  }
  if (state_ == State::kDiscarding) {
    state_ = handle->HasCommitted() ? State::kDiscarded : State::kLive;
    left_ = false;  // the discard's own focus churn isn't the user leaving
    if (state_ == State::kDiscarded) {
      client_->OnPageDiscarded();
    }
    if (std::exchange(restore_when_discarded_, false)) {
      Restore();
    }
  } else if (state_ == State::kRestoring) {
    state_ = State::kLive;
    if (handle->HasCommitted()) {
      // Drop the blank entry so history looks as it did before the discard.
      web_contents()->GetController().PruneForwardEntries();
    }
  }
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(ActivityTracker);

}  // namespace lrb
