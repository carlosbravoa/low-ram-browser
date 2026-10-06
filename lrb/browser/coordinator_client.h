// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_COORDINATOR_CLIENT_H_
#define LRB_BROWSER_COORDINATOR_CLIENT_H_

#include <memory>
#include <string>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/task/sequenced_task_runner.h"

namespace lrb {

// This instance's connection to lrb_coordinator (lrb/coordinator), a
// newline-terminated text protocol over a Unix socket. Created and used on
// the UI thread; the socket is watched on the IO thread.
class CoordinatorClient {
 public:
  // Called on the UI thread with each line the coordinator sends.
  using LineCallback = base::RepeatingCallback<void(const std::string&)>;

  // Nullptr if the coordinator can't be reached.
  static std::unique_ptr<CoordinatorClient> Connect(
      const base::FilePath& socket_path,
      LineCallback on_line);

  CoordinatorClient(const CoordinatorClient&) = delete;
  CoordinatorClient& operator=(const CoordinatorClient&) = delete;
  ~CoordinatorClient();

  // Sends one line (without the newline).
  void Send(const std::string& line);

 private:
  class Io;
  explicit CoordinatorClient(std::unique_ptr<Io, base::OnTaskRunnerDeleter> io);

  std::unique_ptr<Io, base::OnTaskRunnerDeleter> io_;
};

}  // namespace lrb

#endif  // LRB_BROWSER_COORDINATOR_CLIENT_H_
