// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <sched.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/i18n/icu_util.h"
#include "content/public/app/content_main.h"
#include "lrb/app/lrb_main_delegate.h"
#include "lrb/browser/site.h"
#include "lrb/common/lrb_switches.h"
#include "url/gurl.h"

namespace {

// Whether argv names `--<name>` or `--<name>=...`. Looked for by hand: the
// usual start must not initialize anything ContentMain does itself.
bool HasArg(int argc, const char** argv, const char* name) {
  const std::string flag = std::string("--") + name;
  // SAFETY: main()'s argv holds argc strings.
  const auto args = UNSAFE_BUFFERS(base::span(argv, static_cast<size_t>(argc)));
  for (const char* arg : args.subspan(1u)) {
    const std::string_view view(arg);
    if (view == flag || view.starts_with(flag + "=")) {
      return true;
    }
  }
  return false;
}

// `lrb --lrb-site-of=<url>`: prints the URL's site and exits 0, or exits 1
// if it has none. lrb_coordinator asks this way: it has no URL or domain
// code of its own, and starting a browser just to ask took a second.
int PrintSiteOf(int argc, const char** argv) {
  base::AtExitManager at_exit;
  base::CommandLine::Init(argc, argv);
  // International domain names need ICU (icudtl.dat, beside the binary).
  base::i18n::InitializeICU();
  const std::string site = lrb::SiteForUrl(GURL(
      base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
          lrb::switches::kSiteOf)));
  if (site.empty()) {
    return 1;
  }
  printf("%s\n", site.c_str());
  return 0;
}

// `lrb --lrb-sandbox-check`: exits 0 if this process may create a user
// namespace with capabilities in it, which Chromium's renderer sandbox
// needs (sandbox/linux/services/credentials.cc, CanCreateProcessInNewUserNS),
// 1 if not. Ubuntu 23.10+ (kernel.apparmor_restrict_unprivileged_userns)
// lets a binary without an AppArmor profile create the namespace but gives
// it no capabilities, so writing its uid map fails: the check writes the
// maps and enters a second namespace, as Chromium does. In a child, so this
// process stays as it was.
int CheckSandbox() {
  const pid_t child = fork();
  if (child < 0) {
    return 1;
  }
  if (child == 0) {
    const uid_t uid = geteuid();
    const gid_t gid = getegid();
    if (unshare(CLONE_NEWUSER) != 0) {
      _exit(1);
    }
    auto write_proc = [](const char* path, const std::string& text) {
      const int fd = open(path, O_WRONLY | O_CLOEXEC);
      if (fd < 0) {
        return false;
      }
      const ssize_t n = write(fd, text.data(), text.size());
      close(fd);
      return n == static_cast<ssize_t>(text.size());
    };
    // As Chromium's SetGidAndUidMaps: setgroups "deny" first (needed to map
    // the gid without CAP_SETGID), then both maps.
    if (!write_proc("/proc/self/setgroups", "deny") ||
        !write_proc("/proc/self/uid_map",
                    std::to_string(uid) + " " + std::to_string(uid) + " 1") ||
        !write_proc("/proc/self/gid_map",
                    std::to_string(gid) + " " + std::to_string(gid) + " 1")) {
      _exit(1);
    }
    // A second namespace from within the first, as Chromium requires.
    _exit(unshare(CLONE_NEWUSER) == 0 ? 0 : 1);
  }
  int status = 0;
  if (waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
    return 1;
  }
  return WEXITSTATUS(status) == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, const char** argv) {
  // Questions the coordinator asks, answered before any of Chromium starts.
  if (HasArg(argc, argv, lrb::switches::kSandboxCheck)) {
    return CheckSandbox();
  }
  if (HasArg(argc, argv, lrb::switches::kSiteOf)) {
    return PrintSiteOf(argc, argv);
  }
  lrb::LrbMainDelegate delegate;
  content::ContentMainParams params(&delegate);
  params.argc = argc;
  params.argv = argv;
  return content::ContentMain(std::move(params));
}
