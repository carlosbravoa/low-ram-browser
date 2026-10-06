// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/ui/file_picker.h"

#include <unistd.h>

#include <memory>
#include <optional>
#include <utility>

#include <map>

#include "base/environment.h"
#include "base/files/file_util.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/no_destructor.h"
#include "base/strings/escape.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "lrb/browser/lrb_browser_context.h"
#include "lrb/browser/lrb_content_browser_client.h"
#include "net/base/filename_util.h"
#include "url/gurl.h"
#include "ui/shell_dialogs/select_file_policy.h"
#include "ui/shell_dialogs/selected_file_info.h"

namespace lrb {

namespace {

// The portal answers a failure (no portal service, no window to attach to)
// the same way as the user cancelling. A person takes longer than this to
// cancel; a failure comes back at once.
constexpr base::TimeDelta kTooFastForAPerson = base::Milliseconds(400);

// Owns itself until the picker answers.
class FilePicker : public ui::SelectFileDialog::Listener {
 public:
  explicit FilePicker(
      base::OnceCallback<void(PickResult, std::vector<base::FilePath>)> done)
      : done_(std::move(done)),
        dialog_(ui::SelectFileDialog::Create(this, nullptr)) {}

  void Show(ui::SelectFileDialog::Type type,
            const std::u16string& title,
            const base::FilePath& default_path,
            gfx::NativeWindow owner) {
    opened_ = base::TimeTicks::Now();
    dialog_->SelectFile(type, title, default_path, nullptr, 0,
                        base::FilePath::StringType(), owner);
  }

  // ui::SelectFileDialog::Listener:
  void FileSelected(const ui::SelectedFileInfo& file, int index) override {
    Finish(PickResult::kChosen, {file.path()});
  }
  void MultiFilesSelected(
      const std::vector<ui::SelectedFileInfo>& files) override {
    std::vector<base::FilePath> paths;
    for (const ui::SelectedFileInfo& file : files) {
      paths.push_back(file.path());
    }
    Finish(PickResult::kChosen, std::move(paths));
  }
  void FileSelectionCanceled() override {
    Finish(base::TimeTicks::Now() - opened_ < kTooFastForAPerson
               ? PickResult::kUnavailable
               : PickResult::kCanceled,
           {});
  }

 private:
  ~FilePicker() override { dialog_->ListenerDestroyed(); }

  void Finish(PickResult result, std::vector<base::FilePath> paths) {
    auto done = std::move(done_);
    delete this;
    std::move(done).Run(result, std::move(paths));
  }

  base::OnceCallback<void(PickResult, std::vector<base::FilePath>)> done_;
  scoped_refptr<ui::SelectFileDialog> dialog_;
  base::TimeTicks opened_;
};

// The file broker's requests waiting for the coordinator, by id.
using PickCallback =
    base::OnceCallback<void(PickResult, std::vector<base::FilePath>)>;
struct Broker {
  int next_id = 1;
  std::map<std::string, PickCallback> picks;  // pick-open / pick-save
  std::map<base::FilePath, BrokeredSave> saves;  // by staging path
  std::map<std::string, base::OnceCallback<void(bool)>> finishing;
  std::map<std::string, base::FilePath> staged_by_id;
};
Broker& GetBroker() {
  static base::NoDestructor<Broker> broker;
  return *broker;
}

void SendToCoordinator(const std::string& line) {
  if (LrbContentBrowserClient* client = LrbContentBrowserClient::Get()) {
    client->SendToCoordinator(line);
  }
}

std::string Encode(const std::string& text) {
  return base::EscapeAllExceptUnreserved(text);
}

base::FilePath PathOfUri(const std::string& uri) {
  base::FilePath path;
  net::FileURLToFilePath(GURL(uri), &path);
  return path;
}

void PickThroughCoordinator(ui::SelectFileDialog::Type type,
                            const base::FilePath& default_path,
                            PickCallback done) {
  Broker& broker = GetBroker();
  const std::string id = base::NumberToString(broker.next_id++);
  broker.picks[id] = std::move(done);
  if (type == ui::SelectFileDialog::SELECT_SAVEAS_FILE) {
    SendToCoordinator("pick-save " + id + " " +
                      Encode(default_path.BaseName().value()));
  } else {
    SendToCoordinator(
        "pick-open " + id + " " +
        (type == ui::SelectFileDialog::SELECT_OPEN_MULTI_FILE ? "1" : "0"));
  }
}

// "save-target <id> <uri> [auto]": the download is written to
// <profile>/downloads/<id>/<target's name> (created first).
void OnSaveTarget(const std::string& id,
                  const std::vector<std::string>& words,
                  PickCallback done) {
  BrokeredSave save;
  save.id = id;
  save.target = PathOfUri(words[2]);
  save.automatic = words.size() > 3 && words[3] == "auto";
  const base::FilePath dir =
      LrbBrowserContext::ProfileDir().Append("downloads").Append(id);
  const base::FilePath staged = dir.Append(save.target.BaseName());
  GetBroker().saves[staged] = save;
  GetBroker().staged_by_id[id] = staged;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&base::CreateDirectory, dir),
      base::BindOnce(
          [](base::FilePath staged, PickCallback done, bool created) {
            if (created) {
              std::move(done).Run(PickResult::kChosen, {staged});
            } else {
              GetBroker().saves.erase(staged);
              std::move(done).Run(PickResult::kCanceled, {});
            }
          },
          staged, std::move(done)));
}

}  // namespace

