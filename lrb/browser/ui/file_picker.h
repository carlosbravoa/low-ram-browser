// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_UI_FILE_PICKER_H_
#define LRB_BROWSER_UI_FILE_PICKER_H_

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

}  // namespace lrb

#endif  // LRB_BROWSER_UI_FILE_PICKER_H_
