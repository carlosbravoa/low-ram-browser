// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/renderer/cosmetic_filter.h"

#include <string>

#include "base/logging.h"

#include "content/public/renderer/render_frame.h"
#include "lrb/common/content_blocker.h"
#include "third_party/blink/public/web/web_css_origin.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_script_source.h"
#include "url/gurl.h"

namespace lrb {

CosmeticFilter::CosmeticFilter(content::RenderFrame* render_frame)
    : content::RenderFrameObserver(render_frame) {}

CosmeticFilter::~CosmeticFilter() = default;

void CosmeticFilter::DidCreateDocumentElement() {
  blink::WebLocalFrame* frame = render_frame()->GetWebFrame();
  blink::WebDocument document = frame->GetDocument();
  const GURL url(document.Url());
  // Scriptlets first: they must patch the page's globals before its own
  // scripts run (the document element exists, no script has run yet).
  const std::string script = ContentBlocker::Scriptlets(url);
  if (!script.empty()) {
    VLOG(1) << "lrb: running " << script.size() << " bytes of scriptlets on "
            << url.host();
    frame->ExecuteScript(blink::WebScriptSource(blink::WebString::FromUtf8(script)));
  }
  const std::string css = ContentBlocker::CosmeticCss(url);
  if (!css.empty()) {
    VLOG(1) << "lrb: hiding with " << css.size() << " bytes of CSS on "
            << GURL(document.Url()).host();
    document.InsertStyleSheet(blink::WebString::FromUtf8(css), nullptr,
                              blink::WebCssOrigin::kUser);
  }
}

void CosmeticFilter::OnDestruct() {
  delete this;
}

}  // namespace lrb
