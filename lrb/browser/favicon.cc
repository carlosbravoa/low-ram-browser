// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/favicon.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <utility>

#include "base/functional/bind.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/mojom/favicon/favicon_url.mojom.h"
#include "ui/gfx/image/image_skia_rep.h"

namespace lrb {

namespace {

constexpr int kSize = 16;  // DIP
// Bigger icons are scaled down as they're decoded (in the renderer): a page's
// 512 px icon never reaches here at full size.
constexpr uint32_t kMaxBitmapSize = 64;

// How well an icon whose sizes are `sizes` fits 16 px: lower is better.
// Unknown sizes (most /favicon.ico and <link rel=icon> without sizes) come
// after an exact or near fit, before far ones.
int Fit(const std::vector<gfx::Size>& sizes) {
  if (sizes.empty()) {
    return 24;
  }
  int best = INT_MAX;
  for (const gfx::Size& size : sizes) {
    best = std::min(best, size.width() == 0 ? 24  // "any": SVG
                                            : std::abs(size.width() - 2 * kSize));
  }
  return best;
}

}  // namespace

Favicon::Favicon(content::WebContents* contents)
    : content::WebContentsObserver(contents),
      content::WebContentsUserData<Favicon>(*contents) {}

Favicon::~Favicon() = default;

void Favicon::SetOnChanged(base::RepeatingClosure on_changed) {
  on_changed_ = std::move(on_changed);
}

void Favicon::DidUpdateFaviconURL(
    content::RenderFrameHost* render_frame_host,
    const std::vector<blink::mojom::FaviconURLPtr>& candidates,
    blink::mojom::FaviconUpdateReason reason) {
  const blink::mojom::FaviconURL* best = nullptr;
  for (const auto& candidate : candidates) {
    // Touch icons (apple-touch-icon) are big; plain icons only.
    if (candidate->icon_type != blink::mojom::FaviconIconType::kFavicon ||
        !(candidate->icon_url.SchemeIsHTTPOrHTTPS() ||
          candidate->icon_url.SchemeIs("data"))) {
      continue;
    }
    if (!best || Fit(candidate->icon_sizes) < Fit(best->icon_sizes)) {
      best = candidate.get();
    }
  }
  // The same icon as the page before (a site's pages share one): kept, no
  // flicker and no fetch.
  if (!best || best->icon_url == icon_url_) {
    return;
  }
  icon_url_ = best->icon_url;
  download_id_ = web_contents()->DownloadImage(
      icon_url_, /*is_favicon=*/true, gfx::Size(2 * kSize, 2 * kSize),
      kMaxBitmapSize, /*bypass_cache=*/false,
      base::BindOnce(&Favicon::OnDownloaded, weak_factory_.GetWeakPtr()));
}

void Favicon::OnDownloaded(int id,
                           int http_status_code,
                           const GURL& image_url,
                           const std::vector<SkBitmap>& bitmaps,
                           const std::vector<gfx::Size>& sizes) {
  if (id != download_id_ || bitmaps.empty()) {
    return;  // a newer page's, or none
  }
  // The bitmap nearest 32 px (16 DIP at 2x), drawn at 16 DIP.
  const SkBitmap* nearest = &bitmaps.front();
  for (const SkBitmap& bitmap : bitmaps) {
    if (std::abs(bitmap.width() - 2 * kSize) <
        std::abs(nearest->width() - 2 * kSize)) {
      nearest = &bitmap;
    }
  }
  if (nearest->width() <= 0) {
    return;
  }
  const float scale = static_cast<float>(nearest->width()) / kSize;
  Set(gfx::ImageSkia(gfx::ImageSkiaRep(*nearest, scale)));
}

void Favicon::Set(gfx::ImageSkia icon) {
  if (icon.isNull() && icon_.isNull()) {
    return;
  }
  icon_ = std::move(icon);
  if (on_changed_) {
    on_changed_.Run();
  }
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(Favicon);

}  // namespace lrb
