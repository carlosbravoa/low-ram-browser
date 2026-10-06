// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_COORDINATOR_BROKER_H_
#define LRB_COORDINATOR_BROKER_H_

#include <sys/types.h>

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace lrb::coordinator {

// Files the user picks, for confined instances (confine.h), which can't open
// the user's files themselves. The coordinator shows the desktop's picker
// (lrb_picker), so the choice is the user's and not a page's, and hands over
// only what was chosen:
//
//   instance -> coordinator          coordinator -> instance
//   pick-open <id> <multiple 0|1>    picked <id> [<file-uri>...]
//     (upload: the chosen files are    (copies in the instance's profile,
//     copied into <profile>/uploads)   none: cancelled; "unavailable": no
//                                      picker)
//   pick-save <id> <name>            save-target <id> <file-uri> [auto]
//     (download: where to save)        (where it will go; "auto": no picker,
//                                      the Downloads folder; none: cancelled)
//                                    The instance writes to
//                                    <profile>/downloads/<id>/<file name>.
//   save-done <id>                   saved <id> 0|1
//     (finished: the coordinator moves the staged file to the target)
//   save-cancel <id>
//   show-saved <id>                  (the file manager on its folder)
//
// <id> is the instance's, unique per request; names and URIs are
// percent-encoded. Requests are tied to the instance's profile as the
// coordinator started it (never to what an instance says about itself).
class FileBroker {
 public:
  // Sends a line (without the newline) to the instance on `fd`.
  using Reply = std::function<void(int fd, const std::string& line)>;

  FileBroker(std::string picker, std::string downloads_dir, Reply reply);
  ~FileBroker();

  // A request from the instance on `fd`, whose profile is `profile` (empty:
  // unknown, refused).
  void Handle(int fd,
              const std::string& profile,
              const std::string& command,
              const std::string& args);

  // Pipes of pickers waiting for the user, to poll.
  std::vector<int> fds() const;
  void OnReadable(int fd);
  // True if `pid` was a picker (its status is recorded).
  bool OnChildExit(pid_t pid, int status);

  // The instance on `fd` disconnected: forget its requests.
  void Forget(int fd);
  // The instance using `profile` exited: delete its staged copies and
  // unfinished downloads.
  static void CleanProfile(const std::string& profile);

  static std::string Encode(const std::string& text);
  static std::string Decode(const std::string& text);

 private:
  struct Pick {
    int client_fd = -1;
    std::string profile;
    std::string id;
    bool save = false;
    pid_t pid = -1;
    int out = -1;
    std::string output;
    bool eof = false;
    bool exited = false;
    int status = 0;
    std::string name;  // save: the suggested file name
  };

  void StartPicker(Pick pick, bool multiple);
  void Finish(Pick& pick);
  void FinishOpen(Pick& pick, const std::vector<std::string>& paths);
  void FinishSave(Pick& pick, const std::vector<std::string>& paths);
  void SaveDone(int fd, const std::string& profile, const std::string& id);

  std::string picker_;
  std::string downloads_dir_;
  Reply reply_;
  std::map<int, Pick> picks_;  // by pipe fd
  // (profile, id) -> where the finished download goes.
  std::map<std::pair<std::string, std::string>, std::string> saves_;
  // (profile, id) -> where finished downloads went (show-saved).
  std::map<std::pair<std::string, std::string>, std::string> saved_;
};

}  // namespace lrb::coordinator

#endif  // LRB_COORDINATOR_BROKER_H_
