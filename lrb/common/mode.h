// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_COMMON_MODE_H_
#define LRB_COMMON_MODE_H_

namespace lrb {

// How this process runs (--lrb-mode, lrb_switches.h; the coordinator
// chooses per start from the settings and the machine's memory):
//
//   lean      one single-process instance per site, each with its own
//             profile, confined as a whole (Landlock, seccomp); sites never
//             share a process or a cookie jar. For machines where memory is
//             tight.
//   standard  Chromium's usual processes: sandboxed renderers, site
//             isolation, one browser process for every site with one
//             shared profile, so signing in once (Google, Microsoft...)
//             serves every site, as in Chrome. For machines with memory to
//             spare.
//
// Read from the command line once; the same in every process of the
// instance (the browser passes it to its renderers).
bool StandardMode();

}  // namespace lrb

#endif  // LRB_COMMON_MODE_H_
