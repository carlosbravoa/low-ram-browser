// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// lrb_coordinator: starts and serves lrb's browser instances, in one of two
// modes (lrb/common/mode.h), decided per start:
//
// - lean: one site = one single-process lrb instance with that site's
//   persistent profile, confined as a whole;
// - standard: one lrb instance for every site, with Chromium's processes,
//   sandbox and site isolation, on one profile ($XDG_DATA_HOME/lrb/standard,
//   beside the sites' profiles).
//
//   lrb_coordinator [--browser=PATH] [--profiles-dir=DIR] [--socket=PATH]
//                   [--mode=auto|lean|standard] [--max-parked=N]
//                   [--moderate-percent=N] [--critical-percent=N]
//                   [--moderate-max-mb=N] [--critical-max-mb=N] [--verbose]
//                   [--adblock-setting=full|lean|off] [--adblock-file=PATH]
//                   [--safety-lists=on|off] [--no-release-check]
//                   [--no-confine] [--picker=PATH] [--keyring=PATH]
//                   [URL] [-- BROWSER_ARGS...]
//
// The mode: --mode, else the setting "mode" (settings.json: "lean",
// "standard", or absent for automatic). Automatic is standard when the
// machine has at least 3 GiB of RAM, 1.5 GiB of it free at start, and
// `lrb --lrb-sandbox-check` exits 0 under the standard instance's
// confinement (a user namespace can be created: AppArmor on Ubuntu 23.10+
// may say no); lean otherwise. Standard always needs the check to pass.
//
// Every instance runs confined (confine.h): it reads the system's files, its
// own profile and the settings, and nothing else of the user's; it can't
// reach other instances' memory or signal anything outside. The standard
// instance's seccomp filter also lets Chromium's sandbox create its
// namespaces. --no-confine is for debugging only.
//
// Opens URL (or what the settings say to start with), then serves the
// instances' requests until the last instance exits. A second coordinator
// started while one runs hands its URL to the running one and exits. A URL's
// site comes from `lrb --lrb-site-of=<url>` (no browser started); only if
// that fails does a resolver instance load the page to find it.
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
//                            "parked 0|1"         every window of the
//                                                 instance is hidden (1) or
//                                                 not; sent on change
//                            "open <site> <url> [bounds=<x>,<y>,<w>,<h>]
//                                  [back=<url>] [restore] [handoff=<n>]"
//                                                 show url in site's window
//                                                 (an instance without a site
//                                                 sends its first page so and
//                                                 exits). bounds: the window
//                                                 replaces the sender's, in its
//                                                 place; back: the page it
//                                                 left, for Back; restore:
//                                                 going Back, so restore the
//                                                 window the site left;
//                                                 handoff: the sender's window
//                                                 hides ("hide <n>") once the
//                                                 new one reports it is shown,
//                                                 or 8 s after if the target
//                                                 runs. Standard mode: url
//                                                 goes to the one instance,
//                                                 whatever the site
//                            "shown <token>"      the window for this hand-off
//                                                 token is on screen
//                            bookmarks and address-bar suggestions:
//                            lrb/coordinator/bookmarks.h (the standard
//                            instance may bookmark any http(s) page)
//                            saved passwords and autofill: "pw-*", "af-*",
//                            lrb/coordinator/keyring_broker.h (the standard
//                            instance names each password's site)
//                            "open-app <url>"     a link for another app
//                                                 (mailto:, tel:, ...; percent-
//                                                 encoded), the user agreed:
//                                                 xdg-open, at most one every
//                                                 2 s per instance
//   launcher → coordinator   "launch <url>"       open url, site unknown yet
//   coordinator → instance   "show <url> [bounds=..] [back=..]
//                                  [handoff=<token>]"
//                                                 url is on your site (any
//                                                 site for the standard
//                                                 instance); with handoff,
//                                                 report "shown <token>" when
//                                                 its window is on screen
//                            "hide <n>"           hide the window that left
//                                                 with hand-off n: its
//                                                 replacement is shown
//                            "discard moderate|critical"
//                                                 drop your pages, keep the
//                                                 windows and their history
//                                                 (moderate: not pages with
//                                                 unsaved changes)
//                            "discard-background moderate|critical"
//                                                 the same, only for tabs
//                                                 not shown (the instance in
//                                                 use, after every other is
//                                                 discarded; the standard
//                                                 instance)
//                            "adblock-file <path>"  the filter lists are
//                                                 built: load the engine (you
//                                                 started without it)
//                            "close"              save your windows' history
//                                                 and exit (never sent to the
//                                                 standard instance)
//
// Launch switches it adds: --lrb-site (lean), --lrb-mode=standard,
// --lrb-handoff=<token> (the window replaces another site's),
// --lrb-latest-release=<version>, --lrb-resume, --lrb-adblock-file, ...
//
// Lean mode, leaving a site parks its instance: its windows hide once the
// other site's window is shown, and it stays alive for Back. At most
// --max-parked (default 3) are kept: one more, and the least recently
// active parked one is closed.
//
// Started without an address (the applications menu), it opens what the
// settings say: the sites last used (default), an empty window, or a page.
// Lean mode keeps <profiles-dir>/.last-site (the site in use and its
// address) and <profiles-dir>/.open-sites (sites with a window shown, the
// most recently active first): the last site opens with its windows as
// they were, then up to 6 others, one every 2 s. Standard mode starts its
// one instance with --lrb-resume, which restores every site's windows.
//
// Content blocking: windows get --lrb-adblock-file once the engine file
// exists ($XDG_DATA_HOME/lrb/adblock/<setting>.adb). When it is missing or
// over a day old, the coordinator runs `lrb --lrb-update-lists` (no window)
// to fetch the filter lists and compile it. The first window doesn't wait
// for it: instances started without the engine get "adblock-file" when the
// update finishes. The setting defaults to lean on machines with less than
// 1.5 GB of RAM, full otherwise.
//
// Phishing and malware warnings (lrb/common/safety_list.h): windows get
// --lrb-safety-file once $XDG_DATA_HOME/lrb/adblock/safety.adb exists. The
// coordinator refreshes it when it starts, if over an hour old (the lists
// change upstream twice a day), with the same updater, after the filter
// lists; never while it runs. Independent of the
// content-blocking setting; --safety-lists=off is for tests.
//
// New releases: at most once a day (by the file's age), after the list
// updates and never beside one, `lrb --lrb-check-release=<file>` writes the
// newest release's version to $XDG_DATA_HOME/lrb/release/latest-release;
// windows opened afterwards get it as --lrb-latest-release and offer the
// update when it is newer. --no-release-check is for tests.
//
// Under memory pressure it discards an instance's pages: parked instances
// first, then the least recently active, never the one in use (the most
// recently active with a window shown) nor the standard instance (only its
// background tabs sleep). One every 10 s while any budget is below
// --moderate-percent (default 30) of its limit or --moderate-max-mb
// (default 512), whichever is less; one every 3 s below --critical-percent
// (default 15) or --critical-max-mb (default 256). The same thresholds as
// the instances' own evaluator (patches/0001, 0004). Moderate keeps room
// for pages coming back: at 384 MB with critical alone, 3 of 5 windows were
// OOM-killed while the user went back through them. 0 disables a tier; a
// percent over 100 forces it (tests). Still critical with every other
// instance discarded, it closes one in the same order, which saves its
// history and is restored when its site is opened again.

#ifdef UNSAFE_BUFFERS_BUILD
// This file is the libc/syscall boundary (argv, sockets, fprintf) of a
// program that links nothing from Chromium, so base::span and friends are
// unavailable. Everything that parses untrusted input is in rules.cc, which
// keeps the check.
#pragma allow_unsafe_buffers
#endif

#include <dirent.h>
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
#include <cstdint>
#include <chrono>
#include <map>
#include <set>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lrb/coordinator/bookmarks.h"
#include "lrb/coordinator/broker.h"
#include "lrb/coordinator/confine.h"
#include "lrb/coordinator/keyring_broker.h"
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
  bool resume = false;  // lrb started on the last site: its windows as were
  // The window this one replaces ("handoff=<n>" from the instance on
  // handoff_fd): it hides once this one is on screen (Coordinator::Handoff).
  int handoff_fd = -1;
  std::string handoff;
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
  // Given on the command line: the settings' level doesn't apply.
  bool adblock_setting_given = false;
  bool adblock_file_given = false;
  bool safety_lists = true;
  std::string safety_file;
  std::string data_home;
  int moderate_percent = 30;
  int critical_percent = 15;
  // The thresholds' caps: on a large machine a percentage alone counts
  // gigabytes free as pressure (BelowPercent).
  int64_t moderate_max_mb = 512;
  int64_t critical_max_mb = 256;
  // Lean mode: instances kept alive with every window hidden (left for
  // another site, shown again by Back) before the oldest is closed.
  int max_parked = 3;
  // "lean", "standard", "auto" (lrb/common/mode.h); empty: the settings'.
  std::string mode;
  bool release_check = true;
  bool confine = true;
  std::string picker;  // lrb_picker; tests give a stand-in
  std::string keyring;  // lrb_keyring; tests give a stand-in
};

