// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// lrb_coordinator: maps sites to browser instances. One site = one window =
// one single-process lrb instance with that site's persistent profile.
//
//   lrb_coordinator [--browser=PATH] [--profiles-dir=DIR] [--socket=PATH]
//                   [--moderate-percent=N] [--critical-percent=N] [--verbose]
//                   [--adblock-setting=full|lean|off] [--adblock-file=PATH]
//                   [--no-confine] [--picker=PATH] [URL] [-- BROWSER_ARGS...]
//
// Every instance runs confined (confine.h): it reads the system's files, its
// own profile and the settings, and nothing else of the user's; it can't
// reach other instances' memory or signal anything outside. --no-confine is
// for debugging only.
//
// Opens URL (or a blank window), then serves the instances' requests until
// the last instance exits. A second coordinator started while one runs
// hands its URL to the running one and exits.
//
// It never loads web content, so a compromised page can't steer which
// windows open, which profile they get, or which URL another site's window
// shows (lrb/coordinator/rules.h).
//
// Protocol: newline-terminated text lines over a Unix stream socket in a
// directory only the user can read ($XDG_RUNTIME_DIR/lrb).
//   instance → coordinator   "site <site>"        this instance's site
//                            "active"             the user is using it
//                            "background-tabs <n>"  live tabs not shown
//                            "open <site> <url> [bounds=<x>,<y>,<w>,<h>]
//                                  [back=<url>] [restore]"
//                                                 show url in site's window
//                                                 (an instance without a site
//                                                 sends its first page so and
//                                                 exits). bounds: the window
//                                                 replaces the sender's, in its
//                                                 place; back: the page it
//                                                 left, for Back; restore:
//                                                 going Back, so restore the
//                                                 window the site left
//   launcher → coordinator   "launch <url>"       open url, site unknown yet
//   coordinator → instance   "show <url> [bounds=..] [back=..]"
//                                                 url is on your site
//                            "discard"            drop your pages, keep the
//                                                 windows and their history
//                            "discard-background" the same, only for tabs
//                                                 not shown (the instance in
//                                                 use, after every other is
//                                                 discarded)
//                            "close"              save your windows' history
//                                                 and exit (last resort)
//
// Content blocking: windows get --lrb-adblock-file once the engine file
// exists ($XDG_DATA_HOME/lrb/adblock/<setting>.adb). When it is missing or
// over a day old, the coordinator runs `lrb --lrb-update-lists` (no window)
// to fetch the filter lists and compile it. The setting defaults to lean on
// machines with less than 1.5 GB of RAM, full otherwise.
//
// Under memory pressure it discards the least recently used instance's
// pages, never the most recently used one: one every 10 s while any budget
// is below --moderate-percent (default 30), one every 3 s below
// --critical-percent (default 15); the same thresholds as the instances'
// own evaluator (patches/0001). Moderate keeps room for pages coming back:
// at 384 MB with critical alone, 3 of 5 windows were OOM-killed while the
// user went back through them. 0 disables a tier. Still critical with every
// other instance discarded, it closes the least recently used one, which
// saves its history and is restored when its site is opened again.

#ifdef UNSAFE_BUFFERS_BUILD
// This file is the libc/syscall boundary (argv, sockets, fprintf) of a
// program that links nothing from Chromium, so base::span and friends are
// unavailable. Everything that parses untrusted input is in rules.cc, which
// keeps the check.
#pragma allow_unsafe_buffers
#endif

#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lrb/coordinator/broker.h"
#include "lrb/coordinator/confine.h"
#include "lrb/coordinator/pressure.h"
#include "lrb/coordinator/rules.h"

