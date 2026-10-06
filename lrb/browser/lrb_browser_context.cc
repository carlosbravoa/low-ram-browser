// Copyright 2026 The low-ram-browser Authors
// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/lrb_browser_context.h"

#include "base/command_line.h"
#include "base/environment.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/nix/xdg_util.h"
#include "components/keyed_service/content/browser_context_dependency_manager.h"
#include "components/keyed_service/core/simple_dependency_manager.h"
#include "components/keyed_service/core/simple_factory_key.h"
#include "components/keyed_service/core/simple_key_map.h"
#include "components/origin_trials/browser/leveldb_persistence_provider.h"
#include "components/origin_trials/browser/origin_trials.h"
#include "content/public/browser/background_sync_controller.h"
#include "content/public/browser/background_sync_parameters.h"
#include "content/public/browser/background_sync_registration.h"
#include "content/public/browser/storage_partition.h"
#include "lrb/browser/download_manager_delegate.h"
#include "lrb/browser/permission_manager.h"
#include "lrb/browser/permissions/site_permissions.h"
#include "lrb/browser/zoom_levels.h"
#include "lrb/common/lrb_switches.h"
#include "third_party/blink/public/common/origin_trials/trial_token_validator.h"

namespace lrb {

namespace {

// Background sync never wakes the browser: the permission manager denies
// background sync, so this only answers content's bookkeeping. (One-shot
// syncs of an open page run as soon as possible, periodic syncs never.)
class BackgroundSyncController : public content::BackgroundSyncController {
 public:
  void ScheduleBrowserWakeUpWithDelay(blink::mojom::BackgroundSyncType type,
                                      base::TimeDelta delay) override {}
  void CancelBrowserWakeup(blink::mojom::BackgroundSyncType type) override {}
  base::TimeDelta GetNextEventDelay(
      const content::BackgroundSyncRegistration& registration,
      content::BackgroundSyncParameters* parameters,
      base::TimeDelta time_till_soonest_scheduled_event_for_origin) override {
    if (registration.sync_type() ==
        blink::mojom::BackgroundSyncType::PERIODIC) {
      return base::TimeDelta::Max();
    }
    const int attempts = registration.num_attempts();
    if (!attempts) {
      return base::TimeDelta();
    }
    return parameters->initial_retry_delay *
           pow(parameters->retry_delay_factor, attempts - 1);
  }
  std::unique_ptr<BackgroundSyncEventKeepAlive>
  CreateBackgroundSyncEventKeepAlive() override {
    return nullptr;
  }
  void NoteSuspendedPeriodicSyncOrigins(
      std::set<url::Origin> suspended_origins) override {}
  void NoteRegisteredPeriodicSyncOrigins(
      std::set<url::Origin> registered_origins) override {}
  void AddToTrackedOrigins(const url::Origin& origin) override {}
  void RemoveFromTrackedOrigins(const url::Origin& origin) override {}
};

}  // namespace

// static
void LrbBrowserContext::SetUpProfileDir(base::CommandLine& command_line) {
  base::FilePath path = command_line.GetSwitchValuePath(switches::kUserDataDir);
  if (path.empty()) {
    std::unique_ptr<base::Environment> env = base::Environment::Create();
    path = base::nix::GetXDGDirectory(env.get(), "XDG_DATA_HOME",
                                      ".local/share")
               .Append("lrb")
               .Append("standalone");
  }
  if (!base::CreateDirectory(path)) {
    LOG(WARNING) << "can't create the profile directory " << path;
  }
  if (const base::FilePath absolute = base::MakeAbsoluteFilePath(path);
      !absolute.empty()) {
    path = absolute;
  }
  command_line.AppendSwitchPath(switches::kUserDataDir, path);
}

// static
base::FilePath LrbBrowserContext::ProfileDir() {
  return base::CommandLine::ForCurrentProcess()->GetSwitchValuePath(
      switches::kUserDataDir);
}

LrbBrowserContext::LrbBrowserContext(bool off_the_record)
    : off_the_record_(off_the_record),
      path_(ProfileDir()),
      key_(std::make_unique<SimpleFactoryKey>(path_, off_the_record_)),
      answers_(std::make_unique<SitePermissions>(
          off_the_record ? base::FilePath()
                         : path_.Append("lrb-permissions.json"))),
      permission_manager_(std::make_unique<LrbPermissionManager>(*answers_)) {
  SimpleKeyMap::GetInstance()->Associate(this, key_.get());
  BrowserContextDependencyManager::GetInstance()->CreateBrowserContextServices(
      this);
}

LrbBrowserContext::~LrbBrowserContext() {
  // Before the storage partitions they observe (the order lrb had as a
  // subclass of content_shell's context).
  zoom_levels_.reset();
  download_delegate_.reset();
  NotifyWillBeDestroyed();
  DependencyManager::PerformInterlockedTwoPhaseShutdown(
      BrowserContextDependencyManager::GetInstance(), this,
      SimpleDependencyManager::GetInstance(), key_.get());
  SimpleKeyMap::GetInstance()->Dissociate(this);
  ShutdownStoragePartitions();
}

void LrbBrowserContext::StartZoomLevels() {
  if (!zoom_levels_ && !IsOffTheRecord()) {
    zoom_levels_ =
        std::make_unique<ZoomLevels>(this, GetPath().Append("lrb-zoom.json"));
  }
}

base::FilePath LrbBrowserContext::GetPath() const {
  return path_;
}

std::unique_ptr<content::ZoomLevelDelegate>
LrbBrowserContext::CreateZoomLevelDelegate(const base::FilePath&) {
  return nullptr;
}

bool LrbBrowserContext::IsOffTheRecord() {
  return off_the_record_;
}

content::DownloadManagerDelegate*
LrbBrowserContext::GetDownloadManagerDelegate() {
  if (!download_delegate_) {
    download_delegate_ =
        std::make_unique<LrbDownloadManagerDelegate>(GetDownloadManager());
  }
  return download_delegate_.get();
}

content::BrowserPluginGuestManager* LrbBrowserContext::GetGuestManager() {
  return nullptr;
}

storage::SpecialStoragePolicy* LrbBrowserContext::GetSpecialStoragePolicy() {
  return nullptr;
}

content::PlatformNotificationService*
LrbBrowserContext::GetPlatformNotificationService() {
  return nullptr;
}

content::PushMessagingService* LrbBrowserContext::GetPushMessagingService() {
  return nullptr;
}

content::StorageNotificationService*
LrbBrowserContext::GetStorageNotificationService() {
  return nullptr;
}

content::SSLHostStateDelegate* LrbBrowserContext::GetSSLHostStateDelegate() {
  return nullptr;
}

content::PermissionControllerDelegate*
LrbBrowserContext::GetPermissionControllerDelegate() {
  return permission_manager_.get();
}

content::BackgroundFetchDelegate*
LrbBrowserContext::GetBackgroundFetchDelegate() {
  return nullptr;
}

content::BackgroundSyncController*
LrbBrowserContext::GetBackgroundSyncController() {
  // Content expects one (BackgroundSyncProxy DCHECKs it).
  if (!background_sync_controller_) {
    background_sync_controller_ = std::make_unique<BackgroundSyncController>();
  }
  return background_sync_controller_.get();
}

content::BrowsingDataRemoverDelegate*
LrbBrowserContext::GetBrowsingDataRemoverDelegate() {
  return nullptr;
}

content::ClientHintsControllerDelegate*
LrbBrowserContext::GetClientHintsControllerDelegate() {
  return nullptr;
}

content::ReduceAcceptLanguageControllerDelegate*
LrbBrowserContext::GetReduceAcceptLanguageControllerDelegate() {
  // Accept-Language goes out as configured, not reduced per site.
  return nullptr;
}

content::OriginTrialsControllerDelegate*
LrbBrowserContext::GetOriginTrialsControllerDelegate() {
  if (!origin_trials_controller_delegate_) {
    origin_trials_controller_delegate_ =
        std::make_unique<origin_trials::OriginTrials>(
            std::make_unique<origin_trials::LevelDbPersistenceProvider>(
                GetPath(),
                GetDefaultStoragePartition()->GetProtoDatabaseProvider()),
            std::make_unique<blink::TrialTokenValidator>());
  }
  return origin_trials_controller_delegate_.get();
}

}  // namespace lrb