std::optional<BrokeredSave> GetBrokeredSave(const base::FilePath& staged) {
  auto it = GetBroker().saves.find(staged);
  if (it == GetBroker().saves.end()) {
    return std::nullopt;
  }
  return it->second;
}

void FinishBrokeredSave(const base::FilePath& staged,
                        base::OnceCallback<void(bool)> done) {
  Broker& broker = GetBroker();
  auto it = broker.saves.find(staged);
  if (it == broker.saves.end() ||
      it->second.state != BrokeredSave::State::kWriting) {
    return;
  }
  it->second.state = BrokeredSave::State::kMoving;
  broker.finishing[it->second.id] = std::move(done);
  SendToCoordinator("save-done " + it->second.id);
}

void CancelBrokeredSave(const base::FilePath& staged) {
  Broker& broker = GetBroker();
  auto it = broker.saves.find(staged);
  if (it != broker.saves.end()) {
    SendToCoordinator("save-cancel " + it->second.id);
    broker.saves.erase(it);
  }
}

bool OnBrokerLine(const std::string& line) {
  const std::vector<std::string> words = base::SplitString(
      line, " ", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  if (words.size() < 2 ||
      (words[0] != "picked" && words[0] != "save-target" &&
       words[0] != "saved")) {
    return false;
  }
  Broker& broker = GetBroker();
  const std::string& id = words[1];
  if (words[0] == "saved") {
    const bool moved = words.size() > 2 && words[2] == "1";
    if (auto staged = broker.staged_by_id.find(id);
        staged != broker.staged_by_id.end() &&
        broker.saves.contains(staged->second)) {
      broker.saves[staged->second].state =
          moved ? BrokeredSave::State::kMoved : BrokeredSave::State::kFailed;
    }
    auto it = broker.finishing.find(id);
    if (it != broker.finishing.end()) {
      auto done = std::move(it->second);
      broker.finishing.erase(it);
      std::move(done).Run(moved);
    }
    return true;
  }
  auto it = broker.picks.find(id);
  if (it == broker.picks.end()) {
    return true;
  }
  PickCallback done = std::move(it->second);
  broker.picks.erase(it);
  if (words.size() == 2) {
    std::move(done).Run(PickResult::kCanceled, {});
  } else if (words[2] == "unavailable") {
    std::move(done).Run(PickResult::kUnavailable, {});
  } else if (words[0] == "save-target") {
    OnSaveTarget(id, words, std::move(done));
  } else {
    std::vector<base::FilePath> paths;
    for (size_t i = 2; i < words.size(); ++i) {
      paths.push_back(PathOfUri(words[i]));
    }
    std::move(done).Run(PickResult::kChosen, std::move(paths));
  }
  return true;
}

bool CanPickFiles() {
  std::unique_ptr<base::Environment> env = base::Environment::Create();
  std::optional<std::string> address = env->GetVar("DBUS_SESSION_BUS_ADDRESS");
  if (address) {
    return !address->empty() && !address->starts_with("disabled:");
  }
  // libdbus's default: the user bus socket. (access(): one stat, without
  // base's assertion against blocking calls on the UI thread.)
  std::optional<std::string> runtime = env->GetVar("XDG_RUNTIME_DIR");
  return runtime && access((*runtime + "/bus").c_str(), F_OK) == 0;
}

void PickFiles(
    ui::SelectFileDialog::Type type,
    const std::u16string& title,
    const base::FilePath& default_path,
    gfx::NativeWindow owner,
    base::OnceCallback<void(PickResult, std::vector<base::FilePath>)> done) {
  // Confined under the coordinator: it shows the picker.
  if (LrbContentBrowserClient* client = LrbContentBrowserClient::Get();
      client && client->has_coordinator()) {
    PickThroughCoordinator(type, default_path, std::move(done));
    return;
  }
  if (!CanPickFiles()) {
    std::move(done).Run(PickResult::kUnavailable, {});
    return;
  }
  (new FilePicker(std::move(done)))->Show(type, title, default_path, owner);
}

}  // namespace lrb
