// Copyright 2026 The low-ram-browser Authors
// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_DEVTOOLS_MANAGER_DELEGATE_H_
#define LRB_BROWSER_DEVTOOLS_MANAGER_DELEGATE_H_

#include "base/memory/raw_ptr.h"
#include "content/public/browser/devtools_manager_delegate.h"

namespace content {
class BrowserContext;
}

namespace lrb {

// DevTools over the remote debugging protocol, for tests and the harness:
// no DevTools window. From content_shell's ShellDevToolsManagerDelegate
// without its frontend and its own protocol domain.
class LrbDevToolsManagerDelegate : public content::DevToolsManagerDelegate {
 public:
  // The server listens only with --remote-debugging-port (on loopback, or
  // --remote-debugging-address) or --remote-debugging-pipe. content_shell
  // always listened (an ephemeral port when none was given), letting any
  // local process drive every window.
  static void StartRemoteDebuggingIfAsked(
      content::BrowserContext* browser_context);
  static void StopRemoteDebugging();

  explicit LrbDevToolsManagerDelegate(content::BrowserContext* browser_context);
  LrbDevToolsManagerDelegate(const LrbDevToolsManagerDelegate&) = delete;
  LrbDevToolsManagerDelegate& operator=(const LrbDevToolsManagerDelegate&) =
      delete;
  ~LrbDevToolsManagerDelegate() override;

  // content::DevToolsManagerDelegate:
  content::BrowserContext* GetDefaultBrowserContext() override;
  scoped_refptr<content::DevToolsAgentHost> CreateNewTarget(
      const GURL& url,
      TargetType target_type,
      bool new_window) override;

 private:
  raw_ptr<content::BrowserContext> browser_context_;
};

}  // namespace lrb

#endif  // LRB_BROWSER_DEVTOOLS_MANAGER_DELEGATE_H_
