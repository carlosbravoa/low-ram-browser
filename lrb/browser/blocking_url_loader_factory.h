// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_BLOCKING_URL_LOADER_FACTORY_H_
#define LRB_BROWSER_BLOCKING_URL_LOADER_FACTORY_H_

#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "content/public/browser/global_routing_id.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "url/gurl.h"

namespace lrb {

// Sits in front of the network for a page's or worker's requests
// (ContentBrowserClient::WillCreateURLLoaderFactory) and applies content
// blocking: a matched request is answered here, never sent: with the
// surrogate its rule names ($redirect: a no-op script, a 1x1 image), so
// pages that check for their ad scripts see them "load", or else with
// ERR_BLOCKED_BY_CLIENT. Everything else is forwarded unchanged.
//
// Deletes itself when its last client and the target are gone.
class BlockingURLLoaderFactory : public network::mojom::URLLoaderFactory {
 public:
  // `source` is the page (or worker) the requests come from; `frame` its
  // frame, if any, whose page counts what is blocked (BlockedCounter).
  static void Create(
      const GURL& source,
      content::GlobalRenderFrameHostId frame,
      mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
      mojo::PendingRemote<network::mojom::URLLoaderFactory> target);

  BlockingURLLoaderFactory(const BlockingURLLoaderFactory&) = delete;
  BlockingURLLoaderFactory& operator=(const BlockingURLLoaderFactory&) = delete;

  // network::mojom::URLLoaderFactory:
  void CreateLoaderAndStart(
      mojo::PendingReceiver<network::mojom::URLLoader> loader,
      int32_t request_id,
      uint32_t options,
      const network::ResourceRequest& request,
      mojo::PendingRemote<network::mojom::URLLoaderClient> client,
      const net::MutableNetworkTrafficAnnotationTag& traffic_annotation)
      override;
  void Clone(mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver)
      override;

 private:
  BlockingURLLoaderFactory(
      const GURL& source,
      content::GlobalRenderFrameHostId frame,
      mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
      mojo::PendingRemote<network::mojom::URLLoaderFactory> target);
  ~BlockingURLLoaderFactory() override;

  void MaybeDelete();

  const GURL source_;
  const content::GlobalRenderFrameHostId frame_;
  mojo::ReceiverSet<network::mojom::URLLoaderFactory> receivers_;
  mojo::Remote<network::mojom::URLLoaderFactory> target_;
};

}  // namespace lrb

#endif  // LRB_BROWSER_BLOCKING_URL_LOADER_FACTORY_H_
