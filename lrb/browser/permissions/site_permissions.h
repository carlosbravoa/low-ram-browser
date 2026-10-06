// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_PERMISSIONS_SITE_PERMISSIONS_H_
#define LRB_BROWSER_PERMISSIONS_SITE_PERMISSIONS_H_

#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/values.h"
#include "third_party/blink/public/common/permissions/permission_utils.h"
#include "url/origin.h"

namespace lrb {

// The permissions lrb asks the user about: ones a person can judge, that
// lrb can serve. Everything else is denied without asking (location: no
// location source on Linux without Google's API key; notifications:
// nothing to show them with).
struct AskablePermission {
  blink::PermissionType type;
  const char* key;             // in the saved file
  const char16_t* request;     // "<site> wants to ..."
  const char16_t* name;        // in the menu
};
const AskablePermission* FindAskable(blink::PermissionType type);

// The user's answers, per origin, for one profile: in the site's own
// profile directory (lrb-permissions.json), so a site's permissions live
// and go with it. Loaded in the background; until then nothing is
// remembered (the user is asked). An off-the-record profile keeps them in
// memory only.
class SitePermissions {
 public:
  // `file` empty: in memory only.
  explicit SitePermissions(const base::FilePath& file);
  SitePermissions(const SitePermissions&) = delete;
  SitePermissions& operator=(const SitePermissions&) = delete;
  ~SitePermissions();

  // The remembered answer, if any.
  std::optional<bool> Get(const url::Origin& origin,
                          blink::PermissionType type) const;
  void Set(const url::Origin& origin, blink::PermissionType type, bool allowed);
  void Forget(const url::Origin& origin, blink::PermissionType type);
  base::WeakPtr<SitePermissions> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

  // Remembered answers for `origin`, for the menu.
  std::vector<std::pair<blink::PermissionType, bool>> List(
      const url::Origin& origin) const;

 private:
  void OnLoaded(std::optional<std::string> json);
  void Save();

  const base::FilePath file_;
  // Saves run in order.
  const scoped_refptr<base::SequencedTaskRunner> file_task_runner_;
  // {"https://example.com": {"camera": true, ...}, ...}
  base::DictValue answers_;
  base::WeakPtrFactory<SitePermissions> weak_factory_{this};
};

}  // namespace lrb

#endif  // LRB_BROWSER_PERMISSIONS_SITE_PERMISSIONS_H_
