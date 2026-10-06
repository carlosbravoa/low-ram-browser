// Copyright 2026 The low-ram-browser Authors
// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/devtools_manager_delegate.h"

#include <stdint.h>

#include <memory>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/devtools_socket_factory.h"
#include "content/public/common/content_switches.h"
#include "lrb/browser/shell.h"
#include "net/base/ip_address.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/log/net_log_source.h"
#include "net/socket/tcp_server_socket.h"

namespace lrb {

namespace {

constexpr int kBackLog = 10;
constexpr char kRemoteDebuggingAddress[] = "remote-debugging-address";

class TCPServerSocketFactory : public content::DevToolsSocketFactory {
 public:
  TCPServerSocketFactory(const std::string& address, uint16_t port)
      : address_(address), port_(port) {}
  TCPServerSocketFactory(const TCPServerSocketFactory&) = delete;
  TCPServerSocketFactory& operator=(const TCPServerSocketFactory&) = delete;

 private:
  // content::DevToolsSocketFactory:
  std::unique_ptr<net::ServerSocket> CreateForHttpServer() override {
    auto socket =
        std::make_unique<net::TCPServerSocket>(nullptr, net::NetLogSource());
    if (socket->ListenWithAddressAndPort(address_, port_, kBackLog) !=
        net::OK) {
      return nullptr;
    }
    return socket;
  }
  std::unique_ptr<net::ServerSocket> CreateForTethering(
      std::string* out_name) override {
    return nullptr;
  }

  std::string address_;
  uint16_t port_;
};

std::unique_ptr<content::DevToolsSocketFactory> CreateSocketFactory() {
  const base::CommandLine& command_line =
      *base::CommandLine::ForCurrentProcess();
  uint16_t port = 0;
  int value;
  if (base::StringToInt(
          command_line.GetSwitchValueASCII(::switches::kRemoteDebuggingPort),
          &value) &&
      value >= 0 && value < 65535) {
    port = static_cast<uint16_t>(value);
  } else {
    LOG(WARNING) << "Invalid remote debugging port";
  }
  std::string address = net::IPAddress::IPv4Localhost().ToString();
  if (command_line.HasSwitch(kRemoteDebuggingAddress)) {
    address = command_line.GetSwitchValueASCII(kRemoteDebuggingAddress);
    if (!net::IPAddress().AssignFromIPLiteral(address)) {
      LOG(WARNING) << "Invalid remote debugging address: " << address;
    }
  }
  return std::make_unique<TCPServerSocketFactory>(address, port);
}

}  // namespace

// static
void LrbDevToolsManagerDelegate::StartRemoteDebuggingIfAsked(
    content::BrowserContext* browser_context) {
  const base::CommandLine& command_line =
      *base::CommandLine::ForCurrentProcess();
  if (command_line.HasSwitch(::switches::kRemoteDebuggingPort)) {
    content::DevToolsAgentHost::StartRemoteDebuggingServer(
        CreateSocketFactory(), browser_context->GetPath(), base::FilePath());
  }
  if (command_line.HasSwitch(::switches::kRemoteDebuggingPipe)) {
    content::DevToolsAgentHost::StartRemoteDebuggingPipeHandler(
        base::OnceClosure());
  }
}

// static
void LrbDevToolsManagerDelegate::StopRemoteDebugging() {
  content::DevToolsAgentHost::StopRemoteDebuggingServer();
}

LrbDevToolsManagerDelegate::LrbDevToolsManagerDelegate(
    content::BrowserContext* browser_context)
    : browser_context_(browser_context) {}

LrbDevToolsManagerDelegate::~LrbDevToolsManagerDelegate() = default;

content::BrowserContext*
LrbDevToolsManagerDelegate::GetDefaultBrowserContext() {
  return browser_context_;
}

scoped_refptr<content::DevToolsAgentHost>
LrbDevToolsManagerDelegate::CreateNewTarget(const GURL& url,
                                            TargetType target_type,
                                            bool new_window) {
  Shell* shell = Shell::CreateNewWindow(browser_context_, url, nullptr,
                                        Shell::GetShellDefaultSize());
  return target_type == kTab
             ? content::DevToolsAgentHost::GetOrCreateForTab(
                   shell->web_contents())
             : content::DevToolsAgentHost::GetOrCreateFor(
                   shell->web_contents());
}

}  // namespace lrb
