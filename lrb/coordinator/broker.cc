// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/coordinator/broker.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <span>
#include <string_view>

namespace lrb::coordinator {

namespace {

constexpr size_t kMaxId = 32;

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

void MakeDirs(const std::string& path) {
  for (size_t slash = path.find('/', 1); slash != std::string::npos;
       slash = path.find('/', slash + 1)) {
    mkdir(path.substr(0, slash).c_str(), 0700);
  }
  mkdir(path.c_str(), 0700);
}

std::string BaseName(const std::string& path) {
  const size_t slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// A file name the instance proposed, made safe: no directories, no control
// characters, never "." or "..".
std::string SafeName(const std::string& name) {
  std::string safe;
  for (char c : BaseName(name)) {
    safe += static_cast<unsigned char>(c) < 0x20 ? '_' : c;
  }
  if (safe.empty() || safe == "." || safe == "..") {
    return "download";
  }
  return safe;
}

bool Exists(const std::string& path) {
  struct stat st = {};
  return lstat(path.c_str(), &st) == 0;
}

// `dir`/`name`, or "name (1).ext", "name (2).ext", ... if taken.
std::string FreePath(const std::string& dir, const std::string& name) {
  std::string path = dir + "/" + name;
  const size_t dot = name.rfind('.');
  const std::string stem = dot == std::string::npos || dot == 0
                               ? name
                               : name.substr(0, dot);
  const std::string ext = stem == name ? "" : name.substr(dot);
  for (int i = 1; Exists(path) && i < 1000; ++i) {
    path = dir + "/" + stem + " (" + std::to_string(i) + ")" + ext;
  }
  return path;
}

bool CopyFile(const std::string& from, const std::string& to) {
  const int in = open(from.c_str(), O_RDONLY | O_CLOEXEC);
  if (in < 0) {
    return false;
  }
  const int out =
      open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (out < 0) {
    close(in);
    return false;
  }
  bool ok = true;
  std::array<char, 1 << 16> buffer;
  for (;;) {
    const ssize_t n = read(in, buffer.data(), buffer.size());
    if (n == 0) {
      break;
    }
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      ok = false;
      break;
    }
    for (ssize_t done = 0; done < n;) {
      const auto rest = std::span(buffer).subspan(
          static_cast<size_t>(done), static_cast<size_t>(n - done));
      const ssize_t w = write(out, rest.data(), rest.size());
      if (w < 0 && errno == EINTR) {
        continue;
      }
      if (w <= 0) {
        ok = false;
        break;
      }
      done += w;
    }
    if (!ok) {
      break;
    }
  }
  close(in);
  ok &= close(out) == 0;
  if (!ok) {
    unlink(to.c_str());
  }
  return ok;
}

int RemoveEntry(const char* path, const struct stat*, int, struct FTW*) {
  remove(path);
  return 0;
}

void RemoveTree(const std::string& path) {
  nftw(path.c_str(), RemoveEntry, 16, FTW_DEPTH | FTW_PHYS);
}

std::string FileUri(const std::string& path) {
  return "file://" + FileBroker::Encode(path);
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

}  // namespace

FileBroker::FileBroker(std::string picker, std::string downloads_dir,
                       Reply reply)
    : picker_(std::move(picker)),
      downloads_dir_(std::move(downloads_dir)),
      reply_(std::move(reply)) {}

FileBroker::~FileBroker() {
  for (auto& [fd, pick] : picks_) {
    close(fd);
  }
}

// static
std::string FileBroker::Encode(const std::string& text) {
  static constexpr std::string_view kHex = "0123456789ABCDEF";
  static constexpr std::string_view kSafe = "._~/-";
  std::string out;
  for (unsigned char c : text) {
    if (isalnum(c) || kSafe.find(static_cast<char>(c)) != kSafe.npos) {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 15];
    }
  }
  return out;
}

// static
std::string FileBroker::Decode(const std::string& text) {
  std::string out;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size() &&
        isxdigit(static_cast<unsigned char>(text[i + 1])) &&
        isxdigit(static_cast<unsigned char>(text[i + 2]))) {
      out += static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16));
      i += 2;
    } else {
      out += text[i];
    }
  }
  return out;
}

