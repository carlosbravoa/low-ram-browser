// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_UI_FILE_PICKER_H_
#define LRB_BROWSER_UI_FILE_PICKER_H_

#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback_forward.h"
#include "ui/gfx/native_ui_types.h"
#include "ui/shell_dialogs/select_file_dialog.h"

namespace lrb {

// The system's file picker: the desktop portal (org.freedesktop.portal.
// FileChooser) over the session bus. It runs in the desktop's own process,
// so lrb loads no toolkit for it (GTK would cost each instance memory),
// and it works from a confined package.
enum class PickResult {
  kChosen,
  kCanceled,
  // No picker answered (no session bus, no portal): the caller decides
  // what to do without asking.
  kUnavailable,
};

// Whether a picker can be asked for at all: a session bus to reach the
// portal on. (The harness disables it to keep lrb in its cgroup.)
bool CanPickFiles();

// Shows the picker; `done` gets the result and the chosen path(s).
void PickFiles(
    ui::SelectFileDialog::Type type,
    const std::u16string& title,
    const base::FilePath& default_path,
    gfx::NativeWindow owner,
    base::OnceCallback<void(PickResult, std::vector<base::FilePath>)> done);

// Under the coordinator, instances are confined (lrb/coordinator/confine.h)
// and can't open the user's files: PickFiles asks the coordinator, which
// shows the picker (lrb/coordinator/broker.h). Uploads come back as copies
// in this profile. A download target comes back as a staging path in this
// profile; when the download is complete, FinishBrokeredSave has the
// coordinator move it where the user chose.
struct BrokeredSave {
  enum class State { kWriting, kMoving, kMoved, kFailed };
  std::string id;
  base::FilePath target;   // where the user chose
  bool automatic = false;  // no picker: the coordinator chose Downloads
  State state = State::kWriting;
};
// The brokered save behind `staged` (a path PickFiles returned), if any.
std::optional<BrokeredSave> GetBrokeredSave(const base::FilePath& staged);
// Asks the coordinator to move it, once (later calls do nothing); `done` gets
// whether it did.
void FinishBrokeredSave(const base::FilePath& staged,
                        base::OnceCallback<void(bool)> done);
void CancelBrokeredSave(const base::FilePath& staged);
// A line from the coordinator: true if it was the file broker's.
bool OnBrokerLine(const std::string& line);

}  // namespace lrb

#endif  // LRB_BROWSER_UI_FILE_PICKER_H_