namespace lrb::coordinator {
namespace {

// How a site's window opens, all validated: where (x,y,w,h, empty for the
// default), the page it was left from (for Back), and whether to restore
// the window the site left (going Back to it).
struct OpenOptions {
  std::string bounds;
  std::string back;
  bool restore = false;
};


constexpr size_t kMaxLine = 9000;  // longest URL plus the command

struct Options {
  std::string browser;
  std::string profiles_dir;
  std::string socket_path;
  std::string url;
  std::vector<std::string> browser_args;
  bool verbose = false;
  std::string adblock_setting;  // "full", "lean", "off"; empty: by RAM
  std::string adblock_file;
  int moderate_percent = 30;
  int critical_percent = 15;
  bool confine = true;
  std::string picker;  // lrb_picker; tests give a stand-in
};

using Clock = std::chrono::steady_clock;

// stderr line with seconds since start, so events line up with instances'
// and harness logs.
__attribute__((format(printf, 1, 2))) void Log(const char* format, ...) {
  static const Clock::time_point start = Clock::now();
  const double seconds =
      std::chrono::duration<double>(Clock::now() - start).count();
  fprintf(stderr, "lrb_coordinator %.3f: ", seconds);
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  fputc('\n', stderr);
}

struct Client {
  int fd = -1;
  std::string site;  // empty until the instance reports it
  // The profile this coordinator started the instance with, by its process
  // (SO_PEERCRED): what the file broker trusts, never what it says.
  std::string profile;
  std::string input;
  // New instances count as least recently used until the user is at them
  // ("active"), unless launched for a user's request (wanted_site_).
  Clock::time_point last_active;
  bool discarded = false;
  bool closing = false;  // told to close; gone once it disconnects
  // Live tabs not shown in their window (the instance reports them): what
  // can sleep in the instance in use before anything closes.
  int background_tabs = 0;
};

std::string DirName(const std::string& path) {
  const size_t slash = path.rfind('/');
  return slash == std::string::npos ? "." : path.substr(0, slash);
}

std::string Getenv(const char* name) {
  const char* value = getenv(name);
  return value ? value : "";
}

bool MakeDirs(const std::string& path, mode_t mode) {
  for (size_t i = 1; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == '/') {
      const std::string part = path.substr(0, i);
      if (mkdir(part.c_str(), mode) != 0 && errno != EEXIST) {
        return false;
      }
    }
  }
  return true;
}

// What a confined instance may reach: the system's files (libraries, fonts,
// certificates, devices), the desktop's settings it renders with, the filter
// lists, and `rw_dir` (its profile) to write. Not the user's home: other
// sites' profiles, keys, documents. Files the user picks reach it through the
// coordinator (TODO: the picker broker).
coordinator::ConfinePolicy InstancePolicy(const Options& options,
                                          const std::string& rw_dir) {
  const std::string home = Getenv("HOME");
  auto xdg = [&home](const char* name, const char* fallback) {
    const std::string value = Getenv(name);
    return value.empty() ? home + fallback : value;
  };
  const std::string config = xdg("XDG_CONFIG_HOME", "/.config");
  const std::string data = xdg("XDG_DATA_HOME", "/.local/share");
  const std::string cache = xdg("XDG_CACHE_HOME", "/.cache");
  const std::string runtime = Getenv("XDG_RUNTIME_DIR");
  const std::string xauthority = Getenv("XAUTHORITY");

  coordinator::ConfinePolicy policy;
  policy.read_exec = {DirName(options.browser), "/usr", "/lib", "/lib64",
                      "/lib32", "/bin", "/sbin"};
  policy.read = {
      "/etc", "/proc", "/sys", "/run", "/var/lib/dbus",
      "/var/cache/fontconfig", DirName(options.adblock_file),
      xauthority.empty() ? home + "/.Xauthority" : xauthority,
      // Look and feel: GTK theme and settings, fonts, icons, cursors.
      config + "/gtk-3.0", config + "/gtk-4.0", config + "/fontconfig",
      config + "/dconf", config + "/pulse", data + "/fonts", data + "/icons",
      data + "/themes", home + "/.fonts", home + "/.icons", home + "/.themes",
  };
  policy.read_write = {
      rw_dir,
      // TODO: the settings dialog writes here; move its writes to the
      // coordinator so a hijacked page can't change the search engine.
      config + "/lrb",
      "/dev/shm", "/dev/null", "/dev/zero", "/dev/full", "/dev/random",
      "/dev/urandom", "/dev/dri", "/dev/snd",
      cache + "/fontconfig", cache + "/mesa_shader_cache",
      runtime + "/dconf",
  };
  for (int i = 0; i < 8; ++i) {  // cameras
    policy.read_write.push_back("/dev/video" + std::to_string(i));
  }
  return policy;
}

bool ParseOptions(int argc, char** argv, Options& options) {
  std::string self = argv[0];
  options.browser = DirName(self) + "/lrb";
  const std::string data_home = !Getenv("XDG_DATA_HOME").empty()
                                    ? Getenv("XDG_DATA_HOME")
                                    : Getenv("HOME") + "/.local/share";
  options.profiles_dir = data_home + "/lrb/sites";
  const std::string runtime = !Getenv("XDG_RUNTIME_DIR").empty()
                                  ? Getenv("XDG_RUNTIME_DIR")
                                  : "/tmp/lrb-" + std::to_string(getuid());
  options.socket_path = runtime + "/lrb/coordinator.sock";

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--") {
      for (++i; i < argc; ++i) {
        options.browser_args.emplace_back(argv[i]);
      }
    } else if (arg.starts_with("--browser=")) {
      options.browser = arg.substr(10);
    } else if (arg.starts_with("--profiles-dir=")) {
      options.profiles_dir = arg.substr(15);
    } else if (arg.starts_with("--socket=")) {
      options.socket_path = arg.substr(9);
    } else if (arg.starts_with("--adblock-setting=")) {
      options.adblock_setting = arg.substr(18);
    } else if (arg.starts_with("--adblock-file=")) {
      options.adblock_file = arg.substr(15);
    } else if (arg == "--verbose") {
      options.verbose = true;
    } else if (arg == "--no-confine") {
      options.confine = false;
    } else if (arg.starts_with("--picker=")) {
      options.picker = arg.substr(9);
    } else if (arg.starts_with("--moderate-percent=")) {
      options.moderate_percent = atoi(std::string(arg.substr(19)).c_str());
    } else if (arg.starts_with("--critical-percent=")) {
      options.critical_percent = atoi(std::string(arg.substr(19)).c_str());
    } else if (!arg.starts_with("-") && options.url.empty()) {
      options.url = arg;
    } else {
      fprintf(stderr, "lrb_coordinator: unknown argument %s\n", argv[i]);
      return false;
    }
  }
  if (options.adblock_setting.empty()) {
    struct sysinfo info = {};
    const uint64_t ram =
        sysinfo(&info) == 0 ? uint64_t{info.totalram} * info.mem_unit : 0;
    options.adblock_setting =
        ram && ram < uint64_t{1536} * 1024 * 1024 ? "lean" : "full";
  }
  if (options.adblock_setting != "full" && options.adblock_setting != "lean" &&
      options.adblock_setting != "off") {
    fprintf(stderr, "lrb_coordinator: --adblock-setting is full, lean or off\n");
    return false;
  }
  if (options.adblock_file.empty()) {
    options.adblock_file =
        data_home + "/lrb/adblock/" + options.adblock_setting + ".adb";
  }
  if (!options.url.empty() && !IsAcceptableUrl(options.url)) {
    fprintf(stderr, "lrb_coordinator: only http(s) URLs: %s\n",
            options.url.c_str());
    return false;
  }
  return true;
}

