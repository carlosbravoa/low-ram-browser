// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_UI_SETTINGS_DIALOG_H_
#define LRB_BROWSER_UI_SETTINGS_DIALOG_H_

#include "base/memory/weak_ptr.h"

namespace views {
class View;
}

namespace lrb {

// The settings window (lrb/common/settings.h): search engine and
// rendering. Native Views, not a web page: no page can ever reach it. It
// exists only while open.
// `parent`: a view of the window it belongs to.
void ShowSettings(base::WeakPtr<views::View> parent);

// The settings window on its own (lrb --lrb-settings): the process the
// coordinator starts to change the settings, the only one allowed to write
// them (confined instances can't: a hijacked page could change the search
// engine). `first_start`: the first start's question about the GPU (its
// memory trade-off) above the settings. `closed` runs when it closes.
void ShowSettingsAlone(bool first_start, base::OnceClosure closed);

}  // namespace lrb

#endif  // LRB_BROWSER_UI_SETTINGS_DIALOG_H_
