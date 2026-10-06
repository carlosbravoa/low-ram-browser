// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_RENDERER_COSMETIC_FILTER_H_
#define LRB_RENDERER_COSMETIC_FILTER_H_

#include "content/public/renderer/render_frame_observer.h"

namespace lrb {

// When a frame's document is created: runs the site's scriptlets in the
// main world before the page's scripts, and hides ad containers by
// inserting the site's cosmetic rules as a user stylesheet. User stylesheets don't
// appear in the page's document.styleSheets, and their !important wins over
// the page's, so sites neither see nor undo the hiding.
class CosmeticFilter : public content::RenderFrameObserver {
 public:
  explicit CosmeticFilter(content::RenderFrame* render_frame);
  CosmeticFilter(const CosmeticFilter&) = delete;
  CosmeticFilter& operator=(const CosmeticFilter&) = delete;
  ~CosmeticFilter() override;

  // content::RenderFrameObserver:
  void DidCreateDocumentElement() override;
  void OnDestruct() override;
};

}  // namespace lrb

#endif  // LRB_RENDERER_COSMETIC_FILTER_H_
