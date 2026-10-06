// Copyright 2026 The low-ram-browser Authors
// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_LRB_BROWSER_CONTEXT_H_
#define LRB_BROWSER_LRB_BROWSER_CONTEXT_H_

#include <memory>

#include "base/files/file_path.h"
#include "content/public/browser/browser_context.h"

class SimpleFactoryKey;

namespace base {
class CommandLine;
}

namespace lrb {

class LrbDownloadManagerDelegate;
class LrbPermissionManager;
class SitePermissions;
class ZoomLevels;

// The profile: one site's (the coordinator gives each site its own,
// --user-data-dir). lrb's permission manager and downloads, the user's
// permission answers and zoom levels kept in it.
//
// From content_shell's ShellBrowserContext, without its test mocks (its
// background sync controller and Accept-Language reduction were
// content/test mocks).
class LrbBrowserContext : public content::BrowserContext {
 public:
  explicit LrbBrowserContext(bool off_the_record);
  LrbBrowserContext(const LrbBrowserContext&) = delete;
  LrbBrowserContext& operator=(const LrbBrowserContext&) = delete;
  ~LrbBrowserContext() override;

  // The profile directory: --user-data-dir, else
  // $XDG_DATA_HOME/lrb/standalone (lrb started without the coordinator).
  // At startup, while blocking file access is allowed: creates it and puts
  // its absolute path on the command line, where ProfileDir() reads it.
  static void SetUpProfileDir(base::CommandLine& command_line);
  static base::FilePath ProfileDir();

  // Restores the user's zoom levels and keeps them (needs the storage
  // partition: called once the browser is up).
  void StartZoomLevels();

  // content::BrowserContext:
  base::FilePath GetPath() const override;
  std::unique_ptr<content::ZoomLevelDelegate> CreateZoomLevelDelegate(
      const base::FilePath& partition_path) override;
  bool IsOffTheRecord() override;
  content::DownloadManagerDelegate* GetDownloadManagerDelegate() override;
  content::BrowserPluginGuestManager* GetGuestManager() override;
  storage::SpecialStoragePolicy* GetSpecialStoragePolicy() override;
  content::PlatformNotificationService* GetPlatformNotificationService()
      override;
  content::PushMessagingService* GetPushMessagingService() override;
  content::StorageNotificationService* GetStorageNotificationService()
      override;
  content::SSLHostStateDelegate* GetSSLHostStateDelegate() override;
  content::PermissionControllerDelegate* GetPermissionControllerDelegate()
      override;
  content::BackgroundFetchDelegate* GetBackgroundFetchDelegate() override;
  content::BackgroundSyncController* GetBackgroundSyncController() override;
  content::BrowsingDataRemoverDelegate* GetBrowsingDataRemoverDelegate()
      override;
  content::ClientHintsControllerDelegate* GetClientHintsControllerDelegate()
      override;
  content::ReduceAcceptLanguageControllerDelegate*
  GetReduceAcceptLanguageControllerDelegate() override;
  content::OriginTrialsControllerDelegate* GetOriginTrialsControllerDelegate()
      override;

 private:
  const bool off_the_record_;
  const base::FilePath path_;
  std::unique_ptr<SimpleFactoryKey> key_;

  std::unique_ptr<SitePermissions> answers_;
  std::unique_ptr<LrbPermissionManager> permission_manager_;
  std::unique_ptr<LrbDownloadManagerDelegate> download_delegate_;
  std::unique_ptr<ZoomLevels> zoom_levels_;
  std::unique_ptr<content::BackgroundSyncController>
      background_sync_controller_;
  std::unique_ptr<content::OriginTrialsControllerDelegate>
      origin_trials_controller_delegate_;
};

}  // namespace lrb

#endif  // LRB_BROWSER_LRB_BROWSER_CONTEXT_H_