using Clock = std::chrono::steady_clock;

// Set by SIGTERM, SIGINT or SIGHUP (logout, Ctrl+C, a closed terminal).
volatile sig_atomic_t g_stop_signal = 0;

void OnStopSignal(int number) {
  g_stop_signal = number;
}

// No SA_RESTART: the signal interrupts poll(), so the loop sees it at once.
void CatchStopSignals() {
  struct sigaction action = {};
  action.sa_handler = OnStopSignal;
  sigemptyset(&action.sa_mask);
  for (int number : {SIGTERM, SIGINT, SIGHUP}) {
    sigaction(number, &action, nullptr);
  }
}

// How long instances get to exit after a stop signal before the
// coordinator leaves without them.
constexpr auto kStopGrace = std::chrono::seconds(5);

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
  // The standard instance (lrb/common/mode.h): every site, one profile.
  bool standard = false;
  // Every window hidden ("parked 1"): left for another site, kept for Back.
  bool parked = false;
  // Launched before the filter lists were built: told "adblock-file" once
  // they are.
  bool without_engine = false;
  // Live tabs not shown in their window (the instance reports them): what
  // can sleep in the instance in use before anything closes.
  int background_tabs = 0;
  // Its last link handed to another app (open-app): a compromised instance
  // can skip the user's question, so the coordinator spaces them out.
  std::optional<Clock::time_point> last_app_link;
};

std::string DirName(const std::string& path) {
  const size_t slash = path.rfind('/');
  return slash == std::string::npos ? "." : path.substr(0, slash);
}

std::string Getenv(const char* name) {
  const char* value = getenv(name);
  return value ? value : "";
}

// A small file's contents (at most `max` bytes), empty if unreadable.
std::string ReadSmallFile(const std::string& path, size_t max) {
  std::string contents;
  FILE* file = fopen(path.c_str(), "r");
  if (!file) {
    return contents;
  }
  char buffer[4096];
  size_t n;
  while (contents.size() < max &&
         (n = fread(buffer, 1, sizeof(buffer), file)) > 0) {
    contents.append(buffer, n);
  }
  fclose(file);
  return contents;
}

// Writes `contents` to `path` whole or not at all (a new file renamed over
// it).
bool WriteFileAtomically(const std::string& path, const std::string& contents) {
  const std::string temp = path + ".new";
  FILE* file = fopen(temp.c_str(), "w");
  if (!file) {
    return false;
  }
  const bool ok =
      fwrite(contents.data(), 1, contents.size(), file) == contents.size();
  if (fclose(file) != 0 || !ok || rename(temp.c_str(), path.c_str()) != 0) {
    unlink(temp.c_str());
    return false;
  }
  return true;
}

// A local X display (":0", ":1.0") as its socket's path. libxcb tries the
// X server's abstract socket first and falls back to the path only on
// ENOENT or ECONNREFUSED, but Landlock's scoping refuses abstract sockets
// with EPERM: a confined instance couldn't reach the X server. A DISPLAY
// that is a path (libxcb 1.14+) goes straight to the socket. Empty when
// `display` isn't local or has no socket.
std::string X11SocketPath(const std::string& display) {
  if (!display.starts_with(':')) {
    return "";
  }
  const std::string number = display.substr(1, display.find('.') - 1);
  const auto digit = [](char c) { return c >= '0' && c <= '9'; };
  if (number.empty() || !std::ranges::all_of(number, digit)) {
    return "";
  }
  const std::string path = "/tmp/.X11-unix/X" + number;
  struct stat st = {};
  return stat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode) ? path : "";
}

// Waits for `pid` until `deadline`, then kills it. Its exit status, or -1
// if it didn't exit with one in time.
int WaitFor(pid_t pid, Clock::time_point deadline) {
  int status = 0;
  while (true) {
    const pid_t done = waitpid(pid, &status, WNOHANG);
    if (done == pid) {
      return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    if (done < 0 && errno != EINTR) {
      return -1;
    }
    if (Clock::now() >= deadline) {
      kill(pid, SIGKILL);
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      return -1;
    }
    poll(nullptr, 0, 5);
  }
}

// A value in kB from /proc/meminfo's contents, in bytes; 0 if missing.
int64_t MeminfoBytes(const std::string& meminfo, const std::string& key) {
  for (size_t at = 0; at < meminfo.size();) {
    const size_t end = std::min(meminfo.find('\n', at), meminfo.size());
    if (meminfo.compare(at, key.size() + 1, key + ":") == 0) {
      return strtoll(meminfo.c_str() + at + key.size() + 1, nullptr, 10) * 1024;
    }
    at = end + 1;
  }
  return 0;
}

// A release's version as the release check writes it ("0.1.0-alpha.7"):
// passed on to instances' command lines, so nothing but digits, letters,
// dots and '-', at most 40 characters. Empty if `text` isn't one.
std::string ValidRelease(std::string text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == ' ' ||
                           text.back() == '\r' || text.back() == '\t')) {
    text.pop_back();
  }
  const bool valid =
      !text.empty() && text.size() <= 40 &&
      std::ranges::all_of(text, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
               (c >= 'A' && c <= 'Z') || c == '.' || c == '-';
      });
  return valid ? text : std::string();
}

bool AllDigits(std::string_view text, size_t max) {
  return !text.empty() && text.size() <= max &&
         std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; });
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
// lists, lrb's settings and the user's NSS database to read, and `rw_dir` (its profile) to write. Not
// the user's home: other sites' profiles, keys, documents. Files the user
// picks reach it through the coordinator (broker.h). `settings_writer`: the
// settings window's process (no web content), the one that may write lrb's
// settings.
coordinator::ConfinePolicy InstancePolicy(const Options& options,
                                          const std::string& rw_dir,
                                          bool settings_writer) {
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
      DirName(options.safety_file),
      xauthority.empty() ? home + "/.Xauthority" : xauthority,
      // Look and feel: GTK theme and settings, fonts, icons, cursors.
      config + "/gtk-3.0", config + "/gtk-4.0", config + "/fontconfig",
      config + "/dconf", config + "/pulse", config + "/lrb", data + "/fonts",
      data + "/icons",
      data + "/themes", home + "/.fonts", home + "/.icons", home + "/.themes",
      // The user's NSS database, shared with Chrome and Firefox: client
      // certificates and authorities the user added. Read only: a hijacked
      // page mustn't add an authority there. Chromium opens it read-write;
      // SQLite falls back to reading (checked under this policy).
      home + "/.pki/nssdb", data + "/pki/nssdb",
  };
  policy.read_write = {
      rw_dir,
      "/dev/shm", "/dev/null", "/dev/zero", "/dev/full", "/dev/random",
      "/dev/urandom", "/dev/dri", "/dev/snd",
      cache + "/fontconfig", cache + "/mesa_shader_cache",
      runtime + "/dconf",
  };
  for (int i = 0; i < 8; ++i) {  // cameras
    policy.read_write.push_back("/dev/video" + std::to_string(i));
  }
  if (settings_writer) {
    MakeDirs(config + "/lrb", 0700);
    policy.read_write.push_back(config + "/lrb");
  }
  return policy;
}

// The standard instance's policy: as InstancePolicy, and /proc writable:
// Chromium's sandbox writes a new user namespace's uid and gid maps
// (/proc/self/uid_map, sandbox/linux/services/credentials.cc). The kernel
// still limits a process to its own entries there.
coordinator::ConfinePolicy StandardPolicy(const Options& options,
                                          const std::string& rw_dir) {
  coordinator::ConfinePolicy policy =
      InstancePolicy(options, rw_dir, /*settings_writer=*/false);
  std::erase(policy.read, std::string("/proc"));
  policy.read_write.push_back("/proc");
  policy.allow_sandbox = true;
  return policy;
}

// lrb's settings file ($XDG_CONFIG_HOME/lrb/settings.json), empty if none.
// Written by the settings window's process only (lrb/common/settings.h).
std::string ReadSettings() {
  const std::string config = Getenv("XDG_CONFIG_HOME").empty()
                                 ? Getenv("HOME") + "/.config"
                                 : Getenv("XDG_CONFIG_HOME");
  std::string settings;
  if (FILE* file = fopen((config + "/lrb/settings.json").c_str(), "r")) {
    char buffer[65536];
    settings.assign(buffer, fread(buffer, 1, sizeof(buffer), file));
    fclose(file);
  }
  return settings;
}

// The content blocking level: the settings' choice, else by memory (lean
// below 1.5 GB of RAM).
std::string AdblockLevel() {
  const std::optional<std::string> chosen =
      JsonString(ReadSettings(), "content_blocking");
  if (chosen && (*chosen == "full" || *chosen == "lean" || *chosen == "off")) {
    return *chosen;
  }
  struct sysinfo info = {};
  const uint64_t ram =
      sysinfo(&info) == 0 ? uint64_t{info.totalram} * info.mem_unit : 0;
  return ram && ram < uint64_t{1536} * 1024 * 1024 ? "lean" : "full";
}

