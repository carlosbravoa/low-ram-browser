// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/site_window_throttle.h"

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/task/sequenced_task_runner.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"
#include "lrb/browser/shell.h"
#include "lrb/browser/lrb_content_browser_client.h"
#include "lrb/browser/saved_windows.h"
#include "lrb/browser/site.h"
#include "lrb/browser/ui/lrb_platform_delegate.h"
#include "ui/base/page_transition_types.h"

namespace lrb {

namespace {

// Closes `contents`' window after the current navigation step.
void CloseSoon(content::WebContents* contents) {
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<content::WebContents> contents) {
                       Shell* shell =
                           contents ? Shell::FromWebContents(
                                          contents.get())
                                    : nullptr;
                       if (shell) {
                         shell->Close();
                       }
                     },
                     contents->GetWeakPtr()));
}

}  // namespace

SiteWindowThrottle::SiteWindowThrottle(
    content::NavigationThrottleRegistry& registry,
    LrbContentBrowserClient& client)
    : content::NavigationThrottle(registry), client_(client) {}

SiteWindowThrottle::~SiteWindowThrottle() = default;

content::NavigationThrottle::ThrottleCheckResult
SiteWindowThrottle::WillStartRequest() {
  content::NavigationHandle* handle = navigation_handle();
  if (!handle->IsInPrimaryMainFrame()) {
    return PROCEED;  // iframes are third-party content, not navigation
  }
  const std::string target = SiteForUrl(handle->GetURL());
  if (target.empty()) {
    return PROCEED;  // about:, data:, file:, ...
  }
  const std::string& site = client_->site();
  // A browser-initiated navigation (typed URL, back/forward, reload) that
  // stays in this window means the user is here. Our discard's about:blank
  // isn't http(s) and returned above.
  const bool user_here = !handle->IsRendererInitiated();
  if (site.empty()) {
    if (client_->has_coordinator()) {
      VLOG(1) << "lrb: handing the first page, on " << target
              << ", to its site's window";
      client_->OpenSiteWindow(
          target, handle->GetURL(),
          LrbPlatformDelegate::WindowBoundsOf(handle->GetWebContents()));
      client_->CloseAllWindowsSoon();
      return CANCEL;
    }
    client_->SetSite(target);
    if (user_here) {
      client_->ReportActive();
    }
    return PROCEED;
  }
  if (target == site) {
    if (user_here) {
      client_->ReportActive();
    }
    return PROCEED;
  }

  // A new window that nothing links back to: a target=_blank link (no
  // opener by default) or a noopener window.open(). Its first navigation
  // isn't flagged as a link click, but it is one. Popups with an opener
  // (window.open sign-in and payment popups, which report back to the page)
  // stay flows.
  content::WebContents* contents = handle->GetWebContents();
  const bool new_window = contents->GetController().IsInitialNavigation();
  const bool unlinked_new_window = new_window && !contents->HasOpener();

  const ui::PageTransition transition = handle->GetPageTransition();
  const bool history_or_reload =
      (transition & ui::PAGE_TRANSITION_FORWARD_BACK) ||
      ui::PageTransitionCoreTypeIs(transition, ui::PAGE_TRANSITION_RELOAD);
  const bool clicked = handle->HasUserGesture() && !handle->IsPost();
  const bool user_initiated =
      !history_or_reload &&
      (!handle->IsRendererInitiated() ||
       (handle->WasInitiatedByLinkClick() && clicked) ||
       (unlinked_new_window && clicked));
  if (!user_initiated) {
    if (unlinked_new_window) {
      // Another site, in a window nobody asked for: no popup flow needs a
      // window without an opener. Not loaded here, not given a window.
      VLOG(1) << "lrb: blocked an unrequested window for " << target;
      CloseSoon(contents);
      return CANCEL;
    }
    VLOG(1) << "lrb: " << target << " passes through " << site
            << "'s window (part of a flow)";
    if (user_here) {
      client_->ReportActive();  // back/forward or reload within a flow
    }
    return PROCEED;
  }

  if (new_window) {
    // A new window (target=_blank, middle-click) for another site: that
    // site's window opens instead, and this one, still empty, closes.
    VLOG(1) << "lrb: " << target << " opens in its own window, not " << site
            << "'s";
    client_->OpenSiteWindow(target, handle->GetURL());
  } else {
    // Leaving the site (a typed address, a plain link): the window moves on.
    // The site's window opens in this one's place and this one closes; the
    // last window of a site takes its process with it. Keeping both would
    // double the processes, and the memory.
    VLOG(1) << "lrb: " << site << "'s window moves on to " << target;
    client_->OpenSiteWindow(target, handle->GetURL(),
                            LrbPlatformDelegate::WindowBoundsOf(contents),
                            contents->GetLastCommittedURL());
    // The site's last tab: save it, so Back from the new site restores it
    // with its history (its process ends with it).
    Shell* shell = Shell::FromWebContents(contents);
    if (shell && LrbPlatformDelegate::TabCount() == 1) {
      SaveLeftWindow(shell, site, base::BindOnce(
                                [](base::WeakPtr<content::WebContents> left) {
                                  if (left) {
                                    CloseSoon(left.get());
                                  }
                                },
                                contents->GetWeakPtr()));
      return CANCEL;
    }
  }
  CloseSoon(contents);
  return CANCEL;
}

const char* SiteWindowThrottle::GetNameForLogging() {
  return "lrb::SiteWindowThrottle";
}

}  // namespace lrb