sockaddr_un SocketAddress(const std::string& path) {
  sockaddr_un address = {};
  address.sun_family = AF_UNIX;
  strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
  return address;
}

bool WriteAll(int fd, const std::string& data) {
  size_t done = 0;
  while (done < data.size()) {
    const ssize_t n = send(fd, data.data() + done, data.size() - done,
                           MSG_NOSIGNAL);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      return false;
    }
    done += static_cast<size_t>(n);
  }
  return true;
}

// Hands `url` to an already running coordinator. False if none runs.
bool HandToRunningCoordinator(const Options& options) {
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  sockaddr_un address = SocketAddress(options.socket_path);
  if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    close(fd);
    return false;
  }
  if (!options.url.empty()) {
    WriteAll(fd, "launch " + options.url + "\n");
  }
  close(fd);
  return true;
}

// The user's Downloads folder (XDG_DOWNLOAD_DIR in user-dirs.dirs), else
// ~/Downloads.
std::string UserDownloadsDir() {
  const std::string home = Getenv("HOME");
  const std::string config = Getenv("XDG_CONFIG_HOME").empty()
                                 ? home + "/.config"
                                 : Getenv("XDG_CONFIG_HOME");
  if (FILE* file = fopen((config + "/user-dirs.dirs").c_str(), "r")) {
    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), file)) {
      std::string line = buffer;
      if (!line.starts_with("XDG_DOWNLOAD_DIR=\"")) {
        continue;
      }
      line = line.substr(18, line.rfind('"') - 18);
      if (line.starts_with("$HOME")) {
        line = home + line.substr(5);
      }
      fclose(file);
      if (!line.empty() && line != home && line != home + "/") {
        return line;
      }
      return home + "/Downloads";
    }
    fclose(file);
  }
  return home + "/Downloads";
}

