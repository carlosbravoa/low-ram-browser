// Copyright 2026 The low-ram-browser Authors
// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_LRB_BROWSER_MAIN_PARTS_H_
#define LRB_BROWSER_LRB_BROWSER_MAIN_PARTS_H_

#include <memory>

#include "base/memory/raw_ref.h"
#include "content/public/browser/browser_main_parts.h"

namespace performance_manager {
class PerformanceManagerLifetime;
}

namespace lrb {

class ListUpdater;
class LrbBrowserContext;
class LrbContentBrowserClient;

// The browser process's startup and shutdown: the profile, the windows'
// platform, the first window (or the filter-list updater), the connection to
// the coordinator. From content_shell's ShellBrowserMainParts without web
// tests and other platforms; DevTools' remote debugging server starts only
// when asked for (--remote-debugging-port/-pipe: tests, the harness), where
// content_shell always listened.
class LrbBrowserMainParts : public content::BrowserMainParts {
 public:
  explicit LrbBrowserMainParts(LrbContentBrowserClient& client);
  LrbBrowserMainParts(const LrbBrowserMainParts&) = delete;
  LrbBrowserMainParts& operator=(const LrbBrowserMainParts&) = delete;
  ~LrbBrowserMainParts() override;

  LrbBrowserContext* browser_context() { return browser_context_.get(); }
  LrbBrowserContext* off_the_record_browser_context() {
    return off_the_record_browser_context_.get();
  }

  // content::BrowserMainParts:
  void PostCreateMainMessageLoop() override;
  void ToolkitInitialized() override;
  int PostCreateThreads() override;
  int PreMainMessageLoopRun() override;
  void WillRunMainMessageLoop(
      std::unique_ptr<base::RunLoop>& run_loop) override;
  void PostMainMessageLoopRun() override;
  void PostDestroyThreads() override;

 private:
  // The first window, or the filter-list updater.
  void OpenFirstWindow();

  const raw_ref<LrbContentBrowserClient> client_;
  std::unique_ptr<LrbBrowserContext> browser_context_;
  std::unique_ptr<LrbBrowserContext> off_the_record_browser_context_;
  std::unique_ptr<performance_manager::PerformanceManagerLifetime>
      performance_manager_lifetime_;
  std::unique_ptr<ListUpdater> list_updater_;
};

}  // namespace lrb

#endif  // LRB_BROWSER_LRB_BROWSER_MAIN_PARTS_H_
