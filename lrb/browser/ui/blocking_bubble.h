// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_UI_BLOCKING_BUBBLE_H_
#define LRB_BROWSER_UI_BLOCKING_BUBBLE_H_

#include <optional>
#include <string>

#include "base/functional/callback_forward.h"

namespace views {
class View;
}

namespace lrb {

// What the shield with the count means: a small bubble under it (`anchor`)
// with the requests blocked on the page (`blocked`), whether blocking is on
// for `site` (`enabled`; nullopt: the filter lists aren't loaded yet, so
// nothing to switch), a button that turns it off or on (`toggle`, as the
// menu's item) and one that opens the settings (`settings`). Closes on
// Escape, a click outside, and its close button.
void ShowBlockingBubble(views::View* anchor,
                        int blocked,
                        const std::string& site,
                        std::optional<bool> enabled,
                        base::OnceClosure toggle,
                        base::OnceClosure settings);

}  // namespace lrb

#endif  // LRB_BROWSER_UI_BLOCKING_BUBBLE_H_