class Coordinator {
 public:
  explicit Coordinator(Options options)
      : options_(std::move(options)),
        broker_(options_.picker.empty()
                    ? DirName(options_.browser) + "/lrb_picker"
                    : options_.picker,
                UserDownloadsDir(),
                [this](int fd, const std::string& line) {
                  if (fd >= 0 && clients_.contains(fd) &&
                      !WriteAll(fd, line + "\n")) {
                    Drop(fd);
                  }
                }) {}

  int Run() {
    if (!Listen()) {
      return 1;
    }
    MaybeUpdateLists();
    WaitForFirstEngine();
    Launch(/*site=*/"", options_.url);
    while (children_ > 0 || !clients_.empty()) {
      PollOnce();
      Reap();
      CheckPressure();
      MaybeUpdateLists();
    }
    unlink(options_.socket_path.c_str());
    return 0;
  }

 private:
  bool Listen() {
    // Only this user may reach the socket's directory.
    const std::string dir = DirName(options_.socket_path);
    if (!MakeDirs(dir, 0700) || chmod(dir.c_str(), 0700) != 0) {
      perror("lrb_coordinator: socket directory");
      return false;
    }
    unlink(options_.socket_path.c_str());  // stale: none answered
    listen_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un address = SocketAddress(options_.socket_path);
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) != 0 ||
        listen(listen_fd_, 16) != 0) {
      perror("lrb_coordinator: listen");
      return false;
    }
    return true;
  }

  void PollOnce() {
    std::vector<pollfd> fds;
    fds.push_back({listen_fd_, POLLIN, 0});
    for (const auto& [fd, client] : clients_) {
      fds.push_back({fd, POLLIN, 0});
    }
    const size_t first_picker = fds.size();
    for (int fd : broker_.fds()) {
      fds.push_back({fd, POLLIN, 0});
    }
    // Wakes every 250 ms to check memory pressure and reap instances.
    if (poll(fds.data(), fds.size(), 250) <= 0) {
      return;
    }
    if (fds[0].revents & POLLIN) {
      const int fd = accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
      if (fd >= 0) {
        clients_[fd].fd = fd;
        ucred peer = {};
        socklen_t size = sizeof(peer);
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) == 0 &&
            child_profiles_.contains(peer.pid)) {
          clients_[fd].profile = child_profiles_.at(peer.pid);
        }
      }
    }
    for (size_t i = 1; i < fds.size(); ++i) {
      if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
        if (i < first_picker) {
          ReadFrom(fds[i].fd);
        } else {
          broker_.OnReadable(fds[i].fd);  // a picker answered
        }
      }
    }
  }

  void ReadFrom(int fd) {
    if (!clients_.contains(fd)) {
      return;  // dropped earlier in this poll round (e.g. by MakeRoom)
    }
    char buffer[4096];
    const ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
    if (n <= 0) {
      Drop(fd);
      return;
    }
    Client& client = clients_.at(fd);
    client.input.append(buffer, static_cast<size_t>(n));
    size_t newline;
    while ((newline = client.input.find('\n')) != std::string::npos) {
      const std::string line = client.input.substr(0, newline);
      client.input.erase(0, newline + 1);
      Handle(client, line);
    }
    if (client.input.size() > kMaxLine) {
      Log("dropping instance: line too long");
      Drop(fd);
    }
  }

  void Drop(int fd) {
    broker_.Forget(fd);
    close(fd);
    clients_.erase(fd);
  }

  void Handle(Client& client, const std::string& line) {
    const size_t space = line.find(' ');
    const std::string command = line.substr(0, space);
    const std::string args =
        space == std::string::npos ? "" : line.substr(space + 1);

    if (options_.verbose && command != "active") {
      Log("fd %d (%s): %s", client.fd,
          client.site.empty() ? "no site" : client.site.c_str(), line.c_str());
    }
    if (command.starts_with("pick-") || command.starts_with("save-")) {
      broker_.Handle(client.fd, client.profile, command, args);
    } else if (command == "active") {
      const bool reloading = client.discarded;
      client.last_active = Clock::now();
      client.discarded = false;
      if (reloading) {
        MakeRoom(&client);  // its page is coming back
      }
    } else if (command == "background-tabs") {
      client.background_tabs = std::max(0, atoi(args.c_str()));
    } else if (command == "site") {
      // An instance's site is set once: by --lrb-site, or by its first page.
      if (client.site.empty() && IsValidSite(args)) {
        client.site = args;
        if (client.site == wanted_site_) {
          // Launched because the user opened this site: it's in use now.
          client.last_active = Clock::now();
          wanted_site_.clear();
        }
      }
    } else if (command == "open") {
      // <site> <url> [bounds=<x,y,w,h>] [back=<url>] [restore]
      std::vector<std::string> parts;
      for (size_t start = 0; start <= args.size();) {
        const size_t end = std::min(args.find(' ', start), args.size());
        parts.push_back(args.substr(start, end - start));
        start = end + 1;
      }
      if (parts.size() < 2 || !UrlBelongsToSite(parts[1], parts[0])) {
        Log("refused open %s", args.c_str());
        return;
      }
      OpenOptions open;
      for (size_t i = 2; i < parts.size(); ++i) {
        const std::string_view part = parts[i];
        std::optional<WindowBounds> bounds;
        if (part.starts_with("bounds=") &&
            (bounds = ParseBounds(part.substr(7)))) {
          open.bounds = FormatBounds(*bounds);
        } else if (part.starts_with("back=") &&
                   IsAcceptableUrl(part.substr(5))) {
          open.back = std::string(part.substr(5));
        } else if (part == "restore") {
          open.restore = true;
        } else {
          Log("refused open %s", args.c_str());
          return;
        }
      }
      Open(parts[0], parts[1], open);
    } else if (command == "launch") {
      if (IsAcceptableUrl(args)) {
        Launch(/*site=*/"", args);
      }
    }
  }

  // Shows `url` in `site`'s window, launching the instance if it has none.
  void Open(const std::string& site,
            const std::string& url,
            const OpenOptions& open) {
    for (auto& [fd, client] : clients_) {
      if (client.site == site && !client.closing) {
        const bool reloading = client.discarded;
        client.last_active = Clock::now();
        client.discarded = false;
        if (reloading) {
          MakeRoom(&client);
        }
        std::string line = "show " + url;
        if (!open.bounds.empty()) {
          line += " bounds=" + open.bounds;
        }
        if (!open.back.empty()) {
          line += " back=" + open.back;
        }
        if (!WriteAll(fd, line + "\n")) {
          Drop(fd);
          break;  // launch a fresh one below
        }
        return;
      }
    }
    wanted_site_ = site;
    MakeRoom(nullptr);  // a page load is coming
    Launch(site, url, open);
  }

  void Launch(const std::string& site,
              const std::string& url,
              const OpenOptions& open = OpenOptions()) {
    std::vector<std::string> args = {options_.browser,
                                     "--lrb-coordinator=" + options_.socket_path,
                                     "--lrb-profiles-dir=" + options_.profiles_dir};
    std::string profile;
    if (!site.empty()) {
      // Each site keeps its own profile across restarts.
      profile = options_.profiles_dir + "/" + site;
      MakeDirs(profile, 0700);
      args.push_back("--user-data-dir=" + profile);
      args.push_back("--lrb-site=" + site);
    } else {
      // Site unknown (a URL from the command line, or a blank window): this
      // instance only resolves the site. It sends its first page back with
      // "open <site> <url>" and exits, so the page opens with the right
      // profile. Its own profile is never used for browsing.
      profile = options_.profiles_dir + "/.resolver";
      MakeDirs(profile, 0700);
      args.push_back("--user-data-dir=" + profile);
    }
    struct stat engine = {};
    if (options_.adblock_setting != "off" &&
        stat(options_.adblock_file.c_str(), &engine) == 0) {
      args.push_back("--lrb-adblock-file=" + options_.adblock_file);
    }
    for (const std::string& arg : options_.browser_args) {
      args.push_back(arg);
    }
    if (!open.bounds.empty()) {
      args.push_back("--lrb-window-bounds=" + open.bounds);
    }
    if (!open.back.empty()) {
      args.push_back("--lrb-back-url=" + open.back);
    }
    if (open.restore) {
      args.push_back("--lrb-restore-left");
    }
    if (!url.empty()) {
      args.push_back(url);
    }

    if (Spawn(std::move(args), profile) > 0) {
      ++children_;
    }
  }

  // Before a known page load (a site opened, a discarded page coming back):
  // a load can take 100 MB in a second, faster than reacting to pressure
  // (measured at 384 MB: 15% left to an OOM kill in 1.1 s), and a discard
  // frees memory over 1-11 s. So below the moderate threshold, drop the
  // least recently used other page now, ignoring the rate limit; with
  // nothing left to discard and critical, close one. `keep` is the instance
  // about to load (null for one not launched yet).
  void MakeRoom(Client* keep) {
    const std::vector<Budget> budgets = ReadBudgets();
    if (!BelowPercent(budgets, options_.moderate_percent)) {
      return;
    }
    Client* victim = nullptr;
    for (auto& [fd, client] : clients_) {
      if (client.site.empty() || &client == keep || client.discarded ||
          client.closing) {
        continue;
      }
      if (!victim || client.last_active < victim->last_active) {
        victim = &client;
      }
    }
    const Clock::time_point now = Clock::now();
    if (victim) {
      Log("making room for a page load, discarding %s", victim->site.c_str());
      last_discard_ = now;
      victim->discarded = true;
      if (!WriteAll(victim->fd, "discard\n")) {
        Drop(victim->fd);
      }
    } else if (keep && keep->background_tabs > 0) {
      // The instance loading a page has background tabs: they sleep first.
      Log("making room for a page load, sleeping background tabs of %s",
          keep->site.c_str());
      last_discard_ = now;
      keep->background_tabs = 0;
      if (!WriteAll(keep->fd, "discard-background\n")) {
        Drop(keep->fd);
      }
    } else if (BelowPercent(budgets, options_.critical_percent)) {
      Client* oldest = nullptr;
      for (auto& [fd, client] : clients_) {
        if (client.site.empty() || &client == keep || client.closing) {
          continue;
        }
        if (!oldest || client.last_active < oldest->last_active) {
          oldest = &client;
        }
      }
      if (oldest) {
        Log("making room for a page load, closing %s", oldest->site.c_str());
        last_discard_ = now;
        oldest->closing = true;
        if (!WriteAll(oldest->fd, "close\n")) {
          Drop(oldest->fd);
        }
      }
    }
  }

  // Under pressure, drops the pages of the least recently used instance.
  void CheckPressure() {
    const Clock::time_point now = Clock::now();
    if (now - last_discard_ < std::chrono::seconds(3)) {
      return;
    }
    const std::vector<Budget> budgets = ReadBudgets();
    const bool critical = BelowPercent(budgets, options_.critical_percent);
    const bool moderate = BelowPercent(budgets, options_.moderate_percent);
    if (!critical &&
        !(moderate && now - last_discard_ >= std::chrono::seconds(10))) {
      return;
    }
    Client* most_recent = nullptr;
    Client* victim = nullptr;
    for (auto& [fd, client] : clients_) {
      if (client.site.empty()) {
        continue;
      }
      if (!most_recent || client.last_active > most_recent->last_active) {
        most_recent = &client;
      }
    }
    for (auto& [fd, client] : clients_) {
      if (client.site.empty() || client.discarded || &client == most_recent) {
        continue;
      }
      if (!victim || client.last_active < victim->last_active) {
        victim = &client;
      }
    }
    if (!victim) {
      // Every other instance is discarded: the background tabs of the one
      // in use sleep before anything closes.
      if (most_recent && most_recent->background_tabs > 0) {
        Log("%s memory pressure, sleeping %d background tab(s) of %s",
            critical ? "critical" : "moderate", most_recent->background_tabs,
            most_recent->site.c_str());
        last_discard_ = now;
        most_recent->background_tabs = 0;  // it reports anew
        if (!WriteAll(most_recent->fd, "discard-background\n")) {
          Drop(most_recent->fd);
        }
        return;
      }
      if (critical) {
        CloseLeastRecentlyUsed(most_recent, now);
      }
      return;
    }
    Log("%s memory pressure, discarding %s", critical ? "critical" : "moderate",
        victim->site.c_str());
    LogWindows(most_recent);
    last_discard_ = now;
    victim->discarded = true;
    if (!WriteAll(victim->fd, "discard\n")) {
      Drop(victim->fd);
    }
  }

  // With --verbose: every instance's state, most recently used first.
  void LogWindows(const Client* most_recent, bool always = false) {
    if (!options_.verbose && !always) {
      return;
    }
    const Clock::time_point now = Clock::now();
    for (const auto& [fd, client] : clients_) {
      Log("  fd %d %s%s%s%s, %d background tab(s), last active %.1f s ago", fd,
          client.site.empty() ? "(no site)" : client.site.c_str(),
          &client == most_recent ? " [in use]" : "",
          client.discarded ? " [discarded]" : "",
          client.closing ? " [closing]" : "", client.background_tabs,
          std::chrono::duration<double>(now - client.last_active).count());
    }
  }

  // Last resort: every instance but the one in use is already discarded and
  // memory is still critical.
  void CloseLeastRecentlyUsed(Client* most_recent, Clock::time_point now) {
    Client* victim = nullptr;
    for (auto& [fd, client] : clients_) {
      if (client.site.empty() || &client == most_recent || client.closing) {
        continue;
      }
      if (!victim || client.last_active < victim->last_active) {
        victim = &client;
      }
    }
    if (!victim) {
      // Only the window in use is left. Say so now and then: it explains a
      // squeeze the coordinator can't relieve.
      if (now - last_stuck_report_ >= std::chrono::seconds(10)) {
        last_stuck_report_ = now;
        Log("critical memory pressure, nothing left to discard or close");
        LogWindows(most_recent, /*always=*/true);
      }
      return;
    }
    Log("critical memory pressure, closing %s", victim->site.c_str());
    LogWindows(most_recent);
    last_discard_ = now;
    victim->closing = true;
    if (!WriteAll(victim->fd, "close\n")) {
      Drop(victim->fd);
    }
  }

  // Runs the filter-list updater when the engine file is missing or older
  // than a day; checked at most hourly, one updater at a time.
  void MaybeUpdateLists() {
    if (options_.adblock_setting == "off" || updater_pid_ > 0) {
      return;
    }
    const Clock::time_point now = Clock::now();
    if (last_update_check_ != Clock::time_point() &&
        now - last_update_check_ < std::chrono::hours(1)) {
      return;
    }
    last_update_check_ = now;
    struct stat engine = {};
    if (stat(options_.adblock_file.c_str(), &engine) == 0 &&
        time(nullptr) - engine.st_mtime < 24 * 3600) {
      return;
    }
    const size_t slash = options_.adblock_file.rfind('/');
    const std::string dir = options_.adblock_file.substr(0, slash);
    MakeDirs(dir + "/updater-profile", 0700);
    std::vector<std::string> args = {
        options_.browser, "--lrb-update-lists=" + options_.adblock_file,
        "--lrb-adblock-setting=" + options_.adblock_setting,
        "--user-data-dir=" + dir + "/updater-profile"};
    for (const std::string& arg : options_.browser_args) {
      if (!arg.starts_with("--remote-debugging")) {
        args.push_back(arg);
      }
    }
    Log("updating filter lists (%s) into %s", options_.adblock_setting.c_str(),
        options_.adblock_file.c_str());
    updater_pid_ = Spawn(args, dir);
  }

  // On a first run there is no engine file yet: wait for the updater (a few
  // seconds) so that even the first window blocks ads. Later updates run in
  // the background while windows use the previous file.
  void WaitForFirstEngine() {
    struct stat engine = {};
    if (updater_pid_ <= 0 || stat(options_.adblock_file.c_str(), &engine) == 0) {
      return;
    }
    const Clock::time_point deadline = Clock::now() + std::chrono::seconds(15);
    while (Clock::now() < deadline) {
      if (waitpid(updater_pid_, nullptr, WNOHANG) == updater_pid_) {
        updater_pid_ = 0;
        Log("filter-list update finished");
        return;
      }
      usleep(100 * 1000);
    }
    Log("filter lists not ready after 15 s; first window without blocking");
  }

  // Starts `args` (lrb), confined to writing `rw_dir` (see InstancePolicy).
  pid_t Spawn(std::vector<std::string> args, const std::string& rw_dir) {
    const pid_t pid = fork();
    if (pid == 0) {
      if (options_.confine) {
        // Temporary files in the profile: /tmp is outside the confinement.
        const std::string tmp = rw_dir + "/tmp";
        MakeDirs(tmp, 0700);
        setenv("TMPDIR", tmp.c_str(), 1);
        std::string error;
        if (!coordinator::Confine(InstancePolicy(options_, rw_dir), &error)) {
          fprintf(stderr, "lrb_coordinator: not starting %s: %s\n",
                  args[0].c_str(), error.c_str());
          _exit(126);
        }
        if (!error.empty()) {
          fprintf(stderr, "lrb_coordinator: %s\n", error.c_str());
        }
      }
      std::vector<char*> argv;
      for (std::string& arg : args) {
        argv.push_back(arg.data());
      }
      argv.push_back(nullptr);
      execv(argv[0], argv.data());
      perror("lrb_coordinator: exec");
      _exit(127);
    }
    if (pid < 0) {
      perror("lrb_coordinator: fork");
    } else {
      child_profiles_[pid] = rw_dir;
    }
    return pid;
  }

  void Reap() {
    pid_t pid;
    int status = 0;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
      if (broker_.OnChildExit(pid, status)) {
        continue;
      }
      auto profile = child_profiles_.find(pid);
      if (profile == child_profiles_.end()) {
        continue;  // not ours to count (the file manager for show-saved)
      }
      coordinator::FileBroker::CleanProfile(profile->second);
      child_profiles_.erase(profile);
      if (pid == updater_pid_) {
        updater_pid_ = 0;
        Log("filter-list update finished");
      } else {
        --children_;
      }
    }
  }

  const Options options_;
  int listen_fd_ = -1;
  int children_ = 0;
  std::map<int, Client> clients_;
  coordinator::FileBroker broker_;
  // Instances' profiles by process id (Spawn).
  std::map<pid_t, std::string> child_profiles_;
  Clock::time_point last_discard_;
  Clock::time_point last_stuck_report_;
  Clock::time_point last_update_check_;
  pid_t updater_pid_ = 0;
  // The site last launched for a user's request, until it registers.
  std::string wanted_site_;
};

}  // namespace
}  // namespace lrb::coordinator

int main(int argc, char** argv) {
  using namespace lrb::coordinator;
  Options options;
  if (!ParseOptions(argc, argv, options)) {
    return 2;
  }
  if (HandToRunningCoordinator(options)) {
    return 0;
  }
  return Coordinator(std::move(options)).Run();
}
