// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_PERMISSION_MANAGER_H_
#define LRB_BROWSER_PERMISSION_MANAGER_H_

#include <vector>

#include "base/functional/callback_forward.h"
#include "base/memory/raw_ref.h"
#include "content/public/browser/permission_controller_delegate.h"
#include "content/public/browser/permission_result.h"
#include "third_party/blink/public/common/permissions/permission_utils.h"

namespace url {
class Origin;
}

namespace lrb {

class SitePermissions;

// No site gets a permission the user hasn't given (decided 2026-10-04).
// content_shell's manager granted location, sensors, ... to every site
// without asking.
//
// - The few permissions a person can judge and lrb can serve (camera,
//   microphone, clipboard reading: AskablePermission) are asked for in
//   the window, and the answer is remembered per origin (SitePermissions).
// - Only the page's own origin may ask: requests from frames of another
//   origin are denied.
// - Everything else is denied without asking, except writing to the
//   clipboard after a click (copy buttons): it reveals nothing and Chrome
//   doesn't ask either.
class LrbPermissionManager : public content::PermissionControllerDelegate {
 public:
  explicit LrbPermissionManager(SitePermissions& answers);
  LrbPermissionManager(const LrbPermissionManager&) = delete;
  LrbPermissionManager& operator=(const LrbPermissionManager&) = delete;
  ~LrbPermissionManager() override;

  SitePermissions& answers() { return *answers_; }

  // The decision for `types` requested by the document in `frame`: the
  // remembered answers, else the user's, asked in its window. `focus`: the
  // request follows a click, so the prompt may take keyboard focus.
  void Decide(content::RenderFrameHost* frame,
              std::vector<blink::PermissionType> types,
              bool focus,
              base::OnceCallback<void(
                  const std::vector<blink::mojom::PermissionStatus>&)>
                  callback);

  // Without asking: GRANTED, DENIED, or ASK (askable, no answer yet).
  blink::mojom::PermissionStatus Status(blink::PermissionType type,
                                        const url::Origin& requesting,
                                        const url::Origin& top_level);

  // content::PermissionControllerDelegate:
  void RequestPermissionsFromCurrentDocument(
      content::RenderFrameHost* render_frame_host,
      const content::PermissionRequestDescription& request_description,
      base::OnceCallback<void(const std::vector<content::PermissionResult>&)>
          callback) override;
  blink::mojom::PermissionStatus GetPermissionStatus(
      const blink::mojom::PermissionDescriptorPtr& permission,
      const GURL& requesting_origin,
      const GURL& embedding_origin) override;
  content::PermissionResult GetPermissionResultForOriginWithoutContext(
      const blink::mojom::PermissionDescriptorPtr& permission,
      const url::Origin& requesting_origin,
      const url::Origin& embedding_origin) override;
  content::PermissionResult GetPermissionResultForCurrentDocument(
      const blink::mojom::PermissionDescriptorPtr& permission,
      content::RenderFrameHost* render_frame_host,
      bool should_include_device_status) override;
  content::PermissionResult GetPermissionResultForWorker(
      const blink::mojom::PermissionDescriptorPtr& permission,
      content::RenderProcessHost* render_process_host,
      const GURL& worker_origin) override;
  content::PermissionResult GetPermissionResultForEmbeddedRequester(
      const blink::mojom::PermissionDescriptorPtr& permission,
      content::RenderFrameHost* render_frame_host,
      const url::Origin& requesting_origin) override;
  void ResetPermission(blink::PermissionType permission,
                       const GURL& requesting_origin,
                       const GURL& embedding_origin) override;

 private:
  const raw_ref<SitePermissions> answers_;
};

}  // namespace lrb

#endif  // LRB_BROWSER_PERMISSION_MANAGER_H_
