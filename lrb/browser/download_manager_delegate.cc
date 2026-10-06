// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/download_manager_delegate.h"

#include <string>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/nix/xdg_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "components/download/public/common/download_target_info.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/download_item_utils.h"
#include "content/public/browser/web_contents.h"
#include "lrb/browser/shell.h"
#include "lrb/browser/ui/file_picker.h"
#include "lrb/browser/ui/lrb_platform_delegate.h"
#include "lrb/browser/ui/window_view.h"
#include "net/base/filename_util.h"

namespace lrb {

namespace {

// The user's Downloads folder (XDG), with `item`'s name: the picker's
// suggestion. Blocking: reads user-dirs.dirs, may create the folder.
base::FilePath SuggestedPath(const GURL& url,
                             const std::string& content_disposition,
                             const std::string& suggested_name,
                             const std::string& mime_type) {
  base::FilePath folder =
      base::nix::GetXDGUserDirectory("DOWNLOAD", "Downloads");
  // Without a configured Downloads folder (user-dirs.dirs: a minimal
  // system), the lookup answers the home folder itself.
  if (folder == base::GetHomeDir()) {
    folder = folder.Append("Downloads");
  }
  base::CreateDirectory(folder);
  return folder.Append(net::GenerateFileName(url, content_disposition,
                                             std::string(), suggested_name,
                                             mime_type, "download"));
}

Shell* ShellOf(download::DownloadItem* item) {
  content::WebContents* contents =
      content::DownloadItemUtils::GetWebContents(item);
  return contents ? Shell::FromWebContents(contents) : nullptr;
}

void RunTarget(download::DownloadTargetCallback callback,
               const base::FilePath& path) {
  download::DownloadTargetInfo target;
  target.target_path = path;  // empty: cancelled
  if (!path.empty()) {
    // Written under a temporary name, renamed when complete: a half
    // download never sits under the real name.
    target.intermediate_path = path.AddExtension(FILE_PATH_LITERAL("crdownload"));
    // The picker already asked about replacing an existing file.
    target.target_disposition =
        download::DownloadItem::TARGET_DISPOSITION_OVERWRITE;
  }
  std::move(callback).Run(std::move(target));
}

constexpr char kSavedWithoutAsking[] = "lrb-saved-without-asking";

}  // namespace

LrbDownloadManagerDelegate::LrbDownloadManagerDelegate(
    content::DownloadManager* manager)
    : manager_(manager) {
  manager_->AddObserver(this);
}

LrbDownloadManagerDelegate::~LrbDownloadManagerDelegate() {
  // Destroyed with the browser context's members, before content's part of
  // it shuts the download manager down: detach, or it would call us then.
  if (manager_) {
    manager_->SetDelegate(nullptr);
  }
  Shutdown();
}

// static
void LrbDownloadManagerDelegate::Cancel(content::BrowserContext* context,
                                        uint32_t id) {
  if (download::DownloadItem* item =
          context->GetDownloadManager()->GetDownload(id)) {
    item->Cancel(/*user_cancel=*/true);
  }
}

void LrbDownloadManagerDelegate::Shutdown() {
  items_.RemoveAllObservations();
  if (manager_) {
    manager_->RemoveObserver(this);
    manager_ = nullptr;
  }
}

void LrbDownloadManagerDelegate::GetNextId(
    content::DownloadIdCallback callback) {
  // No download history is kept: ids only need to be unique per run.
  static uint32_t next_id = download::DownloadItem::kInvalidId + 1;
  std::move(callback).Run(next_id++);
}

bool LrbDownloadManagerDelegate::DetermineDownloadTarget(
    download::DownloadItem* item,
    download::DownloadTargetCallback* callback) {
  if (!item->GetForcedFilePath().empty()) {
    RunTarget(std::move(*callback), item->GetForcedFilePath());
    return true;
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(&SuggestedPath, item->GetURL(),
                     item->GetContentDisposition(),
                     item->GetSuggestedFilename(), item->GetMimeType()),
      base::BindOnce(&LrbDownloadManagerDelegate::OnNameGenerated,
                     weak_factory_.GetWeakPtr(), item->GetId(),
                     std::move(*callback)));
  return true;
}

void LrbDownloadManagerDelegate::OnNameGenerated(
    uint32_t id,
    download::DownloadTargetCallback callback,
    const base::FilePath& suggested) {
  download::DownloadItem* item = manager_ ? manager_->GetDownload(id) : nullptr;
  if (!item) {
    RunTarget(std::move(callback), base::FilePath());
    return;
  }
  Shell* shell = ShellOf(item);
  PickFiles(
      ui::SelectFileDialog::SELECT_SAVEAS_FILE, u"Save file", suggested,
      shell ? shell->window() : gfx::NativeWindow(),
      base::BindOnce(
          [](base::WeakPtr<LrbDownloadManagerDelegate> self, uint32_t id,
             base::FilePath suggested,
             download::DownloadTargetCallback callback, PickResult result,
             std::vector<base::FilePath> paths) {
            switch (result) {
              case PickResult::kChosen:
                // Through the coordinator without a picker: it chose the
                // Downloads folder.
                if (std::optional<BrokeredSave> save =
                        GetBrokeredSave(paths.front());
                    save && save->automatic && self && self->manager_) {
                  if (download::DownloadItem* item =
                          self->manager_->GetDownload(id)) {
                    item->SetUserData(
                        kSavedWithoutAsking,
                        std::make_unique<base::SupportsUserData::Data>());
                  }
                }
                RunTarget(std::move(callback), paths.front());
                return;
              case PickResult::kCanceled:
                RunTarget(std::move(callback), base::FilePath());
                return;
              case PickResult::kUnavailable:
                break;
            }
            // Nothing to ask with: the Downloads folder, under a free name;
            // the bar says where.
            if (self && self->manager_) {
              if (download::DownloadItem* item =
                      self->manager_->GetDownload(id)) {
                item->SetUserData(
                    kSavedWithoutAsking,
                    std::make_unique<base::SupportsUserData::Data>());
              }
            }
            base::ThreadPool::PostTaskAndReplyWithResult(
                FROM_HERE,
                {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
                 base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
                base::BindOnce(&base::GetUniquePath, suggested),
                base::BindOnce(&RunTarget, std::move(callback)));
          },
          weak_factory_.GetWeakPtr(), id, suggested, std::move(callback)));
}

bool LrbDownloadManagerDelegate::ShouldOpenDownload(
    download::DownloadItem* item,
    content::DownloadOpenDelayedCallback callback) {
  return true;  // nothing to do before opening (lrb never opens on its own)
}

void LrbDownloadManagerDelegate::OnDownloadCreated(
    content::DownloadManager* manager,
    download::DownloadItem* item) {
  items_.AddObservation(item);
  OnDownloadUpdated(item);
}

void LrbDownloadManagerDelegate::ManagerGoingDown(
    content::DownloadManager* manager) {
  Shutdown();
}

void LrbDownloadManagerDelegate::OnDownloadUpdated(
    download::DownloadItem* item) {
  Shell* shell = ShellOf(item);
  WindowView* view = shell ? LrbPlatformDelegate::ViewFor(shell) : nullptr;
  if (!view || item->GetTargetFilePath().empty()) {
    return;  // its window is gone, or no name yet (being asked)
  }
  WindowView::DownloadStatus status;
  status.id = item->GetId();
  status.name = item->GetTargetFilePath().BaseName().LossyDisplayName();
  status.path = item->GetTargetFilePath();
  // Written in this profile for the coordinator to move (file_picker.h): the
  // bar shows where it goes.
  const std::optional<BrokeredSave> brokered =
      GetBrokeredSave(item->GetTargetFilePath());
  if (brokered) {
    status.path = brokered->target;
    status.broker_id = brokered->id;
  }
  status.percent = item->PercentComplete();
  status.saved_without_asking = item->GetUserData(kSavedWithoutAsking);
  switch (item->GetState()) {
    case download::DownloadItem::IN_PROGRESS:
      status.state = WindowView::DownloadStatus::State::kInProgress;
      break;
    case download::DownloadItem::COMPLETE:
      if (brokered && brokered->state == BrokeredSave::State::kMoved) {
        status.state = WindowView::DownloadStatus::State::kDone;
      } else if (brokered &&
                 brokered->state == BrokeredSave::State::kFailed) {
        status.state = WindowView::DownloadStatus::State::kFailed;
      } else if (brokered) {
        // Done once the coordinator has moved it into place.
        status.state = WindowView::DownloadStatus::State::kInProgress;
        FinishBrokeredSave(
            item->GetTargetFilePath(),
            base::BindOnce(
                [](base::WeakPtr<WindowView> view,
                   WindowView::DownloadStatus status, bool moved) {
                  status.state =
                      moved ? WindowView::DownloadStatus::State::kDone
                            : WindowView::DownloadStatus::State::kFailed;
                  if (view) {
                    view->ShowDownload(status);
                  }
                },
                view->GetWeakPtr(), status));
      } else {
        status.state = WindowView::DownloadStatus::State::kDone;
      }
      break;
    case download::DownloadItem::CANCELLED:
      CancelBrokeredSave(item->GetTargetFilePath());
      status.state = WindowView::DownloadStatus::State::kCanceled;
      break;
    case download::DownloadItem::INTERRUPTED:
      CancelBrokeredSave(item->GetTargetFilePath());
      status.state = WindowView::DownloadStatus::State::kFailed;
      break;
    case download::DownloadItem::MAX_DOWNLOAD_STATE:
      return;
  }
  view->ShowDownload(status);
}

void LrbDownloadManagerDelegate::OnDownloadDestroyed(
    download::DownloadItem* item) {
  items_.RemoveObservation(item);
}

}  // namespace lrb
