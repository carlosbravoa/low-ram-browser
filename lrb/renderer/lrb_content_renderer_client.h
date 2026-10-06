// Copyright 2026 The low-ram-browser Authors
// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_RENDERER_LRB_CONTENT_RENDERER_CLIENT_H_
#define LRB_RENDERER_LRB_CONTENT_RENDERER_CLIENT_H_

#include <memory>
#include <string>

#include "content/public/renderer/content_renderer_client.h"

namespace lrb {

// The renderer's side: cosmetic filtering (CosmeticFilter; network blocking
// happens in the browser, BlockingURLLoaderFactory), error pages, DNS
// prefetch hints. From content_shell's ShellContentRendererClient without
// its test services.
class LrbContentRendererClient : public content::ContentRendererClient {
 public:
  LrbContentRendererClient();
  LrbContentRendererClient(const LrbContentRendererClient&) = delete;
  LrbContentRendererClient& operator=(const LrbContentRendererClient&) = delete;
  ~LrbContentRendererClient() override;

  // content::ContentRendererClient:
  void SetUpWebAssemblyTrapHandler() override;
  void RenderFrameCreated(content::RenderFrame* render_frame) override;
  void PrepareErrorPage(content::RenderFrame* render_frame,
                        const blink::WebURLError& error,
                        const std::string& http_method,
                        content::mojom::AlternativeErrorPageOverrideInfoPtr
                            alternative_error_page_info,
                        std::string* error_html) override;
  void PrepareErrorPageForHttpStatusError(
      content::RenderFrame* render_frame,
      const blink::WebURLError& error,
      const std::string& http_method,
      int http_status,
      content::mojom::AlternativeErrorPageOverrideInfoPtr
          alternative_error_page_info,
      std::string* error_html) override;
  std::unique_ptr<blink::URLLoaderThrottleProvider>
  CreateURLLoaderThrottleProvider(
      blink::URLLoaderThrottleProviderType provider_type) override;
  std::unique_ptr<blink::WebPrescientNetworking> CreatePrescientNetworking(
      content::RenderFrame* render_frame) override;
};

}  // namespace lrb

#endif  // LRB_RENDERER_LRB_CONTENT_RENDERER_CLIENT_H_
