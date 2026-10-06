// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_ACTIVITY_TRACKER_H_
#define LRB_BROWSER_ACTIVITY_TRACKER_H_

#include "base/memory/raw_ref.h"
#include "content/public/browser/visibility.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"

namespace blink {
class WebInputEvent;
}

namespace lrb {

class LrbContentBrowserClient;

// Per page: tells the coordinator when the user is using this instance (so
// it isn't the one discarded), and discards / restores the page.
//
// Discarding navigates the page to about:blank and restoring goes back to
// it, then drops the blank entry, so the window and its history stay as
// they were. WebContents::Discard() is not used: it keeps the page's
// JavaScript global alive in a single process (it relies on killing the
// renderer process to free memory). Measured on GitHub, official build:
// Discard() kept 77 MB, about:blank 48 MB, a blank instance 29 MB.
class ActivityTracker : public content::WebContentsObserver,
                        public content::WebContentsUserData<ActivityTracker> {
 public:
  ActivityTracker(const ActivityTracker&) = delete;
  ActivityTracker& operator=(const ActivityTracker&) = delete;
  ~ActivityTracker() override;

  // Drops the page (if not already). Returns whether it did.
  bool Discard();
  bool discarded() const { return state_ != State::kLive; }
  // Brings a discarded page back now (a sleeping tab brought to the front:
  // being shown is the user coming back).
  void RestoreNow() { Restore(); }

  // content::WebContentsObserver:
  void OnWebContentsFocused(
      content::RenderWidgetHost* render_widget_host) override;
  void OnWebContentsLostFocus(
      content::RenderWidgetHost* render_widget_host) override;
  void DidGetUserInteraction(const blink::WebInputEvent& event) override;
  void OnVisibilityChanged(content::Visibility visibility) override;
  void DidStartNavigation(content::NavigationHandle* handle) override;
  void DidFinishNavigation(content::NavigationHandle* handle) override;

 private:
  friend class content::WebContentsUserData<ActivityTracker>;
  ActivityTracker(content::WebContents* web_contents,
                  LrbContentBrowserClient& client);

  enum class State {
    kLive,
    kDiscarding,  // navigating to about:blank
    kDiscarded,   // showing about:blank in place of the page
    kRestoring,   // going back to the page
  };

  // The user is back: report it, and bring a discarded page back.
  void OnUserPresent();
  // A discard or restore navigation of ours is in progress.
  bool InOwnNavigation() const;
  void Restore();

  const raw_ref<LrbContentBrowserClient> client_;
  State state_ = State::kLive;
  // The user came back while the discard was still in progress.
  bool restore_when_discarded_ = false;
  // Focus or visibility left this window. Only then does gaining it back mean
  // the user returned: page commits (a discard's included) move focus too.
  bool left_ = false;

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

}  // namespace lrb

#endif  // LRB_BROWSER_ACTIVITY_TRACKER_H_
