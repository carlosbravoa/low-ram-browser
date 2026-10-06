// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_SITE_LAUNCHER_H_
#define LRB_BROWSER_SITE_LAUNCHER_H_

#include <string>

#include "base/files/file_path.h"

class GURL;

namespace lrb {

// Opens `url` in a new browser instance for `site`, with that site's
// persistent profile (<profiles dir>/<site>). Stand-in until the
// coordinator exists: it will focus an existing window for the site instead
// of always launching one.
// The persistent profile directory of `site`'s window(s): <profiles dir>/
// <site>.
base::FilePath SiteProfileDir(const std::string& site);

// `bounds`, if not empty, is where its window opens ("x,y,w,h"); `back`,
// if not empty, the page it was left from; `restore`: going Back, so
// restore the window the site left.
void LaunchSiteWindow(const std::string& site,
                      const GURL& url,
                      const std::string& bounds,
                      const std::string& back,
                      bool restore);

}  // namespace lrb

#endif  // LRB_BROWSER_SITE_LAUNCHER_H_
