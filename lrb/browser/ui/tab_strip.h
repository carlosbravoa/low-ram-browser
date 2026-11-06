// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_UI_TAB_STRIP_H_
#define LRB_BROWSER_UI_TAB_STRIP_H_

#include <stddef.h>

#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/views/view.h"

namespace lrb {

// A window's tabs: one row above the bar, shown with 2+ tabs (WindowView
// decides). The strip is a shade darker than the bar. The shown tab is a
// card in the bar's own colour, its top corners rounded and its foot on
// the bar, so tab and bar read as one surface; a line in the site's colour
// (the chip's) runs along its top. The other tabs are flat on the strip,
// with a hairline between neighbours and a soft pill under the pointer.
// Each tab: the page's icon, its title, a close button. After the last
// one, a button for a new tab.
//
// Tabs share the width: 220 px each while there is room, narrower
// together when there isn't; under 100 px only the shown tab keeps its
// close button, the title taking its room.
// A click shows a tab, a middle-click closes it. The keyboard's tab
// shortcuts are the window's (shortcut_handler.h), not the strip's.
//
// Native Views, painted here: no images, nothing per tab beyond its three
// child views.
class TabStrip : public views::View {
  METADATA_HEADER(TabStrip, views::View)

 public:
  static constexpr int kHeight = 34;

  struct Tab {
    std::u16string title;
    gfx::ImageSkia icon;  // the page's (favicon.h); empty: none
  };

  // `select` and `close` take the tab's index.
  TabStrip(base::RepeatingCallback<void(size_t)> select,
           base::RepeatingCallback<void(size_t)> close,
           base::RepeatingClosure new_tab);
  TabStrip(const TabStrip&) = delete;
  TabStrip& operator=(const TabStrip&) = delete;
  ~TabStrip() override;

  // Replaces the tabs. `active`: the shown one.
  void SetTabs(std::vector<Tab> tabs, size_t active);
  size_t tab_count() const { return tab_count_; }

  // The colour of the line on the shown tab: its page's site's.
  // SK_ColorTRANSPARENT: no line (a page with no site).
  void SetAccentColor(SkColor color);

 private:
  // Writes where the tabs and the buttons are in the window to the log
  // (VLOG 1), for the tests that click them.
  void LogPositions();

  base::RepeatingCallback<void(size_t)> select_;
  base::RepeatingCallback<void(size_t)> close_;
  base::RepeatingClosure new_tab_;
  size_t tab_count_ = 0;
  SkColor accent_ = SK_ColorTRANSPARENT;
  raw_ptr<views::View> active_tab_ = nullptr;  // a StripTab (tab_strip.cc)

  base::WeakPtrFactory<TabStrip> weak_factory_{this};
};

}  // namespace lrb

#endif  // LRB_BROWSER_UI_TAB_STRIP_H_
