// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_ZOOM_LEVELS_H_
#define LRB_BROWSER_ZOOM_LEVELS_H_

#include <optional>
#include <string>

#include "base/callback_list.h"
#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "content/public/browser/host_zoom_map.h"

namespace content {
class BrowserContext;
}

namespace lrb {

// Zoom levels the user set, per host, kept in the site's own profile
// (lrb-zoom.json) so a site opens at its zoom next time. content_shell
// keeps none (Chrome keeps them in its preferences).
class ZoomLevels {
 public:
  ZoomLevels(content::BrowserContext* context, const base::FilePath& file);
  ZoomLevels(const ZoomLevels&) = delete;
  ZoomLevels& operator=(const ZoomLevels&) = delete;
  ~ZoomLevels();

 private:
  void OnLoaded(std::optional<std::string> json);
  void OnChanged(const content::HostZoomMap::ZoomLevelChange& change);

  const raw_ptr<content::HostZoomMap> map_;
  const base::FilePath file_;
  const scoped_refptr<base::SequencedTaskRunner> file_task_runner_;
  bool loading_ = false;
  base::CallbackListSubscription subscription_;
  base::WeakPtrFactory<ZoomLevels> weak_factory_{this};
};

}  // namespace lrb

#endif  // LRB_BROWSER_ZOOM_LEVELS_H_
