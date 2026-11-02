// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_COORDINATOR_KEYRING_BROKER_H_
#define LRB_COORDINATOR_KEYRING_BROKER_H_

#include <sys/types.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace lrb::coordinator {

// Saved passwords and autofill details for instances, in the desktop's
// keyring through lrb_keyring (keyring.cc). Out of
// every site's process: an instance gets only what it asks for, and only
// what the rules below allow.
//
//   instance -> coordinator              coordinator -> instance
//   pw-list <id> <origin>                pw-users <id> ok [<user>...]
//                                        pw-users <id> unavailable
//                                          (no keyring: don't offer to save)
//   pw-get <id> <origin> <user>          pw <id> ok <password> | pw <id> none
//   pw-save <id> <origin> <user> <pw>    pw-saved <id> 0|1
//   af-get <id>                          af <id> ok <record> | af <id> none
//   af-save <id> <record>                af-saved <id> 0|1
//
//   (from the standard instance, each pw-* line ends with " <site>": the
//   site the origin belongs to)
//
// <id> is the instance's, letters and digits; the rest percent-encoded.
// Passwords: only for the instance's own site (from the profile directory
// the coordinator gave it, never what it says) and origins on that site.
// The standard instance (lrb/common/mode.h) holds every site in one
// process tree under Chromium's site isolation, where the browser process
// is trusted as in Chrome: it names the site, and the origin must still
// belong to it.
// Autofill: one record (name, address, ...) shared by every site, so any
// site's instance can read it; the instance asks the user first, which a
// hijacked one could skip (README, docs/design.md).
class KeyringBroker {
 public:
  // Sends a line (without the newline) to the instance on `fd`.
  using Reply = std::function<void(int fd, const std::string& line)>;

  KeyringBroker(std::string helper, Reply reply);
  ~KeyringBroker();

  // A request from the instance on `fd`, whose site is `site` (from its
  // profile; empty: none, refused; "*": the standard instance, whose pw-*
  // requests name their site as their last word).
  void Handle(int fd,
              const std::string& site,
              const std::string& command,
              const std::string& args);

  // Pipes of helpers running, to poll.
  std::vector<int> fds() const;
  void OnReadable(int fd);
  // True if `pid` was a helper (its status is recorded).
  bool OnChildExit(pid_t pid, int status);
  // The instance on `fd` disconnected: answers go nowhere.
  void Forget(int fd);

 private:
  struct Job {
    int client_fd = -1;
    std::string id;
    std::string command;
    pid_t pid = -1;
    int out = -1;
    std::string output;
    bool eof = false;
    bool exited = false;
    int status = 0;
  };

  // Runs the helper with `args`, `input` on its stdin.
  void Start(Job job,
             const std::vector<std::string>& args,
             const std::string& input);
  void Finish(Job& job);
  void Answer(const Job& job, const std::string& rest);

  std::string helper_;
  Reply reply_;
  std::map<int, Job> jobs_;  // by pipe fd
};

}  // namespace lrb::coordinator

#endif  // LRB_COORDINATOR_KEYRING_BROKER_H_
