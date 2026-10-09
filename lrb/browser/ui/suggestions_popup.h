// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_UI_SUGGESTIONS_POPUP_H_
#define LRB_BROWSER_UI_SUGGESTIONS_POPUP_H_

#include <memory>
#include <optional>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "lrb/browser/bookmarks.h"

namespace views {
class View;
class Widget;
}  // namespace views

namespace lrb {

// The address bar's suggestions (bookmarks, visited sites): a list under the
// address field while typing. A popup window that never takes focus (typing
// stays in the field), made when shown and destroyed when hidden, so it
// costs nothing the rest of the time.
class SuggestionsPopup {
 public:
  // `anchor`: the address field. `open`: a suggestion was clicked.
  SuggestionsPopup(views::View* anchor,
                   base::RepeatingCallback<void(GURL)> open);
  SuggestionsPopup(const SuggestionsPopup&) = delete;
  SuggestionsPopup& operator=(const SuggestionsPopup&) = delete;
  ~SuggestionsPopup();

  // Shows `suggestions` (none: hides), nothing selected.
  void Show(std::vector<Suggestion> suggestions);
  void Hide();
  bool visible() const { return !!widget_; }

  // Up (-1) or Down (+1) through the list; the selected one, if any.
  const Suggestion* Move(int delta);

 private:
  void Build();

  raw_ptr<views::View> anchor_;
  base::RepeatingCallback<void(GURL)> open_;
  std::vector<Suggestion> suggestions_;
  std::optional<size_t> selected_;
  std::unique_ptr<views::Widget> widget_;
};

}  // namespace lrb

#endif  // LRB_BROWSER_UI_SUGGESTIONS_POPUP_H_