void FileBroker::Handle(int fd,
                        const std::string& profile,
                        const std::string& command,
                        const std::string& args) {
  const std::vector<std::string> words = Words(args);
  if (words.empty() || !ValidId(words[0])) {
    return;
  }
  const std::string& id = words[0];
  if (profile.empty()) {
    // Not an instance this coordinator started: nothing to hand over.
    reply_(fd, (command == "pick-save" ? "save-target " : "picked ") + id +
                   " unavailable");
    return;
  }
  if (command == "pick-open") {
    Pick pick;
    pick.client_fd = fd;
    pick.profile = profile;
    pick.id = id;
    StartPicker(std::move(pick), words.size() > 1 && words[1] == "1");
  } else if (command == "pick-save") {
    Pick pick;
    pick.client_fd = fd;
    pick.profile = profile;
    pick.id = id;
    pick.save = true;
    pick.name = SafeName(words.size() > 1 ? Decode(words[1]) : "");
    StartPicker(std::move(pick), false);
  } else if (command == "save-done") {
    SaveDone(fd, profile, id);
  } else if (command == "save-cancel") {
    saves_.erase({profile, id});
  } else if (command == "show-saved") {
    auto it = saved_.find({profile, id});
    if (it == saved_.end()) {
      return;
    }
    const std::string folder = it->second.substr(0, it->second.rfind('/'));
    if (fork() == 0) {
      setsid();
      execlp("xdg-open", "xdg-open", folder.c_str(), nullptr);
      _exit(127);
    }
  }
}

