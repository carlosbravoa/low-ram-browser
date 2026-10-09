// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_FAVICON_H_
#define LRB_BROWSER_FAVICON_H_

#include <vector>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "ui/gfx/image/image_skia.h"

namespace lrb {

// A page's icon, for its tab and its window (the taskbar, Alt+Tab): fetched
// when the page says which it has (or /favicon.ico), the size closest to 16
// px kept, a few KB. A page with another icon replaces it once fetched. Not
// kept on disk: a page loaded again fetches it again (from the HTTP cache).
class Favicon : public content::WebContentsObserver,
                public content::WebContentsUserData<Favicon> {
 public:
  ~Favicon() override;

  // The icon, empty until fetched (or if the page has none).
  const gfx::ImageSkia& icon() const { return icon_; }
  // Called when the icon changes.
  void SetOnChanged(base::RepeatingClosure on_changed);

 private:
  friend class content::WebContentsUserData<Favicon>;
  explicit Favicon(content::WebContents* contents);

  void OnDownloaded(int id,
                    int http_status_code,
                    const GURL& image_url,
                    const std::vector<SkBitmap>& bitmaps,
                    const std::vector<gfx::Size>& sizes);
  void Set(gfx::ImageSkia icon);

  // content::WebContentsObserver:
  void DidUpdateFaviconURL(
      content::RenderFrameHost* render_frame_host,
      const std::vector<blink::mojom::FaviconURLPtr>& candidates,
      blink::mojom::FaviconUpdateReason reason) override;

  gfx::ImageSkia icon_;
  GURL icon_url_;
  int download_id_ = 0;
  base::RepeatingClosure on_changed_;
  base::WeakPtrFactory<Favicon> weak_factory_{this};

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

}  // namespace lrb

#endif  // LRB_BROWSER_FAVICON_H_
