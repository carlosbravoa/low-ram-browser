// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_COMMON_LRB_SWITCHES_H_
#define LRB_COMMON_LRB_SWITCHES_H_

namespace lrb::switches {

// The site this instance is for (registrable domain). Set by whoever opens
// the window; without it the first http(s) navigation decides.
inline constexpr char kSite[] = "lrb-site";

// Where per-site profiles live: <dir>/<site>. Default
// $XDG_DATA_HOME/lrb/sites (~/.local/share/lrb/sites).
inline constexpr char kProfilesDir[] = "lrb-profiles-dir";

// lrb_coordinator's socket. Without it the instance runs alone and launches
// other sites' windows itself.
inline constexpr char kCoordinator[] = "lrb-coordinator";

// The content-blocking engine file (an lrb container around adblock-rust's
// serialized engine). Without it, nothing is blocked.
inline constexpr char kAdblockFile[] = "lrb-adblock-file";

// The phishing and malware lists' engine file (SafetyList). Without it,
// nothing is warned about.
inline constexpr char kSafetyFile[] = "lrb-safety-file";

// Run as the filter-list updater (ListUpdater) instead of a browser: fetch
// the lists, write the engine file at this path, exit.
inline constexpr char kUpdateLists[] = "lrb-update-lists";

// With kUpdateLists: "full" (default) or "lean" (network rules of the main
// lists only, for small machines), or "safety" (the phishing and malware
// lists, for kSafetyFile).
inline constexpr char kAdblockSetting[] = "lrb-adblock-setting";

// With kUpdateLists, for tests: the lists to fetch, comma-separated, in
// place of the setting's.
inline constexpr char kListUrls[] = "lrb-list-urls";

// This instance's profile (the same switch content_shell reads).
inline constexpr char kUserDataDir[] = "user-data-dir";

// Where the first window opens, "x,y,w,h" (screen position, page size): set
// when this instance's window replaces another site's.
inline constexpr char kWindowBounds[] = "lrb-window-bounds";

// The page of another site this instance's first window was opened from:
// Back on its first page returns there.
inline constexpr char kBackUrl[] = "lrb-back-url";

// Opened by going Back: restore the window this site left (with its
// history) instead of opening a fresh page.
inline constexpr char kRestoreLeft[] = "lrb-restore-left";

// Started on the last site used (the coordinator, with no address to
// open): the site's saved windows are the start; its URL only if none.
inline constexpr char kResume[] = "lrb-resume";

// Opt out of lrb's defaults (lrb_main_delegate.cc): Chromium's usual
// processes (for debugging), the GPU (to compare per device).
inline constexpr char kMultiProcess[] = "lrb-multi-process";
inline constexpr char kGpu[] = "lrb-gpu";

// How this process runs (lrb_coordinator decides per start, lrb/common/
// mode.h): "lean" (default), one single-process instance per site, confined
// as a whole; or "standard", Chromium's usual processes and sandbox, every
// site in this one browser with one shared profile, as on machines with
// memory to spare.
inline constexpr char kMode[] = "lrb-mode";

// Print the site (registrable domain) of this URL and exit, before any of
// Chromium starts: how lrb_coordinator finds a site without a browser.
inline constexpr char kSiteOf[] = "lrb-site-of";

// Exit 0 if this process can create a user namespace (Chromium's renderer
// sandbox needs one), 1 if not; before any of Chromium starts.
inline constexpr char kSandboxCheck[] = "lrb-sandbox-check";

// This window replaces another site's: once its first window has shown
// (first paint), tell the coordinator "shown <token>", which hides the
// window that was left.
inline constexpr char kHandoff[] = "lrb-handoff";

// The newest lrb release, "0.1.0-alpha.7" (the coordinator's release check):
// the bar offers the update when it is newer than Version().
inline constexpr char kLatestRelease[] = "lrb-latest-release";

// Run as the release check instead of a browser: fetch the newest release's
// tag, write it to this file, exit.
inline constexpr char kCheckRelease[] = "lrb-check-release";

// Standard mode, renderers: sites the user turned content blocking off for,
// comma-separated (ContentBlocker::EnabledFor).
inline constexpr char kBlockingOffSites[] = "lrb-blocking-off-sites";

// Dark pages on every site, whatever each site's choice (measuring, and
// for whoever wants them everywhere).
inline constexpr char kDarkPages[] = "lrb-dark-pages";

// Show only the settings window, then exit (started by the coordinator: the
// one process allowed to write the settings).
inline constexpr char kSettingsWindow[] = "lrb-settings";

}  // namespace lrb::switches

#endif  // LRB_COMMON_LRB_SWITCHES_H_
