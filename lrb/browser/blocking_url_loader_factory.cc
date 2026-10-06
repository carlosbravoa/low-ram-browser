// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/blocking_url_loader_factory.h"

#include <algorithm>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "lrb/browser/blocked_counter.h"
#include "lrb/common/content_blocker.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "net/base/data_url.h"
#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_util.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"

namespace lrb {

namespace {

// Answers `pending_client` with `data_url`'s content as if it came from the
// network (HTTP 200, the surrogate's type, readable cross-origin). Leaves
// `pending_client` untouched if the surrogate can't be served.
bool RespondWithSurrogate(
    const std::string& data_url,
    mojo::PendingRemote<network::mojom::URLLoaderClient>& pending_client) {
  std::string mime_type, charset, body;
  if (!net::DataURL::Parse(GURL(data_url), &mime_type, &charset, &body)) {
    return false;
  }
  auto head = network::mojom::URLResponseHead::New();
  head->headers = base::MakeRefCounted<net::HttpResponseHeaders>(
      net::HttpUtil::AssembleRawHeaders(
          "HTTP/1.1 200 OK\nContent-Type: " + mime_type +
          "\nAccess-Control-Allow-Origin: *\nCache-Control: no-store\n"));
  head->mime_type = mime_type;
  head->charset = charset;
  head->content_length = static_cast<int64_t>(body.size());

  mojo::ScopedDataPipeProducerHandle producer;
  mojo::ScopedDataPipeConsumerHandle consumer;
  if (mojo::CreateDataPipe(std::max<size_t>(body.size(), 1), producer,
                           consumer) != MOJO_RESULT_OK ||
      producer->WriteAllData(base::as_byte_span(body)) != MOJO_RESULT_OK) {
    return false;
  }
  mojo::Remote<network::mojom::URLLoaderClient> client(
      std::move(pending_client));
  client->OnReceiveResponse(std::move(head), std::move(consumer),
                            std::nullopt);
  network::URLLoaderCompletionStatus status(net::OK);
  status.decoded_body_length = base::ByteSize(body.size());
  status.encoded_body_length = base::ByteSize(body.size());
  client->OnComplete(status);
  return true;
}

}  // namespace

// static
void BlockingURLLoaderFactory::Create(
    const GURL& source,
    content::GlobalRenderFrameHostId frame,
    mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
    mojo::PendingRemote<network::mojom::URLLoaderFactory> target) {
  // Owns itself; see MaybeDelete().
  new BlockingURLLoaderFactory(source, frame, std::move(receiver),
                               std::move(target));
}

BlockingURLLoaderFactory::BlockingURLLoaderFactory(
    const GURL& source,
    content::GlobalRenderFrameHostId frame,
    mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
    mojo::PendingRemote<network::mojom::URLLoaderFactory> target)
    : source_(source), frame_(frame), target_(std::move(target)) {
  receivers_.Add(this, std::move(receiver));
  receivers_.set_disconnect_handler(base::BindRepeating(
      &BlockingURLLoaderFactory::MaybeDelete, base::Unretained(this)));
  target_.set_disconnect_handler(base::BindOnce(
      &BlockingURLLoaderFactory::MaybeDelete, base::Unretained(this)));
}

BlockingURLLoaderFactory::~BlockingURLLoaderFactory() = default;

void BlockingURLLoaderFactory::MaybeDelete() {
  if (receivers_.empty() || !target_.is_connected()) {
    delete this;
  }
}

void BlockingURLLoaderFactory::CreateLoaderAndStart(
    mojo::PendingReceiver<network::mojom::URLLoader> loader,
    int32_t request_id,
    uint32_t options,
    const network::ResourceRequest& request,
    mojo::PendingRemote<network::mojom::URLLoaderClient> client,
    const net::MutableNetworkTrafficAnnotationTag& traffic_annotation) {
  // The page itself (a top-level navigation) is never blocked.
  const bool top_level =
      request.destination == network::mojom::RequestDestination::kDocument;
  const GURL source = request.request_initiator
                          ? request.request_initiator->GetURL()
                          : source_;
  const ContentBlocker::Decision decision =
      top_level ? ContentBlocker::Decision()
                : ContentBlocker::Check(request.url, source,
                                        request.destination, request.method);
  if (!decision.block) {
    target_->CreateLoaderAndStart(std::move(loader), request_id, options,
                                  request, std::move(client),
                                  traffic_annotation);
    return;
  }
  BlockedCounter::OnBlocked(frame_);
  if (!decision.redirect.empty() &&
      RespondWithSurrogate(decision.redirect, client)) {
    VLOG(1) << "lrb: surrogate for " << request.url;
    return;
  }
  // No named surrogate: answer the kinds of request whose failure a page
  // notices with an empty success. A failed beacon or analytics fetch gets
  // retried and its events buffered (GitHub held ~22 MB more with its
  // telemetry failing than without blocking at all), and a failed script is
  // what anti-adblock checks look for. Nothing reaches the tracker either way.
  using D = network::mojom::RequestDestination;
  const char* empty_success = nullptr;
  if (request.destination == D::kEmpty) {  // fetch(), XHR, sendBeacon()
    empty_success = "data:text/plain,";
  } else if (request.destination == D::kScript) {
    empty_success = "data:application/javascript,";
  }
  if (empty_success && RespondWithSurrogate(empty_success, client)) {
    VLOG(1) << "lrb: empty success for " << request.url;
    return;
  }
  VLOG(1) << "lrb: blocked " << request.url;
  if (client) {
    mojo::Remote<network::mojom::URLLoaderClient>(std::move(client))
        ->OnComplete(
            network::URLLoaderCompletionStatus(net::ERR_BLOCKED_BY_CLIENT));
  }
}

void BlockingURLLoaderFactory::Clone(
    mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver) {
  receivers_.Add(this, std::move(receiver));
}

}  // namespace lrb
