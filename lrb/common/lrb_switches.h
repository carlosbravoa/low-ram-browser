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

// Run as the filter-list updater (ListUpdater) instead of a browser: fetch
// the lists, write the engine file at this path, exit.
inline constexpr char kUpdateLists[] = "lrb-update-lists";

// With kUpdateLists: "full" (default) or "lean" (network rules of the main
// lists only, for small machines).
inline constexpr char kAdblockSetting[] = "lrb-adblock-setting";

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

// Opt out of lrb's defaults (lrb_main_delegate.cc): Chromium's usual
// processes (for debugging), the GPU (to compare per device).
inline constexpr char kMultiProcess[] = "lrb-multi-process";
inline constexpr char kGpu[] = "lrb-gpu";

// Show only the settings window, then exit (started by the coordinator: the
// one process allowed to write the settings). "first-start": with the first
// start's question about the GPU.
inline constexpr char kSettingsWindow[] = "lrb-settings";

}  // namespace lrb::switches

#endif  // LRB_COMMON_LRB_SWITCHES_H_
