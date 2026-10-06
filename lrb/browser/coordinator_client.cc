// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/coordinator_client.h"

#include <sys/socket.h>
#include <sys/un.h>

#include <utility>

#include "base/files/file_descriptor_watcher_posix.h"
#include "base/files/scoped_file.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "base/posix/eintr_wrapper.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"

namespace lrb {

namespace {
constexpr size_t kMaxLine = 9000;
}  // namespace

// Owns the socket. Lives on the IO thread, where file descriptors can be
// watched; lines go back to the UI thread.
class CoordinatorClient::Io {
 public:
  Io(base::ScopedFD fd, LineCallback on_line)
      : fd_(std::move(fd)),
        on_line_(std::move(on_line)),
        ui_task_runner_(content::GetUIThreadTaskRunner({})) {}

  void StartWatching() {
    DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
    watcher_ = base::FileDescriptorWatcher::WatchReadable(
        fd_.get(), base::BindRepeating(&Io::OnReadable, base::Unretained(this)));
  }

  void Send(const std::string& line) {
    DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
    if (!fd_.is_valid()) {
      return;
    }
    const std::string data = line + "\n";
    // Lines are short and the socket buffer large; a short write means the
    // coordinator is gone or stuck, either way drop the connection.
    const ssize_t n = HANDLE_EINTR(
        send(fd_.get(), data.data(), data.size(), MSG_NOSIGNAL | MSG_DONTWAIT));
    if (n != static_cast<ssize_t>(data.size())) {
      Disconnect();
    }
  }

 private:
  void OnReadable() {
    char buffer[4096];
    const ssize_t n = HANDLE_EINTR(recv(fd_.get(), buffer, sizeof(buffer), 0));
    if (n <= 0) {
      Disconnect();
      return;
    }
    input_.append(buffer, static_cast<size_t>(n));
    size_t newline;
    while ((newline = input_.find('\n')) != std::string::npos) {
      ui_task_runner_->PostTask(
          FROM_HERE, base::BindOnce(on_line_, input_.substr(0, newline)));
      input_.erase(0, newline + 1);
    }
    if (input_.size() > kMaxLine) {
      Disconnect();
    }
  }

  void Disconnect() {
    // The instance keeps working without a coordinator.
    LOG(WARNING) << "lrb: lost the coordinator";
    watcher_.reset();
    fd_.reset();
  }

  base::ScopedFD fd_;
  LineCallback on_line_;
  scoped_refptr<base::SequencedTaskRunner> ui_task_runner_;
  std::unique_ptr<base::FileDescriptorWatcher::Controller> watcher_;
  std::string input_;
};

// static
std::unique_ptr<CoordinatorClient> CoordinatorClient::Connect(
    const base::FilePath& socket_path,
    LineCallback on_line) {
  base::ScopedFD fd(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
  sockaddr_un address = {};
  address.sun_family = AF_UNIX;
  const std::string& path = socket_path.value();
  if (!fd.is_valid() || path.size() >= sizeof(address.sun_path)) {
    return nullptr;
  }
  path.copy(address.sun_path, path.size());
  // A local socket connects at once or fails; it doesn't block.
  if (HANDLE_EINTR(connect(fd.get(), reinterpret_cast<sockaddr*>(&address),
                           sizeof(address))) != 0) {
    PLOG(WARNING) << "lrb: no coordinator at " << path;
    return nullptr;
  }

  auto io_task_runner = content::GetIOThreadTaskRunner({});
  std::unique_ptr<Io, base::OnTaskRunnerDeleter> io(
      new Io(std::move(fd), std::move(on_line)),
      base::OnTaskRunnerDeleter(io_task_runner));
  io_task_runner->PostTask(FROM_HERE, base::BindOnce(&Io::StartWatching,
                                                     base::Unretained(io.get())));
  return base::WrapUnique(new CoordinatorClient(std::move(io)));
}

CoordinatorClient::CoordinatorClient(
    std::unique_ptr<Io, base::OnTaskRunnerDeleter> io)
    : io_(std::move(io)) {}

CoordinatorClient::~CoordinatorClient() = default;

void CoordinatorClient::Send(const std::string& line) {
  // `io_` is deleted on the IO thread after any task posted before it.
  content::GetIOThreadTaskRunner({})->PostTask(
      FROM_HERE,
      base::BindOnce(&Io::Send, base::Unretained(io_.get()), line));
}

}  // namespace lrb
