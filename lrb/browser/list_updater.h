// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_LIST_UPDATER_H_
#define LRB_BROWSER_LIST_UPDATER_H_

#include <memory>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"

namespace content {
class BrowserContext;
class WebContents;
}

namespace network {
class SimpleURLLoader;
class URLLoaderFactoryBuilder;
}

namespace lrb {

// `lrb --lrb-update-lists=<engine file> [--lrb-adblock-setting=lean]`: no
// window; downloads the filter lists, compiles the content-blocking engine
// and replaces the engine file atomically, then exits. lrb_coordinator runs
// it when the file is missing or old, so the coordinator itself needs no
// network code and the lists are fetched by Chromium's network stack.
//
// Also writes <engine file>.resources.json: the surrogates $redirect rules
// answer with (no-op scripts, a 1x1 image, ...), assembled from uBlock
// Origin's web_accessible_resources, and uBlock Origin's scriptlets (+js
// rules) for the full setting. Lists and resources are fetched at run
// time and never shipped with lrb (several are GPLv3).
//
// If any download fails, the existing files are kept.
class ListUpdater {
 public:
  ListUpdater(content::BrowserContext* browser_context,
              base::FilePath engine_file,
              bool lean,
              base::OnceClosure done);
  ListUpdater(const ListUpdater&) = delete;
  ListUpdater& operator=(const ListUpdater&) = delete;
  ~ListUpdater();

  void Start();

  // The lists for each setting.
  static std::vector<std::string> ListUrls(bool lean);

  // While an updater runs: catches the scriptlet page's result (a POST to
  // lrb-updater.invalid) among the requests of a URLLoaderFactory being
  // created (ContentBrowserClient::WillCreateURLLoaderFactory).
  static void MaybeInterceptRequests(network::URLLoaderFactoryBuilder& builder);

 private:
  void Fetch(size_t index);
  void OnFetched(size_t index, std::optional<std::string> body);
  void OnWritten(bool ok);
  // Then the surrogates ($redirect resources) from uBlock Origin.
  void FetchResourceMap();
  void OnResourceMap(std::optional<std::string> body);
  void FetchResource(size_t index);
  void OnResource(size_t index, std::optional<std::string> body);
  void OnSurrogatesAssembled(std::optional<std::string> json);
  // Then the scriptlets: an invisible page imports uBlock Origin's scriptlet
  // module, converts it to adblock-rust resources and POSTs the JSON, which
  // MaybeInterceptRequests catches (nothing is sent anywhere).
  void FetchScriptlets();
  void OnScriptlets(std::optional<std::string> json);
  void OnResourcesWritten(bool ok);
  void Download(const std::string& url,
                base::OnceCallback<void(std::optional<std::string>)> done);

  const raw_ptr<content::BrowserContext> browser_context_;
  const base::FilePath engine_file_;
  const bool lean_;
  base::OnceClosure done_;
  const std::vector<std::string> urls_;
  std::vector<std::string> lists_;
  std::string resource_map_;
  std::vector<std::string> resource_names_;
  std::vector<std::string> resources_;
  std::string surrogates_json_;
  std::unique_ptr<content::WebContents> scriptlet_page_;
  base::OneShotTimer scriptlet_timeout_;
  std::unique_ptr<network::SimpleURLLoader> loader_;
  base::WeakPtrFactory<ListUpdater> weak_factory_{this};
};

}  // namespace lrb

#endif  // LRB_BROWSER_LIST_UPDATER_H_
