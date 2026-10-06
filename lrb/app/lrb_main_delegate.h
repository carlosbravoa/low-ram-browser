// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_APP_LRB_MAIN_DELEGATE_H_
#define LRB_APP_LRB_MAIN_DELEGATE_H_

#include <memory>
#include <optional>
#include <string>
#include <variant>

#include "components/memory_system/memory_system.h"
#include "content/public/app/content_main_delegate.h"

namespace lrb {

class LrbContentBrowserClient;
class LrbContentClient;
class LrbContentRendererClient;

// content_shell's ShellMainDelegate without web tests, the crash reporter
// and non-Linux code. The GPU and utility processes use content's default
// clients (content_shell's only added test interfaces).
class LrbMainDelegate : public content::ContentMainDelegate {
 public:
  LrbMainDelegate();
  LrbMainDelegate(const LrbMainDelegate&) = delete;
  LrbMainDelegate& operator=(const LrbMainDelegate&) = delete;
  ~LrbMainDelegate() override;

  // content::ContentMainDelegate:
  std::optional<int> BasicStartupComplete() override;
  bool ShouldCreateFeatureList(InvokedIn invoked_in) override;
  bool ShouldInitializeMojo(InvokedIn invoked_in) override;
  void PreSandboxStartup() override;
  std::variant<int, content::MainFunctionParams> RunProcess(
      const std::string& process_type,
      content::MainFunctionParams main_function_params) override;
  std::optional<int> PostEarlyInitialization(InvokedIn invoked_in) override;
  content::ContentClient* CreateContentClient() override;
  content::ContentBrowserClient* CreateContentBrowserClient() override;
  content::ContentRendererClient* CreateContentRendererClient() override;

 private:
  std::unique_ptr<LrbContentBrowserClient> browser_client_;
  std::unique_ptr<LrbContentRendererClient> renderer_client_;
  std::unique_ptr<LrbContentClient> content_client_;

  memory_system::MemorySystem memory_system_;
};

}  // namespace lrb

#endif  // LRB_APP_LRB_MAIN_DELEGATE_H_
