// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/site_launcher.h"

#include "base/command_line.h"
#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/process/kill.h"
#include "base/process/launch.h"
#include "base/task/thread_pool.h"
#include "lrb/common/lrb_switches.h"
#include "url/gurl.h"

namespace lrb {

namespace {

base::FilePath ProfilesDir(const base::CommandLine& command_line) {
  if (command_line.HasSwitch(switches::kProfilesDir)) {
    return command_line.GetSwitchValuePath(switches::kProfilesDir);
  }
  std::unique_ptr<base::Environment> env = base::Environment::Create();
  base::FilePath data_home;
  if (std::optional<std::string> xdg = env->GetVar("XDG_DATA_HOME");
      xdg && !xdg->empty()) {
    data_home = base::FilePath(*xdg);
  } else {
    data_home = base::GetHomeDir().Append(".local/share");
  }
  return data_home.Append("lrb/sites");
}

// Switches that belong to this instance only, not to the new one.
constexpr const char* kPerInstanceSwitches[] = {
    switches::kUserDataDir,
    switches::kSite,
    switches::kWindowBounds,
    switches::kBackUrl,
    switches::kRestoreLeft,
    switches::kResume,
    "remote-debugging-port",
    "remote-debugging-pipe",
};


void Launch(const std::string& site,
            const GURL& url,
            const std::string& bounds,
            const std::string& back,
            bool restore) {
  const base::CommandLine& current = *base::CommandLine::ForCurrentProcess();
  base::CommandLine command_line(current.GetProgram());
  for (const auto& [name, value] : current.GetSwitches()) {
    bool per_instance = false;
    for (const char* skip : kPerInstanceSwitches) {
      per_instance |= name == skip;
    }
    if (!per_instance) {
      command_line.AppendSwitchNative(name, value);
    }
  }
  const base::FilePath profiles = ProfilesDir(current);
  command_line.AppendSwitchPath(switches::kUserDataDir, profiles.Append(site));
  command_line.AppendSwitchASCII(switches::kSite, site);
  if (!bounds.empty()) {
    command_line.AppendSwitchASCII(switches::kWindowBounds, bounds);
  }
  if (!back.empty()) {
    command_line.AppendSwitchASCII(switches::kBackUrl, back);
  }
  if (restore) {
    command_line.AppendSwitch(switches::kRestoreLeft);
  }
  command_line.AppendArg(url.spec());

  base::Process process = base::LaunchProcess(command_line, {});
  if (!process.IsValid()) {
    LOG(ERROR) << "lrb: could not open a window for " << site;
    return;
  }
  // Not our child to wait for: it lives on as an independent window.
  base::EnsureProcessGetsReaped(std::move(process));
}

}  // namespace

base::FilePath SiteProfileDir(const std::string& site) {
  return ProfilesDir(*base::CommandLine::ForCurrentProcess()).Append(site);
}

void LaunchSiteWindow(const std::string& site,
                      const GURL& url,
                      const std::string& bounds,
                      const std::string& back,
                      bool restore) {
  // Launching forks and execs: not on the UI thread.
  base::ThreadPool::PostTask(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(&Launch, site, url, bounds, back, restore));
}

}  // namespace lrb
