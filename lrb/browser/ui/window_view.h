// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_UI_WINDOW_VIEW_H_
#define LRB_BROWSER_UI_WINDOW_VIEW_H_

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/callback_list.h"
#include "base/containers/circular_deque.h"
#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "third_party/blink/public/common/permissions/permission_utils.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/menus/simple_menu_model.h"
#include "ui/views/controls/textfield/textfield_controller.h"
#include "ui/views/view.h"
#include "url/gurl.h"

namespace content {
class WebContents;
}  // namespace content

namespace views {
class ImageButton;
class ImageView;
class Label;
class LabelButton;
class MdTextButton;
class Textfield;
class MenuRunner;
class WebView;
}  // namespace views

namespace lrb {

class AddressField;
class Shell;
class ShortcutHandler;

// A window's contents: lrb's slim bar above the page. One 36 px row: back,
// forward, reload/stop, the site chip (the page's site, in a colour derived
// from it) with the rest of the address beside it (click either to edit),
// the count of requests blocked on this page, and a menu. Native Views, not
// HTML: a web-page bar measured 6.5 MB per window against 2.7 MB.
//
// With 2+ tabs, a thin row of them under the bar. The bar shows the active
// tab (its Shell); the delegate (LrbPlatformDelegate) owns the tabs and
// tells which to show. Shells own themselves and outlive their time here.
class WindowView : public views::View,
                   public views::TextfieldController,
                   public ui::SimpleMenuModel::Delegate {
  METADATA_HEADER(WindowView, views::View)

 public:
  // What the tab row and the tab shortcuts do (the delegate's tabs).
  struct TabActions {
    TabActions();
    TabActions(TabActions&&);
    TabActions& operator=(TabActions&&);
    ~TabActions();
    base::RepeatingCallback<void(size_t)> select;
    base::RepeatingCallback<void(int)> cycle;  // +1 next, -1 previous
    base::RepeatingCallback<void(size_t)> close;
    base::RepeatingClosure new_tab;
  };
  explicit WindowView(TabActions actions);
  WindowView(const WindowView&) = delete;
  WindowView& operator=(const WindowView&) = delete;
  ~WindowView() override;

  // Shows `shell`'s page, the bar following it. `back_url`: the page of
  // another site its tab was opened from (Back on its first page returns
  // there), or empty. `first_size`: the window's first page, which sizes it.
  void ShowTab(Shell* shell,
               const GURL& back_url,
               std::optional<gfx::Size> first_size = std::nullopt);
  // The shown page is going away (its tab closes): nothing is shown until
  // the next ShowTab().
  void PageClosed();
  Shell* shell() const { return shell_; }
  // The tab row: titles, and which tab is shown. Hidden with one tab.
  void SetTabs(const std::vector<std::u16string>& titles, size_t active);
  // Where a new tab starts: the site's home page.
  GURL NewTabUrl() const;
  void SetUrl(const GURL& url);
  void SetLoading(bool loading);
  void EnableBack(bool enabled);
  const GURL& back_url() const { return back_url_; }
  void EnableForward(bool enabled);

  // A question for the user in a row under the bar (inside the window: no
  // popup to cost memory or get lost): permission questions and the page's
  // dialogs (alert, confirm, prompt, "leave this page?"). One at a time, in
  // order.
  struct Question {
    enum class Focus { kNone, kAccept, kCancel, kInput };
    Question();
    Question(Question&&);
    Question& operator=(Question&&);
    ~Question();
    std::u16string text;
    std::u16string accept_label;  // "Allow", "OK", "Leave"
    std::u16string cancel_label;  // "Block", "Cancel"; empty: none (alert)
    bool input = false;           // prompt(): a text field
    std::u16string default_input;
    // A sign-in (HTTP authentication): username and password fields; the
    // answer goes to `credentials` instead of `answer`.
    bool login = false;
    base::OnceCallback<void(
        std::optional<std::pair<std::u16string, std::u16string>>)>
        credentials;
    // Whether and where the row takes keyboard focus.
    Focus focus = Focus::kNone;
    // Who asked, to find or cancel its questions (the dialog manager).
    raw_ptr<const void> owner = nullptr;
    // Accepted or not, with the text field's text; nothing if it went away
    // unanswered (the page navigated, the window closed).
    base::OnceCallback<void(std::optional<bool> accepted,
                            const std::u16string& input)>
        answer;
  };
  void Ask(Question question);

  // A permission question ("example.com wants to use your camera"), Block or
  // Allow. `focus`: the request follows a click, so the row may take
  // keyboard focus, on Block: a stray Enter never allows.
  void AskPermission(const std::u16string& question,
                     bool focus,
                     base::OnceCallback<void(std::optional<bool>)> answer);

  // `owner`'s questions: whether it has any, answering the one on screen
  // (false if it isn't `owner`'s), dropping them all unanswered.
  bool HasQuestionsFrom(const void* owner) const;
  bool AnswerFrom(const void* owner,
                  bool accept,
                  const std::u16string* input);
  void DropQuestionsFrom(const void* owner);

  // A download from this window: its progress in the bar (the latest one).
  struct DownloadStatus {
    enum class State { kInProgress, kDone, kCanceled, kFailed };
    uint32_t id = 0;
    std::u16string name;
    base::FilePath path;
    int percent = -1;  // unknown
    State state = State::kInProgress;
    // No picker to ask with: saved to the Downloads folder.
    bool saved_without_asking = false;
    // Saved through the coordinator (confined): its id there, to show the
    // folder (the file manager must not start inside the confinement).
    std::string broker_id;
  };
  void ShowDownload(const DownloadStatus& status);

  // A camera/microphone stream started or stopped: shown in the bar.
  void AddCapture(bool audio, bool video);
  void RemoveCapture(bool audio, bool video);

  base::WeakPtr<WindowView> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

  // Where a search for `terms` goes, with the user's search engine
  // (typed in the bar, or a selection): `done` gets it.
  static void Search(std::u16string terms,
                     base::OnceCallback<void(GURL)> done);

  // Results of find in page for the shown tab's page.
  void OnFindReply(int matches, int active, bool final_update);

  // Window shortcuts, seen before the page sees the key. True if handled.
  bool HandleShortcut(const ui::KeyEvent& event);

 private:
  enum Command {
    kNewPage = 1,
    kClosePage,
    kCloseSite,
    kCancelDownload,
    kShowDownload,
    kDismissDownload,
    kToggleBlocking,
    kSettings,
    kForgetFirst = 100,
  };
  void ShowDownloadMenu();
  // Content blocking off (or back on) for this site, then reload.
  void ToggleBlocking();

  // Find in page: a row under the bar.
  void OpenFind();
  void CloseFind();
  void Find(bool forward, bool new_session);

  // Zoom: in steps (+1, -1), 0 resets. Kept per host (ZoomLevels).
  void Zoom(int direction);
  void UpdateZoom();

  void ShowNextQuestion();
  static void Unanswered(Question& question);
  void FocusQuestion();
  void Answer(std::optional<bool> accepted);
  void DropQuestions();
  void UpdateCapture();

  // Back in this site's history, or, from its first page, to the page of
  // another site this window was opened from.
  void GoBack();
  void FocusAddress();
  void Navigate(std::u16string_view text);
  void SetBlocked(int count);
  void ShowMenu();
  void NewPage();
  void CloseSoon();

  // views::View:
  gfx::Size GetMinimumSize() const override;
  void AddedToWidget() override;

  // views::TextfieldController:
  bool HandleKeyEvent(views::Textfield* sender,
                      const ui::KeyEvent& key_event) override;
  void ContentsChanged(views::Textfield* sender,
                       const std::u16string& new_contents) override;

  // ui::SimpleMenuModel::Delegate:
  void ExecuteCommand(int command_id, int event_flags) override;
  bool GetAcceleratorForCommandId(int command_id,
                                  ui::Accelerator* accelerator) const override;

  void SetWebContents(content::WebContents* web_contents,
                      std::optional<gfx::Size> size);
  void ClearPage();

  TabActions actions_;
  raw_ptr<Shell> shell_ = nullptr;
  GURL url_;
  std::string site_;
  GURL back_url_;
  bool loading_ = false;

  raw_ptr<views::ImageButton> back_ = nullptr;
  raw_ptr<views::ImageButton> forward_ = nullptr;
  raw_ptr<views::ImageButton> reload_ = nullptr;
  raw_ptr<views::LabelButton> chip_ = nullptr;
  raw_ptr<AddressField> address_ = nullptr;
  raw_ptr<views::LabelButton> blocked_ = nullptr;
  raw_ptr<views::LabelButton> download_ = nullptr;
  std::optional<DownloadStatus> download_status_;
  ui::SimpleMenuModel download_menu_model_{this};
  raw_ptr<views::View> tab_row_ = nullptr;
  raw_ptr<views::View> question_row_ = nullptr;
  raw_ptr<views::Label> question_label_ = nullptr;
  raw_ptr<views::Textfield> question_input_ = nullptr;
  raw_ptr<views::Textfield> question_password_ = nullptr;
  raw_ptr<views::MdTextButton> cancel_ = nullptr;
  raw_ptr<views::MdTextButton> accept_ = nullptr;
  raw_ptr<views::ImageView> camera_in_use_ = nullptr;
  raw_ptr<views::ImageView> microphone_in_use_ = nullptr;
  raw_ptr<views::View> find_row_ = nullptr;
  raw_ptr<views::Textfield> find_input_ = nullptr;
  raw_ptr<views::Label> find_count_ = nullptr;
  raw_ptr<views::LabelButton> zoom_ = nullptr;
  raw_ptr<views::View> contents_ = nullptr;
  raw_ptr<views::WebView> web_view_ = nullptr;

  base::circular_deque<Question> questions_;
  int audio_captures_ = 0;
  int video_captures_ = 0;
  // Remembered permission answers listed in the menu, by command id.
  std::vector<blink::PermissionType> forget_;

  ui::SimpleMenuModel menu_model_{this};
  std::unique_ptr<views::MenuRunner> menu_runner_;
  std::unique_ptr<ShortcutHandler> shortcuts_;
  int find_request_id_ = 0;
  base::CallbackListSubscription zoom_subscription_;

  base::WeakPtrFactory<WindowView> weak_factory_{this};
};

}  // namespace lrb

#endif  // LRB_BROWSER_UI_WINDOW_VIEW_H_