bool ParseOptions(int argc, char** argv, Options& options) {
  std::string self = argv[0];
  options.browser = DirName(self) + "/lrb";
  const std::string data_home = !Getenv("XDG_DATA_HOME").empty()
                                    ? Getenv("XDG_DATA_HOME")
                                    : Getenv("HOME") + "/.local/share";
  options.profiles_dir = data_home + "/lrb/sites";
  options.data_home = data_home;
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
      options.adblock_setting_given = true;
    } else if (arg.starts_with("--adblock-file=")) {
      options.adblock_file = arg.substr(15);
      options.adblock_file_given = true;
    } else if (arg == "--safety-lists=on" || arg == "--safety-lists=off") {
      options.safety_lists = arg == "--safety-lists=on";
    } else if (arg == "--verbose") {
      options.verbose = true;
    } else if (arg == "--no-confine") {
      options.confine = false;
    } else if (arg.starts_with("--picker=")) {
      options.picker = arg.substr(9);
    } else if (arg.starts_with("--keyring=")) {
      options.keyring = arg.substr(10);
    } else if (arg.starts_with("--moderate-percent=")) {
      options.moderate_percent = atoi(std::string(arg.substr(19)).c_str());
    } else if (arg.starts_with("--critical-percent=")) {
      options.critical_percent = atoi(std::string(arg.substr(19)).c_str());
    } else if (arg.starts_with("--moderate-max-mb=")) {
      options.moderate_max_mb = atoll(std::string(arg.substr(18)).c_str());
    } else if (arg.starts_with("--critical-max-mb=")) {
      options.critical_max_mb = atoll(std::string(arg.substr(18)).c_str());
    } else if (arg.starts_with("--max-parked=")) {
      options.max_parked =
          std::max(0, atoi(std::string(arg.substr(13)).c_str()));
    } else if (arg == "--mode=auto" || arg == "--mode=lean" ||
               arg == "--mode=standard") {
      options.mode = arg.substr(7);
    } else if (arg == "--no-release-check") {
      options.release_check = false;
    } else if (!arg.starts_with("-") && options.url.empty()) {
      options.url = arg;
    } else {
      fprintf(stderr, "lrb_coordinator: unknown argument %s\n", argv[i]);
      return false;
    }
  }
  // Native Wayland windows on a Wayland desktop:
  // through Xwayland, without GTK (use_gtk = false), Chromium's X11 backend
  // composes no dead keys and reaches no input method, so á, ü and the
  // like couldn't be typed. Ozone Wayland composes with xkbcommon and talks
  // to input methods itself. An --ozone-platform after "--" still decides
  // (the X11 tests ask for x11).
  bool platform_given = false;
  for (const std::string& arg : options.browser_args) {
    platform_given = platform_given || arg.starts_with("--ozone-platform=");
  }
  if (!platform_given && !Getenv("WAYLAND_DISPLAY").empty()) {
    options.browser_args.emplace_back("--ozone-platform=wayland");
  }
  if (options.adblock_setting.empty()) {
    options.adblock_setting = AdblockLevel();
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
  options.safety_file = data_home + "/lrb/adblock/safety.adb";
  // Longer would be cut short silently (sockaddr_un), and the next start
  // couldn't find or replace the socket.
  if (options.socket_path.size() >= sizeof(sockaddr_un{}.sun_path)) {
    fprintf(stderr, "lrb_coordinator: socket path too long (%zu bytes): %s\n",
            options.socket_path.size(), options.socket_path.c_str());
    return false;
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
        adblock_setting_(options_.adblock_setting),
        adblock_file_(options_.adblock_file),
        standard_profile_(DirName(options_.profiles_dir) + "/standard"),
        release_file_(options_.data_home + "/lrb/release/latest-release"),
        broker_(options_.picker.empty()
                    ? DirName(options_.browser) + "/lrb_picker"
                    : options_.picker,
                UserDownloadsDir(),
                [this](int fd, const std::string& line) { Send(fd, line); },
                // Read for each download: absent or true asks (the default).
                [] {
                  return JsonBool(ReadSettings(), "ask_downloads")
                      .value_or(true);
                }),
        keyring_(options_.keyring.empty()
                     ? DirName(options_.browser) + "/lrb_keyring"
                     : options_.keyring,
                 [this](int fd, const std::string& line) { Send(fd, line); }) {}

  int Run() {
    if (!Listen()) {
      return 1;
    }
    DecideMode();
    latest_release_ = ValidRelease(ReadSmallFile(release_file_, 64));
    MaybeUpdateLists();
    LaunchFirst();
    CatchStopSignals();
    // Also while an updater (filter lists, safety lists, the release check)
    // runs, and until the safety lists have had their turn after the filter
    // lists: with no window open yet the first start must still finish them.
    while (children_ > 0 || !clients_.empty() || updater_pid_ > 0 ||
           !safety_list_checked_) {
      if (g_stop_signal) {
        Stop();
        break;
      }
      PollOnce();
      Reap();
      CheckHandoffs();
      CheckPressure();
      RestoreNext();
      MaybeUpdateLists();
    }
    unlink(options_.socket_path.c_str());
    return 0;
  }

 private:
  // A window leaving for another site ("open ... handoff=<n>" from the
  // requester): it stays on screen until the target site's window is
  // ("shown <token>" from the target), then it hides ("hide <n>"), so the
  // user sees one window replace the other, never a gap. Tokens sent to
  // targets are the coordinator's own (instances count theirs from 1 each,
  // so two could collide); the requester gets its own back.
  struct Handoff {
    int requester_fd = -1;
    std::string token;  // the requester's
    std::string site;   // the target
    Clock::time_point deadline;
  };
  // How long a target gets to report its window shown before the leaving
  // window hides anyway (if the target is running at all).
  static constexpr auto kHandoffTimeout = std::chrono::seconds(8);

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

  // Lean or standard for this start (lrb/common/mode.h): --mode, else the
  // settings' "mode", else automatic: standard with at least 3 GiB of RAM,
  // 1.5 GiB of it free now, and Chromium's sandbox able to run under our
  // confinement. A forced standard still needs the sandbox: without it every
  // renderer would abort ("No usable sandbox!"), so lean it is then.
  void DecideMode() {
    std::string chosen = options_.mode;
    std::string source = "--mode";
    if (chosen.empty()) {
      chosen = JsonString(ReadSettings(), "mode").value_or("");
      source = "the settings";
    }
    if (chosen == "lean") {
      if (options_.verbose) {
        Log("lean mode (%s)", source.c_str());
      }
      return;
    }
    if (chosen == "standard") {
      if (SandboxCheck()) {
        standard_ = true;
        Log("standard mode (%s)", source.c_str());
      } else {
        Log("standard mode refused (%s asked for it): Chromium's sandbox "
            "can't create a user namespace here (AppArmor on Ubuntu 23.10+: "
            "docs/building.md); lean mode",
            source.c_str());
      }
      return;
    }
    constexpr double kGiB = 1024.0 * 1024 * 1024;
    const std::string meminfo = ReadSmallFile("/proc/meminfo", 16384);
    const double total = MeminfoBytes(meminfo, "MemTotal") / kGiB;
    const double available = MeminfoBytes(meminfo, "MemAvailable") / kGiB;
    if (total < 3) {
      Log("lean mode: %.1f GiB of RAM (standard needs 3)", total);
    } else if (available < 1.5) {
      Log("lean mode: %.1f GiB free at start (standard needs 1.5)", available);
    } else if (!SandboxCheck()) {
      Log("lean mode: Chromium's sandbox can't create a user namespace here "
          "(AppArmor on Ubuntu 23.10+: docs/building.md)");
    } else {
      standard_ = true;
      Log("standard mode: %.1f GiB of RAM, %.1f GiB free", total, available);
    }
  }

  // Whether a standard instance's sandbox could run: `lrb
  // --lrb-sandbox-check` under the standard instance's confinement (AppArmor
  // restricts user namespaces per binary, seccomp ours), exit 0 within 1 s.
  bool SandboxCheck() {
    MakeDirs(standard_profile_, 0700);
    const pid_t pid = fork();
    if (pid == 0) {
      ConfineChild(standard_profile_, /*settings_writer=*/false,
                   /*allow_sandbox=*/true, options_.browser);
      execl(options_.browser.c_str(), options_.browser.c_str(),
            "--lrb-sandbox-check", nullptr);
      _exit(127);
    }
    if (pid < 0) {
      return false;
    }
    const int status = WaitFor(pid, Clock::now() + std::chrono::seconds(1));
    if (options_.verbose) {
      Log("sandbox check: exit %d", status);
    }
    return status == 0;
  }

  // The site of `url` from `lrb --lrb-site-of=<url>` (the same rule the
  // instances use, without starting a browser), within 1 s; empty if it has
  // none or the answer didn't come.
  std::string SiteOf(const std::string& url) const {
    int out[2];
    if (pipe2(out, O_CLOEXEC) != 0) {
      return std::string();
    }
    const std::string arg = "--lrb-site-of=" + url;
    const pid_t pid = fork();
    if (pid == 0) {
      dup2(out[1], STDOUT_FILENO);  // the copy is inherited across exec
      execl(options_.browser.c_str(), options_.browser.c_str(), arg.c_str(),
            nullptr);
      _exit(127);
    }
    close(out[1]);
    if (pid < 0) {
      close(out[0]);
      return std::string();
    }
    const Clock::time_point deadline = Clock::now() + std::chrono::seconds(1);
    std::string output;
    while (output.size() < 512) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                            deadline - Clock::now())
                            .count();
      pollfd readable = {out[0], POLLIN, 0};
      const int ready = left > 0 ? poll(&readable, 1, static_cast<int>(left)) : 0;
      if (ready < 0 && errno == EINTR) {
        continue;
      }
      if (ready <= 0) {
        break;
      }
      char buffer[256];
      const ssize_t n = read(out[0], buffer, sizeof(buffer));
      if (n < 0 && errno == EINTR) {
        continue;
      }
      if (n <= 0) {
        break;
      }
      output.append(buffer, static_cast<size_t>(n));
    }
    close(out[0]);
    if (WaitFor(pid, deadline) != 0) {
      return std::string();
    }
    while (!output.empty() && (output.back() == '\n' || output.back() == ' ')) {
      output.pop_back();
    }
    return IsValidSite(output) && UrlBelongsToSite(url, output) ? output
                                                               : std::string();
  }

  // A line to an instance, if it's still connected (the brokers' answers).
  void Send(int fd, const std::string& line) {
    if (fd >= 0 && clients_.contains(fd) && !WriteAll(fd, line + "\n")) {
      Drop(fd);
    }
  }

  // A site's instance or the standard one: what pressure, parking and the
  // session apply to (not the resolver).
  static bool Managed(const Client& client) {
    return !client.site.empty() || client.standard;
  }

  Client* StandardClient() {
    for (auto& [fd, client] : clients_) {
      if (client.standard && !client.closing) {
        return &client;
      }
    }
    return nullptr;
  }

  void Accept() {
    const int fd = accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (fd < 0) {
      return;
    }
    Client& client = clients_[fd];
    client.fd = fd;
    ucred peer = {};
    socklen_t size = sizeof(peer);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 ||
        !child_profiles_.contains(peer.pid)) {
      return;
    }
    client.profile = child_profiles_.at(peer.pid);
    client.standard = standard_ && client.profile == standard_profile_;
    if (without_engine_.erase(peer.pid)) {
      struct stat engine = {};
      if (stat(adblock_file_.c_str(), &engine) == 0) {
        // Built while the instance started.
        if (!WriteAll(fd, "adblock-file " + adblock_file_ + "\n")) {
          Drop(fd);
          return;
        }
      } else {
        client.without_engine = true;
      }
    }
    if (client.standard) {
      // Addresses opened while it was starting.
      std::vector<std::string> urls = std::move(pending_standard_urls_);
      pending_standard_urls_.clear();
      for (const std::string& url : urls) {
        if (!WriteAll(fd, "show " + url + "\n")) {
          Drop(fd);
          return;
        }
      }
    }
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
    const size_t first_keyring = fds.size();
    for (int fd : keyring_.fds()) {
      fds.push_back({fd, POLLIN, 0});
    }
    // Wakes every 250 ms to check memory pressure and reap instances.
    if (poll(fds.data(), fds.size(), 250) <= 0) {
      return;
    }
    if (fds[0].revents & POLLIN) {
      Accept();
    }
    for (size_t i = 1; i < fds.size(); ++i) {
      if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
        if (i < first_picker) {
          ReadFrom(fds[i].fd);
        } else if (i < first_keyring) {
          broker_.OnReadable(fds[i].fd);  // a picker answered
        } else {
          keyring_.OnReadable(fds[i].fd);  // the keyring answered
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
    clients_.at(fd).input.append(buffer, static_cast<size_t>(n));
    // Looked up for each line: handling one may drop the instance.
    while (clients_.contains(fd)) {
      Client& client = clients_.at(fd);
      const size_t newline = client.input.find('\n');
      if (newline == std::string::npos) {
        if (client.input.size() > kMaxLine) {
          Log("dropping instance: line too long");
          Drop(fd);
        }
        return;
      }
      const std::string line = client.input.substr(0, newline);
      client.input.erase(0, newline + 1);
      Handle(client, line);
    }
  }

  // A link for another app (mailto:, tel:, zoommtg:, ...) to the desktop's
  // handler. The instance asked the user first; here only what can't be
  // left to an instance: the link is one for an app (never http(s), files,
  // data: or javascript:; rules.h), the instance is one this coordinator
  // started, and links come at most one every 2 s.
  void OpenInApp(Client& client, const std::string& url) {
    const std::string scheme = ExternalAppScheme(url);
    if (scheme.empty() || client.profile.empty()) {
      Log("refused open-app from %s",
          client.site.empty() ? "an instance without a site"
                              : client.site.c_str());
      return;
    }
    const Clock::time_point now = Clock::now();
    if (client.last_app_link &&
        now - *client.last_app_link < std::chrono::seconds(2)) {
      Log("refused open-app from %s: too soon after the last",
          client.site.c_str());
      return;
    }
    client.last_app_link = now;
    Log("opening a %s: link from %s in its app", scheme.c_str(),
        client.site.c_str());
    if (fork() == 0) {
      setsid();
      execlp("xdg-open", "xdg-open", url.c_str(), nullptr);
      _exit(127);
    }
  }

  void Drop(int fd) {
    broker_.Forget(fd);
    keyring_.Forget(fd);
    close(fd);
    clients_.erase(fd);
    std::erase_if(handoffs_, [fd](const auto& entry) {
      return entry.second.requester_fd == fd;
    });
    UpdateOpenSites();
  }

  void Handle(Client& client, const std::string& line) {
    const size_t space = line.find(' ');
    const std::string command = line.substr(0, space);
    const std::string args =
        space == std::string::npos ? "" : line.substr(space + 1);

    if (options_.verbose && command != "active") {
      Log("fd %d (%s): %s", client.fd,
          client.standard     ? "standard"
          : client.site.empty() ? "no site"
                                : client.site.c_str(),
          line.c_str());
    }
    if (command == "open-settings") {
      OpenSettings();
    } else if (command == "open-app") {
      OpenInApp(client, FileBroker::Decode(args));
    } else if (command.starts_with("pw-") || command.starts_with("af-")) {
      // The standard instance names each password's site itself
      // (keyring_broker.h): Chromium's site isolation, not a process per
      // site, keeps sites apart there.
      keyring_.Handle(client.fd,
                      client.standard ? "*" : SiteOfProfile(client.profile),
                      command, args);
    } else if (command.starts_with("pick-") || command.starts_with("save-") ||
               command == "show-saved") {
      broker_.Handle(client.fd, client.profile, command, args);
    } else if (command == "active") {
      const bool reloading = client.discarded;
      client.last_active = Clock::now();
      RecordLastSite(client.site);
      client.discarded = false;
      UpdateOpenSites();
      if (reloading) {
        MakeRoom(&client);  // its page is coming back
      }
    } else if (command == "background-tabs") {
      client.background_tabs = std::max(0, atoi(args.c_str()));
    } else if (command == "parked") {
      if ((args == "0" || args == "1") && client.parked != (args == "1")) {
        client.parked = args == "1";
        UpdateOpenSites();
        if (client.parked) {
          LimitParked();
        }
      }
    } else if (command == "shown") {
      OnShown(client, args);
    } else if (command == "site") {
      // An instance's site is set once: by --lrb-site, or by its first page.
      if (client.site.empty() && !client.standard && IsValidSite(args)) {
        client.site = args;
        if (client.site == wanted_site_) {
          // Launched because the user opened this site: it's in use now.
          client.last_active = Clock::now();
          wanted_site_.clear();
          RecordLastSite(client.site);
        }
        UpdateOpenSites();
      }
    } else if (command == "open") {
      HandleOpen(client, args);
    } else if (command == "launch") {
      if (IsAcceptableUrl(args)) {
        LaunchUrl(args);
      }
    } else if (command == "suggest" || command == "bookmarks" ||
               command == "is-bookmarked" || command == "bookmark-add" ||
               command == "bookmark-remove") {
      HandleBookmarks(client, command, args);
    }
  }

  // "open <site> <url> [bounds=<x,y,w,h>] [back=<url>] [restore]
  // [handoff=<n>]"
  void HandleOpen(Client& client, const std::string& args) {
    std::vector<std::string> parts;
    for (size_t start = 0; start <= args.size();) {
      const size_t end = std::min(args.find(' ', start), args.size());
      parts.push_back(args.substr(start, end - start));
      start = end + 1;
    }
    // The standard instance holds every site: no site to keep to.
    if (parts.size() < 2 ||
        !(standard_ ? IsAcceptableUrl(parts[1])
                    : UrlBelongsToSite(parts[1], parts[0]))) {
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
      } else if (part.starts_with("back=") && IsAcceptableUrl(part.substr(5))) {
        open.back = std::string(part.substr(5));
      } else if (part == "restore") {
        open.restore = true;
      } else if (part.starts_with("handoff=") &&
                 AllDigits(part.substr(8), 18)) {
        open.handoff_fd = client.fd;
        open.handoff = std::string(part.substr(8));
      } else {
        Log("refused open %s", args.c_str());
        return;
      }
    }
    Open(parts[0], parts[1], open);
  }

  // "shown <token>": the target of a hand-off has its window on screen, so
  // the window it replaces hides. Only from the target site's instance.
  void OnShown(const Client& client, const std::string& args) {
    auto entry = AllDigits(args, 18)
                     ? handoffs_.find(strtoull(args.c_str(), nullptr, 10))
                     : handoffs_.end();
    if (entry == handoffs_.end() || client.site != entry->second.site) {
      Log("refused shown %s", args.c_str());
      return;
    }
    const Handoff handoff = entry->second;
    handoffs_.erase(entry);
    Send(handoff.requester_fd, "hide " + handoff.token);
  }

  // Hand-offs whose target never said "shown": the leaving window hides
  // anyway if the target is running (it may paint late, or never report),
  // and stays if not (the target failed: better a stale window than none).
  void CheckHandoffs() {
    const Clock::time_point now = Clock::now();
    std::vector<Handoff> expired;
    for (auto entry = handoffs_.begin(); entry != handoffs_.end();) {
      if (now < entry->second.deadline) {
        ++entry;
        continue;
      }
      expired.push_back(entry->second);
      entry = handoffs_.erase(entry);
    }
    for (const Handoff& handoff : expired) {
      const bool running = std::ranges::any_of(clients_, [&](const auto& c) {
        return c.second.site == handoff.site && !c.second.closing;
      });
      if (running) {
        Log("%s didn't report its window shown in time; hiding the one left",
            handoff.site.c_str());
        Send(handoff.requester_fd, "hide " + handoff.token);
      }
    }
  }

  // Registers the hand-off `open` asks for, to `site`: the token to give
  // the target, 0 if none.
  uint64_t StartHandoff(const std::string& site, const OpenOptions& open) {
    if (open.handoff_fd < 0) {
      return 0;
    }
    const uint64_t token = next_handoff_++;
    handoffs_[token] = {open.handoff_fd, open.handoff, site,
                        Clock::now() + kHandoffTimeout};
    return token;
  }

  // Bookmarks and suggestions (lrb/coordinator/bookmarks.h). Read from the
  // file for each request (a keystroke's worth of work): nothing is kept in
  // the coordinator's memory.
  void HandleBookmarks(Client& client,
                       const std::string& command,
                       const std::string& args) {
    const size_t space = args.find(' ');
    const std::string first = args.substr(0, space);
    const std::string rest =
        space == std::string::npos ? "" : args.substr(space + 1);
    auto valid_id = [](const std::string& id) {
      return !id.empty() && id.size() <= 20 &&
             id.find_first_not_of("0123456789") == std::string::npos;
    };
    std::string reply;
    if (command == "suggest" && valid_id(first)) {
      constexpr size_t kMaxSuggestions = 8;
      for (const Suggestion& suggestion :
           Suggest(DecodeText(rest), ReadBookmarks(), VisitedSites(),
                   kMaxSuggestions)) {
        reply += "suggestion " + first + (suggestion.bookmark ? " b " : " s ") +
                 suggestion.url + " " + EncodeText(suggestion.title) + "\n";
      }
      reply += "suggestions-done " + first + "\n";
    } else if (command == "bookmarks" && valid_id(first)) {
      for (const Bookmark& bookmark : ReadBookmarks()) {
        reply += "bookmark-item " + first + " " + bookmark.url + " " +
                 EncodeText(bookmark.title) + "\n";
      }
      reply += "bookmarks-done " + first + "\n";
    } else if (command == "is-bookmarked" && valid_id(first)) {
      bool found = false;
      for (const Bookmark& bookmark : ReadBookmarks()) {
        found = found || bookmark.url == rest;
      }
      reply = "is-bookmarked " + first + (found ? " 1\n" : " 0\n");
    } else if (command == "bookmark-add" || command == "bookmark-remove") {
      // Only a page of the instance's own site, by the profile this
      // coordinator started it with (never what it says about itself). The
      // standard instance holds every site, trusted as Chrome's browser
      // process is: any http(s) page.
      const std::string site = SiteOfProfile(client.profile);
      if ((!client.standard && site.empty()) || !IsAcceptableUrl(first) ||
          first.size() > kMaxBookmarkUrl ||
          (!client.standard && !UrlBelongsToSite(first, site))) {
        Log("refused %s %s", command.c_str(), first.c_str());
        return;
      }
      std::vector<Bookmark> bookmarks = ReadBookmarks();
      std::erase_if(bookmarks,
                    [&](const Bookmark& b) { return b.url == first; });
      if (command == "bookmark-add") {
        bookmarks.push_back({first, CleanTitle(DecodeText(rest))});
      }
      const std::string file = SerializeBookmarks(bookmarks);
      if (bookmarks.size() > kMaxBookmarks || file.size() > kMaxBookmarksBytes) {
        Log("refused bookmark-add: %zu bookmarks, %zu bytes", bookmarks.size(),
            file.size());
        return;
      }
      if (!WriteFileAtomically(BookmarksPath(), file)) {
        Log("can't write %s: %s", BookmarksPath().c_str(), strerror(errno));
      }
      return;
    } else {
      return;
    }
    if (!WriteAll(client.fd, reply)) {
      Drop(client.fd);
    }
  }

  std::string BookmarksPath() const {
    return options_.profiles_dir + "/.bookmarks";
  }

  std::vector<Bookmark> ReadBookmarks() const {
    return ParseBookmarks(ReadSmallFile(BookmarksPath(), kMaxBookmarksBytes));
  }

  // The site whose profile `profile` is, empty if it's no site's (the
  // resolver's, the settings window's, the standard instance's).
  std::string SiteOfProfile(const std::string& profile) const {
    const std::string prefix = options_.profiles_dir + "/";
    if (!profile.starts_with(prefix)) {
      return std::string();
    }
    const std::string name = profile.substr(prefix.size());
    return IsValidSite(name) ? name : std::string();
  }

  // Sites with a profile, the most recently used first (a profile's
  // directory changes as its windows are saved).
  std::vector<std::string> VisitedSites() const {
    std::vector<std::pair<time_t, std::string>> found;
    if (DIR* dir = opendir(options_.profiles_dir.c_str())) {
      while (const dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        struct stat info = {};
        if (name.starts_with(".") || !IsValidSite(name) ||
            stat((options_.profiles_dir + "/" + name).c_str(), &info) != 0 ||
            !S_ISDIR(info.st_mode)) {
          continue;
        }
        found.push_back({info.st_mtime, name});
      }
      closedir(dir);
    }
    std::sort(found.begin(), found.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<std::string> sites;
    for (auto& [time, name] : found) {
      sites.push_back(std::move(name));
    }
    return sites;
  }

  // An address from outside (the launcher, the command line, the startup
  // page): its site from `lrb --lrb-site-of`, then that site's window. If
  // that fails, the resolver instance finds the site by loading the page.
  void LaunchUrl(const std::string& url) {
    if (standard_) {
      ShowStandard(url, OpenOptions());
      return;
    }
    const std::string site = SiteOf(url);
    if (!site.empty()) {
      Open(site, url, OpenOptions());
      return;
    }
    Log("no site for %s from lrb --lrb-site-of; resolving it in a browser",
        url.c_str());
    Launch(/*site=*/"", url);
  }

  // Standard mode: every address goes to the one instance, launched if it
  // isn't running.
  void ShowStandard(const std::string& url, const OpenOptions& open) {
    if (Client* client = StandardClient()) {
      if (url.empty()) {
        return;
      }
      std::string line = "show " + url;
      if (!open.bounds.empty()) {
        line += " bounds=" + open.bounds;
      }
      if (!open.back.empty()) {
        line += " back=" + open.back;
      }
      Send(client->fd, line);
      return;
    }
    if (standard_pid_ > 0) {
      // Started, not connected yet: shown once it is (Accept).
      if (!url.empty()) {
        pending_standard_urls_.push_back(url);
      }
      return;
    }
    MakeRoom(nullptr);
    Launch(/*site=*/"", url, open);
  }

  // Shows `url` in `site`'s window, launching the instance if it has none.
  void Open(const std::string& site,
            const std::string& url,
            const OpenOptions& open) {
    if (standard_) {
      ShowStandard(url, open);
      return;
    }
    site_urls_[site] = url;
    const uint64_t handoff = StartHandoff(site, open);
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
        if (handoff) {
          line += " handoff=" + std::to_string(handoff);
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
    Launch(site, url, open, handoff);
  }

  // Starts an instance: the standard one in standard mode, else `site`'s, or
  // with no site the resolver. `handoff`: the coordinator's token for the
  // window it replaces (0: none).
  void Launch(const std::string& site,
              const std::string& url,
              const OpenOptions& open = OpenOptions(),
              uint64_t handoff = 0) {
    std::vector<std::string> args = {options_.browser,
                                     "--lrb-coordinator=" + options_.socket_path,
                                     "--lrb-profiles-dir=" + options_.profiles_dir};
    std::string profile;
    if (standard_) {
      // Every site in one browser with one profile, beside the sites'.
      profile = standard_profile_;
      MakeDirs(profile, 0700);
      args.push_back("--user-data-dir=" + profile);
      args.push_back("--lrb-mode=standard");
    } else if (!site.empty()) {
      // Each site keeps its own profile across restarts.
      profile = options_.profiles_dir + "/" + site;
      MakeDirs(profile, 0700);
      args.push_back("--user-data-dir=" + profile);
      args.push_back("--lrb-site=" + site);
    } else {
      // Site unknown (lrb --lrb-site-of failed, or a blank window): this
      // instance only resolves the site. It sends its first page back with
      // "open <site> <url>" and exits, so the page opens with the right
      // profile. Its own profile is never used for browsing.
      profile = options_.profiles_dir + "/.resolver";
      MakeDirs(profile, 0700);
      args.push_back("--user-data-dir=" + profile);
    }
    struct stat engine = {};
    const bool has_engine = adblock_setting_ != "off" &&
                            stat(adblock_file_.c_str(), &engine) == 0;
    if (has_engine) {
      args.push_back("--lrb-adblock-file=" + adblock_file_);
    }
    struct stat safety = {};
    if (options_.safety_lists &&
        stat(options_.safety_file.c_str(), &safety) == 0) {
      args.push_back("--lrb-safety-file=" + options_.safety_file);
    }
    if (!latest_release_.empty()) {
      args.push_back("--lrb-latest-release=" + latest_release_);
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
    if (open.resume) {
      args.push_back("--lrb-resume");
    }
    if (handoff) {
      args.push_back("--lrb-handoff=" + std::to_string(handoff));
    }
    if (!url.empty()) {
      args.push_back(url);
    }

    const pid_t pid = Spawn(std::move(args), profile, /*settings_writer=*/false,
                            /*allow_sandbox=*/standard_);
    if (pid > 0) {
      ++children_;
      if (standard_) {
        standard_pid_ = pid;
      }
      if ((standard_ || !site.empty()) && !has_engine &&
          adblock_setting_ != "off") {
        without_engine_.insert(pid);
      }
    }
  }

  // The instance in use: the most recently active one with a window shown.
  Client* InUse() {
    Client* in_use = nullptr;
    for (auto& [fd, client] : clients_) {
      if (Managed(client) && !client.parked && !client.closing &&
          (!in_use || client.last_active > in_use->last_active)) {
        in_use = &client;
      }
    }
    return in_use;
  }

  // The next instance to discard (or with `to_close`, to close): parked
  // ones first (nothing of theirs is on screen), then the least recently
  // active. Never the one in use, `keep`, or the standard instance (it
  // holds every site; only its background tabs sleep).
  Client* PickVictim(const Client* keep, bool to_close) {
    const Client* in_use = InUse();
    Client* victim = nullptr;
    for (auto& [fd, client] : clients_) {
      if (!Managed(client) || client.standard || client.closing ||
          &client == keep || &client == in_use ||
          (!to_close && client.discarded)) {
        continue;
      }
      if (!victim ||
          std::pair(!client.parked, client.last_active) <
              std::pair(!victim->parked, victim->last_active)) {
        victim = &client;
      }
    }
    return victim;
  }

  bool Critical(const std::vector<Budget>& budgets) const {
    return BelowPercent(budgets, options_.critical_percent,
                        options_.critical_max_mb * 1024 * 1024);
  }

  bool Moderate(const std::vector<Budget>& budgets) const {
    return BelowPercent(budgets, options_.moderate_percent,
                        options_.moderate_max_mb * 1024 * 1024);
  }

  // Lean mode keeps at most --max-parked instances with every window hidden:
  // one more, and the least recently active of them closes (it saves its
  // windows; opening its site again restores them).
  void LimitParked() {
    int parked = 0;
    Client* oldest = nullptr;
    for (auto& [fd, client] : clients_) {
      if (Managed(client) && !client.standard && client.parked &&
          !client.closing) {
        ++parked;
        if (!oldest || client.last_active < oldest->last_active) {
          oldest = &client;
        }
      }
    }
    if (parked <= options_.max_parked || !oldest) {
      return;
    }
    Log("%d parked instances (at most %d), closing %s", parked,
        options_.max_parked, oldest->site.c_str());
    oldest->closing = true;
    Send(oldest->fd, "close");
  }

  // Before a known page load (a site opened, a discarded page coming back):
  // a load can take 100 MB in a second, faster than reacting to pressure
  // (measured at 384 MB: 15% left to an OOM kill in 1.1 s), and a discard
  // frees memory over 1-11 s. So below the moderate threshold, drop another
  // page now (PickVictim), ignoring the rate limit; with nothing left to
  // discard and critical, close one. `keep` is the instance about to load
  // (null for one not launched yet).
  void MakeRoom(Client* keep) {
    const std::vector<Budget> budgets = ReadBudgets();
    if (!Moderate(budgets)) {
      return;
    }
    const bool critical = Critical(budgets);
    const std::string level = critical ? "critical" : "moderate";
    const Clock::time_point now = Clock::now();
    if (Client* victim = PickVictim(keep, /*to_close=*/false)) {
      Log("making room for a page load, discarding %s (%s)",
          victim->site.c_str(), level.c_str());
      last_discard_ = now;
      victim->discarded = true;
      Send(victim->fd, "discard " + level);
    } else if (keep && keep->background_tabs > 0) {
      // The instance loading a page has background tabs: they sleep first.
      Log("making room for a page load, sleeping background tabs of %s",
          keep->standard ? "the standard instance" : keep->site.c_str());
      last_discard_ = now;
      keep->background_tabs = 0;
      Send(keep->fd, "discard-background " + level);
    } else if (critical) {
      if (Client* oldest = PickVictim(keep, /*to_close=*/true)) {
        Log("making room for a page load, closing %s", oldest->site.c_str());
        last_discard_ = now;
        oldest->closing = true;
        Send(oldest->fd, "close");
      }
    }
  }

  // Under pressure, drops the pages of the next victim (PickVictim).
  void CheckPressure() {
    const Clock::time_point now = Clock::now();
    if (now - last_discard_ < std::chrono::seconds(3)) {
      return;
    }
    const std::vector<Budget> budgets = ReadBudgets();
    const bool critical = Critical(budgets);
    const bool moderate = Moderate(budgets);
    if (!critical &&
        !(moderate && now - last_discard_ >= std::chrono::seconds(10))) {
      return;
    }
    const std::string level = critical ? "critical" : "moderate";
    Client* in_use = InUse();
    Client* victim = PickVictim(nullptr, /*to_close=*/false);
    if (!victim) {
      // Every other instance is discarded: the background tabs of the one
      // in use (or the standard instance's) sleep before anything closes.
      Client* sleeper = in_use ? in_use : StandardClient();
      if (sleeper && sleeper->background_tabs > 0) {
        Log("%s memory pressure, sleeping %d background tab(s) of %s",
            level.c_str(), sleeper->background_tabs,
            sleeper->standard ? "the standard instance"
                              : sleeper->site.c_str());
        last_discard_ = now;
        sleeper->background_tabs = 0;  // it reports anew
        Send(sleeper->fd, "discard-background " + level);
        return;
      }
      if (critical) {
        CloseLeastRecentlyUsed(in_use, now);
      }
      return;
    }
    Log("%s memory pressure, discarding %s%s", level.c_str(),
        victim->site.c_str(), victim->parked ? " (parked)" : "");
    LogWindows(in_use);
    last_discard_ = now;
    victim->discarded = true;
    Send(victim->fd, "discard " + level);
  }

  // With --verbose: every instance's state.
  void LogWindows(const Client* in_use, bool always = false) {
    if (!options_.verbose && !always) {
      return;
    }
    const Clock::time_point now = Clock::now();
    for (const auto& [fd, client] : clients_) {
      Log("  fd %d %s%s%s%s%s, %d background tab(s), last active %.1f s ago",
          fd,
          client.standard       ? "(standard)"
          : client.site.empty() ? "(no site)"
                                : client.site.c_str(),
          &client == in_use ? " [in use]" : "",
          client.parked ? " [parked]" : "",
          client.discarded ? " [discarded]" : "",
          client.closing ? " [closing]" : "", client.background_tabs,
          std::chrono::duration<double>(now - client.last_active).count());
    }
  }

  // Last resort: every instance but the one in use is already discarded and
  // memory is still critical.
  void CloseLeastRecentlyUsed(Client* in_use, Clock::time_point now) {
    Client* victim = PickVictim(nullptr, /*to_close=*/true);
    if (!victim) {
      // Only the window in use is left. Say so now and then: it explains a
      // squeeze the coordinator can't relieve.
      if (now - last_stuck_report_ >= std::chrono::seconds(10)) {
        last_stuck_report_ = now;
        Log("critical memory pressure, nothing left to discard or close");
        LogWindows(in_use, /*always=*/true);
      }
      return;
    }
    Log("critical memory pressure, closing %s%s", victim->site.c_str(),
        victim->parked ? " (parked)" : "");
    LogWindows(in_use);
    last_discard_ = now;
    victim->closing = true;
    Send(victim->fd, "close");
  }

  // Runs the filter-list updater when the engine file is missing or older
  // than a day, checked at most hourly; then, once per start, the safety
  // lists' (RefreshSafetyList); then the release check (MaybeCheckRelease).
  // One at a time.
  void MaybeUpdateLists() {
    if (updater_pid_ > 0) {
      return;
    }
    if (adblock_setting_ != "off") {
      MaybeUpdateFilterLists();
    }
    if (updater_pid_ <= 0 && !safety_list_checked_) {
      safety_list_checked_ = true;
      RefreshSafetyList();
    }
    if (updater_pid_ <= 0) {
      MaybeCheckRelease();
    }
  }

  void MaybeUpdateFilterLists() {
    const Clock::time_point now = Clock::now();
    if (last_update_check_ != Clock::time_point() &&
        now - last_update_check_ < std::chrono::hours(1)) {
      return;
    }
    last_update_check_ = now;
    struct stat engine = {};
    if (stat(adblock_file_.c_str(), &engine) == 0 &&
        time(nullptr) - engine.st_mtime < 24 * 3600) {
      return;
    }
    Log("updating filter lists (%s) into %s", adblock_setting_.c_str(),
        adblock_file_.c_str());
    RunUpdater(adblock_file_, adblock_setting_);
  }

  // The phishing and malware lists, when lrb starts, unless fetched in the
  // last hour (a quick restart).
  void RefreshSafetyList() {
    struct stat engine = {};
    if (!options_.safety_lists ||
        (stat(options_.safety_file.c_str(), &engine) == 0 &&
         time(nullptr) - engine.st_mtime < 3600)) {
      return;
    }
    Log("updating safety lists into %s", options_.safety_file.c_str());
    RunUpdater(options_.safety_file, "safety");
  }

  void RunUpdater(const std::string& file, const std::string& setting) {
    const size_t slash = file.rfind('/');
    const std::string dir = file.substr(0, slash);
    MakeDirs(dir + "/updater-profile", 0700);
    std::vector<std::string> args = {
        options_.browser, "--lrb-update-lists=" + file,
        "--lrb-adblock-setting=" + setting,
        "--user-data-dir=" + dir + "/updater-profile"};
    for (const std::string& arg : options_.browser_args) {
      if (!arg.starts_with("--remote-debugging")) {
        args.push_back(arg);
      }
    }
    updater_pid_ = Spawn(args, dir);
    updater_lists_ = setting == "safety" ? "safety" : "filter";
  }

  // The newest lrb release, at most once a day (by the file's age), after
  // the list updates and never beside one: `lrb --lrb-check-release=<file>`
  // (no window) writes its version there. The file has a directory of its
  // own: the check's confinement may write only there, not beside every
  // profile. Windows opened afterwards get it (--lrb-latest-release), and
  // the bar offers the update when it is newer than theirs.
  void MaybeCheckRelease() {
    const Clock::time_point now = Clock::now();
    if (!options_.release_check || now < next_release_check_) {
      return;
    }
    struct stat file = {};
    if (stat(release_file_.c_str(), &file) == 0 &&
        time(nullptr) - file.st_mtime < 24 * 3600) {
      next_release_check_ = now + std::chrono::hours(1);
      return;
    }
    // A failed check leaves the file as it was: not again today.
    next_release_check_ = now + std::chrono::hours(24);
    const std::string dir = DirName(release_file_);
    MakeDirs(dir + "/profile", 0700);
    std::vector<std::string> args = {options_.browser,
                                     "--lrb-check-release=" + release_file_,
                                     "--user-data-dir=" + dir + "/profile"};
    for (const std::string& arg : options_.browser_args) {
      if (!arg.starts_with("--remote-debugging")) {
        args.push_back(arg);
      }
    }
    if (options_.verbose) {
      Log("checking for a new release into %s", release_file_.c_str());
    }
    updater_pid_ = Spawn(args, dir);
    updater_lists_ = "release";
  }

  // The filter lists were just built: instances started without them load
  // the engine now.
  void SendEngineToWaiting() {
    struct stat engine = {};
    if (adblock_setting_ == "off" ||
        stat(adblock_file_.c_str(), &engine) != 0) {
      return;
    }
    std::vector<int> waiting;
    for (auto& [fd, client] : clients_) {
      if (client.without_engine) {
        client.without_engine = false;
        waiting.push_back(fd);
      }
    }
    for (int fd : waiting) {
      Send(fd, "adblock-file " + adblock_file_);
    }
  }

  // The settings window, in a process of its own that loads no web content
  // and is the only one allowed to write lrb's settings. One at a time: a
  // page can at most make it appear; the user decides.
  void OpenSettings() {
    if (settings_pid_ > 0) {
      return;
    }
    const std::string profile = options_.profiles_dir + "/.settings";
    MakeDirs(profile, 0700);
    std::vector<std::string> args = {options_.browser, "--lrb-settings",
                                     "--user-data-dir=" + profile};
    for (const std::string& arg : options_.browser_args) {
      if (!arg.starts_with("--remote-debugging")) {
        args.push_back(arg);
      }
    }
    settings_pid_ = Spawn(std::move(args), profile, /*settings_writer=*/true);
  }

  // In a child between fork() and exec(): confines it to writing `rw_dir`
  // (InstancePolicy), or exits. `allow_sandbox`: Chromium's own sandbox
  // runs inside (the standard instance).
  void ConfineChild(const std::string& rw_dir,
                    bool settings_writer,
                    bool allow_sandbox,
                    const std::string& program) const {
    if (!options_.confine) {
      return;
    }
    // Temporary files in the profile: /tmp is outside the confinement.
    const std::string tmp = rw_dir + "/tmp";
    MakeDirs(tmp, 0700);
    setenv("TMPDIR", tmp.c_str(), 1);
    const std::string x11 = X11SocketPath(Getenv("DISPLAY"));
    if (!x11.empty()) {
      setenv("DISPLAY", x11.c_str(), 1);
    }
    const coordinator::ConfinePolicy policy =
        allow_sandbox ? StandardPolicy(options_, rw_dir)
                      : InstancePolicy(options_, rw_dir, settings_writer);
    std::string error;
    if (!coordinator::Confine(policy, &error)) {
      fprintf(stderr, "lrb_coordinator: not starting %s: %s\n",
              program.c_str(), error.c_str());
      _exit(126);
    }
    if (!error.empty()) {
      fprintf(stderr, "lrb_coordinator: %s\n", error.c_str());
    }
  }

  // Starts `args` (lrb), confined to writing `rw_dir` (see InstancePolicy).
  pid_t Spawn(std::vector<std::string> args,
              const std::string& rw_dir,
              bool settings_writer = false,
              bool allow_sandbox = false) {
    if (ProfileUnused(rw_dir)) {
      // Left by a coordinator that was killed or crashed.
      coordinator::FileBroker::CleanProfile(rw_dir);
    }
    const pid_t pid = fork();
    if (pid == 0) {
      ConfineChild(rw_dir, settings_writer, allow_sandbox, args[0]);
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

  // A stop signal: the instances stop too, and are reaped so their staged
  // copies of the user's files are deleted (Reap), not left in profiles.
  void Stop() {
    Log("signal %d: stopping %zu instance(s)", static_cast<int>(g_stop_signal),
        child_profiles_.size());
    for (const auto& [pid, profile] : child_profiles_) {
      kill(pid, SIGTERM);
    }
    const auto deadline = Clock::now() + kStopGrace;
    while (!child_profiles_.empty() && Clock::now() < deadline) {
      poll(nullptr, 0, 100);
      Reap();
    }
    for (const auto& [pid, profile] : child_profiles_) {
      Log("instance %d didn't stop", pid);
    }
  }

  // No other instance runs on `profile`.
  bool ProfileUnused(const std::string& profile) const {
    return std::ranges::none_of(child_profiles_, [&](const auto& child) {
      return child.second == profile;
    });
  }

  // The first window: the address given, else what the settings say to
  // start with (lrb/common/settings.h): the sites open last time, with
  // their windows as they were (default), an empty window, or a page.
  void LaunchFirst() {
    if (!options_.url.empty()) {
      LaunchUrl(options_.url);
      return;
    }
    const std::string settings = ReadSettings();
    const std::string startup =
        JsonString(settings, "startup").value_or("last-site");
    if (startup == "page") {
      const std::optional<std::string> page =
          JsonString(settings, "startup_page");
      if (page && IsAcceptableUrl(*page)) {
        LaunchUrl(*page);
        return;
      }
    } else if (startup != "blank") {
      OpenOptions open;
      open.resume = true;
      if (standard_) {
        // One profile: the instance restores every site's windows.
        ShowStandard(/*url=*/"", open);
        return;
      }
      std::string site;
      std::string url;
      if (ReadLastSite(&site, &url)) {
        Log("starting on the last site, %s", site.c_str());
        Open(site, url, open);
        // Then the other sites that had windows shown, in the background.
        constexpr size_t kMaxRestored = 6;
        for (const std::string& other : ReadOpenSites()) {
          if (other != site && restore_queue_.size() < kMaxRestored) {
            restore_queue_.push_back(other);
          }
        }
        next_restore_ = Clock::now() + kRestoreInterval;
        return;
      }
    }
    if (standard_) {
      ShowStandard(/*url=*/"", OpenOptions());
    } else {
      Launch(/*site=*/"", "");
    }
  }

  // Restoring the last session's other sites: one every 2 s, so the site
  // the user starts on loads first and each load gets room (MakeRoom).
  static constexpr auto kRestoreInterval = std::chrono::seconds(2);

  void RestoreNext() {
    const Clock::time_point now = Clock::now();
    if (restore_queue_.empty() || now < next_restore_) {
      return;
    }
    const std::string site = restore_queue_.front();
    restore_queue_.erase(restore_queue_.begin());
    next_restore_ = now + kRestoreInterval;
    if (!ProfileUnused(options_.profiles_dir + "/" + site)) {
      return;  // opened since
    }
    Log("restoring %s from the last session", site.c_str());
    OpenOptions open;
    open.resume = true;
    const std::string url = "https://" + site + "/";
    site_urls_[site] = url;
    MakeRoom(nullptr);
    Launch(site, url, open);
  }

  std::string LastSitePath() const {
    return options_.profiles_dir + "/.last-site";
  }

  std::string OpenSitesPath() const {
    return options_.profiles_dir + "/.open-sites";
  }

  // The sites with a window shown, one per line, the most recently active
  // first (lean mode; parked ones are left out: the user left them), for
  // reopening them all at the next start. Written when it changes, not
  // while stopping (instances exiting then are still the session).
  void UpdateOpenSites() {
    if (standard_ || g_stop_signal) {
      return;
    }
    std::vector<const Client*> shown;
    for (const auto& [fd, client] : clients_) {
      if (!client.site.empty() && !client.standard && !client.parked &&
          !client.closing) {
        shown.push_back(&client);
      }
    }
    std::ranges::sort(shown, [](const Client* a, const Client* b) {
      return a->last_active > b->last_active;
    });
    std::string contents;
    std::vector<std::string> listed;
    for (const Client* client : shown) {
      if (std::ranges::find(listed, client->site) == listed.end()) {
        listed.push_back(client->site);
        contents += client->site + "\n";
      }
    }
    if (open_sites_written_ && contents == *open_sites_written_) {
      return;
    }
    open_sites_written_ = contents;
    WriteFileAtomically(OpenSitesPath(), contents);
  }

  std::vector<std::string> ReadOpenSites() const {
    std::vector<std::string> sites;
    const std::string contents = ReadSmallFile(OpenSitesPath(), 65536);
    for (size_t start = 0; start < contents.size();) {
      const size_t end = std::min(contents.find('\n', start), contents.size());
      const std::string site = contents.substr(start, end - start);
      if (IsValidSite(site) && std::ranges::find(sites, site) == sites.end()) {
        sites.push_back(site);
      }
      start = end + 1;
    }
    return sites;
  }

  // The site in use, and the address that opened it ("<site> <url>"), for
  // starting there next time. Written when the site in use changes.
  void RecordLastSite(const std::string& site) {
    if (site.empty() || site == last_site_written_) {
      return;
    }
    last_site_written_ = site;
    auto known = site_urls_.find(site);
    const std::string url = known != site_urls_.end()
                                ? known->second
                                : "https://" + site + "/";
    WriteFileAtomically(LastSitePath(), site + " " + url + "\n");
  }

  bool ReadLastSite(std::string* site, std::string* url) const {
    char buffer[9000] = {};
    FILE* file = fopen(LastSitePath().c_str(), "r");
    if (!file) {
      return false;
    }
    std::string line(buffer, fread(buffer, 1, sizeof(buffer) - 1, file));
    fclose(file);
    if (!line.empty() && line.back() == '\n') {
      line.pop_back();
    }
    const size_t space = line.find(' ');
    *site = line.substr(0, space);
    if (!IsValidSite(*site)) {
      return false;
    }
    *url = space == std::string::npos ? "" : line.substr(space + 1);
    if (!IsAcceptableUrl(*url) || !UrlBelongsToSite(*url, *site)) {
      *url = "https://" + *site + "/";
    }
    return true;
  }

  // The content blocking level from the settings (unless given on the
  // command line): windows opened from now on get it.
  void ApplyAdblockSetting() {
    if (options_.adblock_setting_given) {
      return;
    }
    const std::string level = AdblockLevel();
    if (level == adblock_setting_) {
      return;
    }
    Log("content blocking: %s (was %s)", level.c_str(),
        adblock_setting_.c_str());
    adblock_setting_ = level;
    if (!options_.adblock_file_given) {
      adblock_file_ = options_.data_home + "/lrb/adblock/" + level + ".adb";
    }
    last_update_check_ = Clock::time_point();  // fetch its lists if missing
    MaybeUpdateLists();
  }

  void Reap() {
    pid_t pid;
    int status = 0;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
      if (broker_.OnChildExit(pid, status) ||
          keyring_.OnChildExit(pid, status)) {
        continue;
      }
      auto profile = child_profiles_.find(pid);
      if (profile == child_profiles_.end()) {
        continue;  // not ours to count (xdg-open: show-saved, open-app)
      }
      const std::string rw_dir = std::move(profile->second);
      child_profiles_.erase(profile);
      if (ProfileUnused(rw_dir)) {
        coordinator::FileBroker::CleanProfile(rw_dir);
      }
      if (pid == updater_pid_) {
        updater_pid_ = 0;
        if (updater_lists_ == "release") {
          const std::string release =
              ValidRelease(ReadSmallFile(release_file_, 64));
          if (!release.empty()) {
            latest_release_ = release;
          }
          if (options_.verbose) {
            Log("release check finished: %s",
                release.empty() ? "nothing" : release.c_str());
          }
        } else {
          Log("%s-list update finished", updater_lists_.c_str());
          if (updater_lists_ == "filter") {
            SendEngineToWaiting();
          }
        }
      } else if (pid == settings_pid_) {
        settings_pid_ = 0;
        ApplyAdblockSetting();  // the user may have changed it
      } else {
        --children_;
        without_engine_.erase(pid);
        if (pid == standard_pid_) {
          standard_pid_ = 0;
          pending_standard_urls_.clear();
        }
      }
    }
  }

  const Options options_;
  // Content blocking: options_' level, or the settings' once changed there.
  std::string adblock_setting_;
  std::string adblock_file_;
  // Standard mode (DecideMode): one instance for every site, on this
  // profile, beside the sites' directory.
  bool standard_ = false;
  const std::string standard_profile_;
  pid_t standard_pid_ = 0;  // while it runs
  // Addresses for the standard instance before it connected.
  std::vector<std::string> pending_standard_urls_;
  // The release check's file, and the newest release it found.
  const std::string release_file_;
  std::string latest_release_;
  Clock::time_point next_release_check_;
  // The address each site was last opened with, and the site last recorded
  // as in use (RecordLastSite).
  std::map<std::string, std::string> site_urls_;
  std::string last_site_written_;
  // .open-sites as last written (UpdateOpenSites); the sites still to
  // reopen from the last session, and when the next one is due.
  std::optional<std::string> open_sites_written_;
  std::vector<std::string> restore_queue_;
  Clock::time_point next_restore_;
  int listen_fd_ = -1;
  int children_ = 0;
  std::map<int, Client> clients_;
  coordinator::FileBroker broker_;
  coordinator::KeyringBroker keyring_;
  // Instances' profiles by process id (Spawn).
  std::map<pid_t, std::string> child_profiles_;
  // Instances launched before the filter lists existed, until they connect
  // (Accept moves this to Client::without_engine).
  std::set<pid_t> without_engine_;
  // Windows waiting to hide until their replacement shows, by the token
  // the target got.
  std::map<uint64_t, Handoff> handoffs_;
  uint64_t next_handoff_ = 1;
  Clock::time_point last_discard_;
  Clock::time_point last_stuck_report_;
  Clock::time_point last_update_check_;
  pid_t updater_pid_ = 0;
  // "filter", "safety" or "release", while it runs.
  std::string updater_lists_;
  bool safety_list_checked_ = false;
  pid_t settings_pid_ = 0;  // the settings window, while open
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
