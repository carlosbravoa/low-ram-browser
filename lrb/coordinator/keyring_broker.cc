// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/coordinator/keyring_broker.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <iterator>
#include <string_view>
#include <utility>

#include "lrb/coordinator/broker.h"
#include "lrb/coordinator/rules.h"

namespace lrb::coordinator {

namespace {

constexpr size_t kMaxId = 32;
// Lines are at most 9000 bytes each way (kMaxLine), and values travel
// percent-encoded (up to 3 bytes each; the record's own encoding is
// encoded again): these keep every answer under it. Also well under a
// pipe's buffer: see Start.
constexpr size_t kMaxUser = 256;
constexpr size_t kMaxPassword = 1024;
constexpr size_t kMaxRecord = 2400;

// lrb_keyring's exit statuses (keyring.cc).
constexpr int kDone = 0;
constexpr int kNoKeyring = 2;

bool ValidId(const std::string& id) {
  if (id.empty() || id.size() > kMaxId) {
    return false;
  }
  for (char c : id) {
    if (!isalnum(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  return true;
}

std::vector<std::string> Words(const std::string& text) {
  std::vector<std::string> words;
  size_t start = 0;
  while (start < text.size()) {
    const size_t end = std::min(text.find(' ', start), text.size());
    if (end > start) {
      words.push_back(text.substr(start, end - start));
    }
    start = end + 1;
  }
  return words;
}

bool HasNul(const std::string& text) {
  return text.find('\0') != std::string::npos;
}

// An origin of the instance's site: "http(s)://host[:port]", nothing
// after it.
bool OriginOfSite(const std::string& origin, const std::string& site) {
  const size_t scheme_end = origin.find("://");
  return IsAcceptableUrl(origin) && UrlBelongsToSite(origin, site) &&
         scheme_end != std::string::npos &&
         origin.find_first_of("/?#@", scheme_end + 3) == std::string::npos;
}

}  // namespace

KeyringBroker::KeyringBroker(std::string helper, Reply reply)
    : helper_(std::move(helper)), reply_(std::move(reply)) {}

KeyringBroker::~KeyringBroker() {
  for (auto& [fd, job] : jobs_) {
    close(fd);
  }
}

void KeyringBroker::Handle(int fd,
                           const std::string& site,
                           const std::string& command,
                           const std::string& args) {
  const std::vector<std::string> words = Words(args);
  if (words.empty() || !ValidId(words[0])) {
    return;
  }
  Job job;
  job.client_fd = fd;
  job.id = words[0];
  job.command = command;
  auto refuse = [&]() {
    Answer(job, command == "pw-list" ? "unavailable"
                : command == "pw-get" || command == "af-get" ? "none"
                                                            : "0");
  };
  std::vector<std::string> decoded;
  for (size_t i = 1; i < words.size(); ++i) {
    decoded.push_back(FileBroker::Decode(words[i]));
    if (HasNul(decoded.back())) {
      refuse();
      return;
    }
  }
  std::string own_site = site;
  if (site == "*") {
    // The standard instance (every site in one profile, Chromium's site
    // isolation): password requests name their site last, and the checks
    // below hold the origin to it. Autofill needs no site.
    if (command.starts_with("pw-")) {
      if (decoded.empty()) {
        refuse();
        return;
      }
      own_site = decoded.back();
      decoded.pop_back();
    }
  } else if (site.empty() || !IsValidSite(site)) {
    refuse();  // not a site's instance: the resolver, or unknown
    return;
  }
  if (command.starts_with("pw-") && !IsValidSite(own_site)) {
    refuse();
    return;
  }
  if (command == "pw-list" && decoded.size() == 1 &&
      OriginOfSite(decoded[0], own_site)) {
    Start(std::move(job),
          {"list", "password", "username", "site=" + own_site,
           "origin=" + decoded[0]},
          "");
  } else if (command == "pw-get" && decoded.size() == 2 &&
             OriginOfSite(decoded[0], own_site) && !decoded[1].empty() &&
             decoded[1].size() <= kMaxUser) {
    Start(std::move(job),
          {"get", "password", "site=" + own_site, "origin=" + decoded[0],
           "username=" + decoded[1]},
          "");
  } else if (command == "pw-save" && decoded.size() == 3 &&
             OriginOfSite(decoded[0], own_site) && !decoded[1].empty() &&
             decoded[1].size() <= kMaxUser && !decoded[2].empty() &&
             decoded[2].size() <= kMaxPassword) {
    Start(std::move(job),
          {"store", "password", "site=" + own_site, "origin=" + decoded[0],
           "username=" + decoded[1]},
          decoded[2]);
  } else if (command == "af-get" && decoded.empty()) {
    Start(std::move(job), {"get", "autofill", "profile=default"}, "");
  } else if (command == "af-save" && decoded.size() == 1 &&
             !decoded[0].empty() && decoded[0].size() <= kMaxRecord) {
    Start(std::move(job), {"store", "autofill", "profile=default"},
          decoded[0]);
  } else {
    refuse();
  }
}

void KeyringBroker::Start(Job job,
                          const std::vector<std::string>& args,
                          const std::string& input) {
  int out[2];
  int in[2];
  if (pipe2(out, O_CLOEXEC) != 0) {
    job.exited = job.eof = true;
    job.status = kNoKeyring << 8;
    Finish(job);
    return;
  }
  if (pipe2(in, O_CLOEXEC) != 0) {
    close(out[0]);
    close(out[1]);
    job.exited = job.eof = true;
    job.status = kNoKeyring << 8;
    Finish(job);
    return;
  }
  // The secret fits the pipe's buffer (64 KiB; kMaxRecord is far less), so
  // it's written before the helper runs and never blocks.
  for (std::string_view rest = input; !rest.empty();) {
    const ssize_t n = write(in[1], rest.data(), rest.size());
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      break;
    }
    rest.remove_prefix(static_cast<size_t>(n));
  }
  close(in[1]);
  job.pid = fork();
  if (job.pid == 0) {
    dup2(in[0], STDIN_FILENO);
    dup2(out[1], STDOUT_FILENO);
    std::vector<std::string> argv_strings = {helper_};
    argv_strings.insert(argv_strings.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& arg : argv_strings) {
      argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    execv(argv[0], argv.data());
    _exit(kNoKeyring);  // no helper: as without a keyring
  }
  close(in[0]);
  close(out[1]);
  if (job.pid < 0) {
    close(out[0]);
    job.exited = job.eof = true;
    job.status = kNoKeyring << 8;
    Finish(job);
    return;
  }
  job.out = out[0];
  jobs_.emplace(job.out, std::move(job));
}

std::vector<int> KeyringBroker::fds() const {
  std::vector<int> fds;
  for (const auto& [fd, job] : jobs_) {
    if (!job.eof) {
      fds.push_back(fd);
    }
  }
  return fds;
}

void KeyringBroker::OnReadable(int fd) {
  auto it = jobs_.find(fd);
  if (it == jobs_.end()) {
    return;
  }
  Job& job = it->second;
  char buffer[4096];
  const ssize_t n = read(fd, buffer, sizeof(buffer));
  if (n > 0) {
    if (job.output.size() <= 9000) {  // enough to know it's too long
      job.output.append(buffer, static_cast<size_t>(n));
    }
    return;
  }
  if (n < 0 && errno == EINTR) {
    return;
  }
  job.eof = true;
  if (!job.exited && waitpid(job.pid, &job.status, 0) == job.pid) {
    job.exited = true;
  }
  Finish(job);
}

bool KeyringBroker::OnChildExit(pid_t pid, int status) {
  for (auto& [fd, job] : jobs_) {
    if (job.pid == pid) {
      job.exited = true;
      job.status = status;
      if (job.eof) {
        Finish(job);
      }
      return true;
    }
  }
  return false;
}

void KeyringBroker::Forget(int fd) {
  for (auto& [pipe, job] : jobs_) {
    if (job.client_fd == fd) {
      job.client_fd = -1;
    }
  }
}

void KeyringBroker::Finish(Job& job) {
  const int code = WIFEXITED(job.status) ? WEXITSTATUS(job.status) : kNoKeyring;
  if (job.command == "pw-list") {
    if (code == kNoKeyring) {
      Answer(job, "unavailable");
    } else {
      // One encoded user name per line, as lrb_keyring prints them.
      std::string users = "ok";
      size_t start = 0;
      for (size_t end; (end = job.output.find('\n', start)) != std::string::npos;
           start = end + 1) {
        // Never a line over the limit: the instance would drop the rest.
        if (end > start && users.size() + (end - start) < 8000) {
          users += " " + job.output.substr(start, end - start);
        }
      }
      Answer(job, users);
    }
  } else if (job.command == "pw-get" || job.command == "af-get") {
    // Over the limit: not one lrb saved (lrb_keyring prints it whole).
    Answer(job, code == kDone && !job.output.empty() &&
                        job.output.size() <= kMaxRecord
                    ? "ok " + FileBroker::Encode(job.output)
                    : "none");
  } else {
    Answer(job, code == kDone ? "1" : "0");
  }
  if (job.out >= 0) {
    close(job.out);
    jobs_.erase(job.out);  // `job` is gone after this
  }
}

void KeyringBroker::Answer(const Job& job, const std::string& rest) {
  static constexpr std::pair<std::string_view, std::string_view> kReplies[] = {
      {"pw-list", "pw-users"}, {"pw-get", "pw"},  {"pw-save", "pw-saved"},
      {"af-get", "af"},        {"af-save", "af-saved"}};
  auto reply = std::ranges::find(kReplies, std::string_view(job.command),
                                 &std::pair<std::string_view,
                                            std::string_view>::first);
  if (job.client_fd >= 0 && reply != std::end(kReplies)) {
    reply_(job.client_fd,
           std::string(reply->second) + " " + job.id + " " + rest);
  }
}

}  // namespace lrb::coordinator
