// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_DOWNLOAD_MANAGER_DELEGATE_H_
#define LRB_BROWSER_DOWNLOAD_MANAGER_DELEGATE_H_

#include <cstdint>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_multi_source_observation.h"
#include "components/download/public/common/download_item.h"
#include "content/public/browser/download_manager.h"
#include "content/public/browser/download_manager_delegate.h"

namespace content {
class BrowserContext;
}

namespace lrb {

// Downloads ask where to save, with the system file picker (decided
// 2026-10-04), suggesting the Downloads folder and the file's name. With
// no picker to ask (no session bus or portal: a kiosk, a bare Pi), they go
// to the Downloads folder under a free name, and the bar says where.
// content_shell cancelled every download on Linux. Progress shows in the
// bar of the window the download came from.
class LrbDownloadManagerDelegate : public content::DownloadManagerDelegate,
                                   public content::DownloadManager::Observer,
                                   public download::DownloadItem::Observer {
 public:
  explicit LrbDownloadManagerDelegate(content::DownloadManager* manager);
  LrbDownloadManagerDelegate(const LrbDownloadManagerDelegate&) = delete;
  LrbDownloadManagerDelegate& operator=(const LrbDownloadManagerDelegate&) =
      delete;
  ~LrbDownloadManagerDelegate() override;

  // From the bar's download menu.
  static void Cancel(content::BrowserContext* context, uint32_t id);

  // content::DownloadManagerDelegate:
  void Shutdown() override;
  void GetNextId(content::DownloadIdCallback callback) override;
  bool DetermineDownloadTarget(
      download::DownloadItem* item,
      download::DownloadTargetCallback* callback) override;
  bool ShouldOpenDownload(
      download::DownloadItem* item,
      content::DownloadOpenDelayedCallback callback) override;

  // content::DownloadManager::Observer:
  void OnDownloadCreated(content::DownloadManager* manager,
                         download::DownloadItem* item) override;
  void ManagerGoingDown(content::DownloadManager* manager) override;

  // download::DownloadItem::Observer:
  void OnDownloadUpdated(download::DownloadItem* item) override;
  void OnDownloadDestroyed(download::DownloadItem* item) override;

 private:
  void OnNameGenerated(uint32_t id,
                       download::DownloadTargetCallback callback,
                       const base::FilePath& suggested);

  raw_ptr<content::DownloadManager> manager_;
  base::ScopedMultiSourceObservation<download::DownloadItem,
                                     download::DownloadItem::Observer>
      items_{this};
  base::WeakPtrFactory<LrbDownloadManagerDelegate> weak_factory_{this};
};

}  // namespace lrb

#endif  // LRB_BROWSER_DOWNLOAD_MANAGER_DELEGATE_H_
