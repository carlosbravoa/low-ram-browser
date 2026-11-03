// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_UI_SNAP_CONNECTIONS_DIALOG_H_
#define LRB_BROWSER_UI_SNAP_CONNECTIONS_DIALOG_H_

#include <string>

#include "base/memory/weak_ptr.h"

namespace views {
class View;
}

namespace lrb {

// Whether lrb runs as a snap: snapd sets SNAP_NAME in its environment.
bool RunningAsSnap();

// The Snap connections window: which of the snap's plugs that aren't
// connected automatically are connected (each enables one feature), and the
// terminal commands that connect the others. The coordinator runs snapctl
// and answers ("snap-plugs <id>"): an instance runs no programs. Native
// Views, not a page. It opens once the coordinator answers (5 s at most),
// built for the answer. `parent`: a view of the window it belongs to.
void ShowSnapConnections(base::WeakPtr<views::View> parent);

// A line from the coordinator: true if it was an answer to a snap-plugs
// request ("snap-plug <id> <plug> 0|1", "snap-plugs-done <id>").
bool OnSnapPlugsLine(const std::string& line);

}  // namespace lrb

#endif  // LRB_BROWSER_UI_SNAP_CONNECTIONS_DIALOG_H_