void FileBroker::StartPicker(Pick pick, bool multiple) {
  int pipe_fds[2];
  if (pipe2(pipe_fds, O_CLOEXEC) != 0) {
    pick.exited = pick.eof = true;
    pick.status = 2 << 8;
    Finish(pick);
    return;
  }
  std::vector<std::string> args = {picker_, pick.save ? "save" : "open"};
  if (multiple) {
    args.push_back("--multiple");
  }
  if (pick.save) {
    args.push_back("--name=" + pick.name);
    args.push_back("--title=Save file");
  } else {
    args.push_back("--title=Choose files to upload");
  }
  pick.pid = fork();
  if (pick.pid == 0) {
    dup2(pipe_fds[1], STDOUT_FILENO);
    std::vector<char*> argv;
    for (std::string& arg : args) {
      argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    execv(argv[0], argv.data());
    _exit(2);  // no picker: as without a portal
  }
  close(pipe_fds[1]);
  if (pick.pid < 0) {
    close(pipe_fds[0]);
    pick.exited = pick.eof = true;
    pick.status = 2 << 8;
    Finish(pick);
    return;
  }
  pick.out = pipe_fds[0];
  picks_.emplace(pick.out, std::move(pick));
}

std::vector<int> FileBroker::fds() const {
  std::vector<int> fds;
  for (const auto& [fd, pick] : picks_) {
    if (!pick.eof) {
      fds.push_back(fd);
    }
  }
  return fds;
}

void FileBroker::OnReadable(int fd) {
  auto it = picks_.find(fd);
  if (it == picks_.end()) {
    return;
  }
  Pick& pick = it->second;
  char buffer[4096];
  const ssize_t n = read(fd, buffer, sizeof(buffer));
  if (n > 0) {
    pick.output.append(buffer, static_cast<size_t>(n));
    return;
  }
  if (n < 0 && errno == EINTR) {
    return;
  }
  pick.eof = true;
  if (!pick.exited) {
    // Its output is closed: it's exiting.
    if (waitpid(pick.pid, &pick.status, 0) == pick.pid) {
      pick.exited = true;
    }
  }
  Finish(pick);
}

bool FileBroker::OnChildExit(pid_t pid, int status) {
  for (auto& [fd, pick] : picks_) {
    if (pick.pid == pid) {
      pick.exited = true;
      pick.status = status;
      if (pick.eof) {
        Finish(pick);
      }
      return true;
    }
  }
  return false;
}

void FileBroker::Finish(Pick& pick) {
  std::vector<std::string> paths;
  size_t start = 0;
  for (size_t end; (end = pick.output.find('\n', start)) != std::string::npos;
       start = end + 1) {
    if (end > start) {
      paths.push_back(pick.output.substr(start, end - start));
    }
  }
  const int code =
      WIFEXITED(pick.status) ? WEXITSTATUS(pick.status) : 2;
  if (code == 0 && !paths.empty()) {
    pick.save ? FinishSave(pick, paths) : FinishOpen(pick, paths);
  } else if (code == 1) {
    reply_(pick.client_fd,
           (pick.save ? "save-target " : "picked ") + pick.id);
  } else if (pick.save) {
    // No picker: the Downloads folder, under a free name, as lrb did
    // without one.
    MakeDirs(downloads_dir_);
    const std::string target = FreePath(downloads_dir_, pick.name);
    saves_[{pick.profile, pick.id}] = target;
    reply_(pick.client_fd,
           "save-target " + pick.id + " " + FileUri(target) + " auto");
  } else {
    reply_(pick.client_fd, "picked " + pick.id + " unavailable");
  }
  if (pick.out >= 0) {
    close(pick.out);
    picks_.erase(pick.out);  // `pick` is gone after this
  }
}

void FileBroker::FinishOpen(Pick& pick, const std::vector<std::string>& paths) {
  // Copies, not links: the page can't change the user's original.
  std::string line = "picked " + pick.id;
  for (size_t i = 0; i < paths.size(); ++i) {
    const std::string dir = pick.profile + "/uploads/" + pick.id + "/" +
                            std::to_string(i);
    MakeDirs(dir);
    const std::string copy = dir + "/" + SafeName(paths[i]);
    if (CopyFile(paths[i], copy)) {
      line += " " + FileUri(copy);
    } else {
      fprintf(stderr, "lrb_coordinator: can't copy %s for upload: %s\n",
              paths[i].c_str(), strerror(errno));
    }
  }
  reply_(pick.client_fd, line);
}

void FileBroker::FinishSave(Pick& pick, const std::vector<std::string>& paths) {
  const std::string& target = paths.front();
  saves_[{pick.profile, pick.id}] = target;
  reply_(pick.client_fd, "save-target " + pick.id + " " + FileUri(target));
}

void FileBroker::SaveDone(int fd,
                          const std::string& profile,
                          const std::string& id) {
  auto it = saves_.find({profile, id});
  if (it == saves_.end()) {
    reply_(fd, "saved " + id + " 0");
    return;
  }
  const std::string target = it->second;
  saves_.erase(it);
  // Where the instance was told to write: its own profile, never a path it
  // names itself.
  const std::string staged =
      profile + "/downloads/" + id + "/" + BaseName(target);
  struct stat st = {};
  bool ok = lstat(staged.c_str(), &st) == 0 && S_ISREG(st.st_mode);
  if (ok && rename(staged.c_str(), target.c_str()) != 0) {
    ok = errno == EXDEV && CopyFile(staged, target);
    if (ok) {
      unlink(staged.c_str());
    }
  }
  if (!ok) {
    fprintf(stderr, "lrb_coordinator: can't save %s: %s\n", target.c_str(),
            strerror(errno));
  }
  RemoveTree(profile + "/downloads/" + id);
  if (ok) {
    saved_[{profile, id}] = target;
  }
  reply_(fd, "saved " + id + (ok ? " 1" : " 0"));
}

void FileBroker::Forget(int fd) {
  for (auto& [pipe, pick] : picks_) {
    if (pick.client_fd == fd) {
      pick.client_fd = -1;  // the answer goes nowhere
    }
  }
}

// static
void FileBroker::CleanProfile(const std::string& profile) {
  if (!profile.empty()) {
    RemoveTree(profile + "/uploads");
    RemoveTree(profile + "/downloads");
  }
}

}  // namespace lrb::coordinator
