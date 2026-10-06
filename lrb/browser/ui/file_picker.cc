// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/ui/file_picker.h"

#include <unistd.h>

#include <memory>
#include <optional>
#include <utility>

#include "base/environment.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
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

}  // namespace

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
  if (!CanPickFiles()) {
    std::move(done).Run(PickResult::kUnavailable, {});
    return;
  }
  (new FilePicker(std::move(done)))->Show(type, title, default_path, owner);
}

}  // namespace lrb
