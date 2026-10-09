// Copyright 2026 The low-ram-browser Authors
// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/renderer/lrb_content_renderer_client.h"

#include <memory>

#include "base/command_line.h"
#include "base/debug/stack_trace.h"
#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/types/pass_key.h"
#include "components/network_hints/renderer/web_prescient_networking_impl.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/web_identity.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_thread.h"
#include "lrb/common/content_blocker.h"
#include "lrb/renderer/cosmetic_filter.h"
#include "net/base/net_errors.h"
#include "printing/buildflags/buildflags.h"
#include "third_party/blink/public/platform/url_loader_throttle_provider.h"
#include "third_party/blink/public/platform/web_url_error.h"
#include "third_party/blink/public/web/modules/credentialmanagement/throttle_helper.h"
#include "v8/include/v8-initialization.h"
#include "v8/include/v8-wasm-trap-handler-posix.h"

#if BUILDFLAG(ENABLE_PRINTING)
#include "components/printing/renderer/print_render_frame_helper.h"
#include "third_party/blink/public/web/web_element.h"
#endif

namespace lrb {

namespace {

// Sign-in status headers from identity providers (FedCM's Set-Login).
class UrlLoaderThrottleProvider : public blink::URLLoaderThrottleProvider {
 public:
  UrlLoaderThrottleProvider()
      : main_thread_task_runner_(
            content::RenderThread::IsMainThread()
                ? base::SequencedTaskRunner::GetCurrentDefault()
                : nullptr) {}
  UrlLoaderThrottleProvider(
      scoped_refptr<base::SequencedTaskRunner> main_thread_task_runner,
      base::PassKey<UrlLoaderThrottleProvider>)
      : main_thread_task_runner_(std::move(main_thread_task_runner)) {}

  // blink::URLLoaderThrottleProvider:
  std::unique_ptr<URLLoaderThrottleProvider> Clone() override {
    return std::make_unique<UrlLoaderThrottleProvider>(
        main_thread_task_runner_, base::PassKey<UrlLoaderThrottleProvider>());
  }
  std::vector<std::unique_ptr<blink::URLLoaderThrottle>> CreateThrottles(
      base::optional_ref<const blink::LocalFrameToken> local_frame_token,
      const network::ResourceRequest& request) override {
    std::vector<std::unique_ptr<blink::URLLoaderThrottle>> throttles;
    if (!local_frame_token.has_value()) {
      return throttles;
    }
    auto throttle = content::MaybeCreateIdentityUrlLoaderThrottle(
        base::BindRepeating(
            [](const blink::LocalFrameToken& token,
               const scoped_refptr<base::SequencedTaskRunner>
                   main_thread_task_runner,
               const std::optional<url::Origin>& initiator,
               const url::Origin& idp_origin,
               blink::mojom::IdpSigninStatus status) {
              if (content::RenderThread::IsMainThread()) {
                blink::SetIdpSigninStatus(token, idp_origin, status);
                return;
              }
              if (main_thread_task_runner) {
                main_thread_task_runner->PostTask(
                    FROM_HERE, base::BindOnce(&blink::SetIdpSigninStatus,
                                              token, idp_origin, status));
              }
            },
            local_frame_token.value(), main_thread_task_runner_),
        content::GetSetLoginHeaderInProcessParser());
    if (throttle) {
      throttles.push_back(std::move(throttle));
    }
    return throttles;
  }
  void SetOnline(bool is_online) override {}

 private:
  scoped_refptr<base::SequencedTaskRunner> main_thread_task_runner_;
};

}  // namespace

LrbContentRendererClient::LrbContentRendererClient() = default;
LrbContentRendererClient::~LrbContentRendererClient() = default;

void LrbContentRendererClient::SetUpWebAssemblyTrapHandler() {
  // WebAssembly bounds checks by guard pages: V8 handles the signal, through
  // base's stack dump handler unless in-process stack traces are off.
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          ::switches::kDisableInProcessStackTraces)) {
    v8::V8::EnableWebAssemblyTrapHandler(/*use_v8_signal_handler=*/true);
    return;
  }
  if (base::debug::SetStackDumpFirstChanceCallback(
          v8::TryHandleWebAssemblyTrapPosix)) {
    v8::V8::EnableWebAssemblyTrapHandler(/*use_v8_signal_handler=*/false);
  }
}

#if BUILDFLAG(ENABLE_PRINTING)
namespace {

// Printing to a PDF without a preview (lrb/browser/print.h).
class PrintDelegate : public printing::PrintRenderFrameHelper::Delegate {
 public:
  blink::WebElement GetPdfElement(blink::WebLocalFrame* frame) override {
    return blink::WebElement();  // no PDF viewer
  }
  bool IsPrintPreviewEnabled() override { return false; }
  bool ShouldGenerateTaggedPDF() override { return false; }
  bool OverridePrint(blink::WebLocalFrame* frame) override { return false; }
};

}  // namespace
#endif

void LrbContentRendererClient::RenderFrameCreated(
    content::RenderFrame* render_frame) {
  if (ContentBlocker::loaded()) {
    new CosmeticFilter(render_frame);  // deletes itself with the frame
  }
#if BUILDFLAG(ENABLE_PRINTING)
  // The page is printed from its main frame, its iframes with it (in this
  // one process). Not in every frame: each helper costs memory, and an
  // iframe calling window.print() is rare (it does nothing then).
  if (render_frame->IsMainFrame()) {
    new printing::PrintRenderFrameHelper(  // deletes itself with the frame
        render_frame, std::make_unique<PrintDelegate>());
  }
#endif
}

void LrbContentRendererClient::PrepareErrorPage(
    content::RenderFrame* render_frame,
    const blink::WebURLError& error,
    const std::string& http_method,
    content::mojom::AlternativeErrorPageOverrideInfoPtr
        alternative_error_page_info,
    std::string* error_html) {
  if (error_html && error_html->empty()) {
    *error_html =
        "<head><title>Error</title></head><body>Could not load the requested "
        "resource.<br/>Error code: " +
        base::NumberToString(error.reason()) +
        (error.reason() < 0 ? " (" + net::ErrorToString(error.reason()) + ")"
                            : "") +
        "</body>";
  }
}

void LrbContentRendererClient::PrepareErrorPageForHttpStatusError(
    content::RenderFrame* render_frame,
    const blink::WebURLError& error,
    const std::string& http_method,
    int http_status,
    content::mojom::AlternativeErrorPageOverrideInfoPtr
        alternative_error_page_info,
    std::string* error_html) {
  if (error_html) {
    *error_html =
        "<head><title>Error</title></head><body>Server returned HTTP status " +
        base::NumberToString(http_status) + "</body>";
  }
}

std::unique_ptr<blink::URLLoaderThrottleProvider>
LrbContentRendererClient::CreateURLLoaderThrottleProvider(
    blink::URLLoaderThrottleProviderType provider_type) {
  return std::make_unique<UrlLoaderThrottleProvider>();
}

std::unique_ptr<blink::WebPrescientNetworking>
LrbContentRendererClient::CreatePrescientNetworking(
    content::RenderFrame* render_frame) {
  return std::make_unique<network_hints::WebPrescientNetworkingImpl>(
      render_frame);
}

}  // namespace lrb
