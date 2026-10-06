// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/permission_manager.h"

#include <optional>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "content/public/browser/page.h"
#include "content/public/browser/permission_request_description.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "lrb/browser/permissions/site_permissions.h"
#include "lrb/browser/ui/lrb_platform_delegate.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace lrb {

namespace {

using blink::mojom::PermissionStatus;

blink::PermissionType TypeOf(
    const blink::mojom::PermissionDescriptorPtr& permission) {
  return blink::PermissionDescriptorToPermissionType(permission);
}

// The page's top-level origin, if `frame` belongs to the page on screen.
std::optional<url::Origin> TopLevelOrigin(content::RenderFrameHost* frame) {
  if (!frame || !frame->IsActive() || !frame->GetPage().IsPrimary()) {
    return std::nullopt;  // being left, cached or prerendered: never asks
  }
  return frame->GetMainFrame()->GetLastCommittedOrigin();
}

}  // namespace

LrbPermissionManager::LrbPermissionManager(SitePermissions& answers)
    : answers_(answers) {}

LrbPermissionManager::~LrbPermissionManager() = default;

PermissionStatus LrbPermissionManager::Status(blink::PermissionType type,
                                              const url::Origin& requesting,
                                              const url::Origin& top_level) {
  // Writing to the clipboard (sanitized, and only with user activation:
  // Blink checks that) is what "copy" buttons do; it reveals nothing.
  if (type == blink::PermissionType::CLIPBOARD_SANITIZED_WRITE) {
    return PermissionStatus::GRANTED;
  }
  if (!FindAskable(type) || requesting != top_level || requesting.opaque()) {
    return PermissionStatus::DENIED;
  }
  const std::optional<bool> answer = answers_->Get(top_level, type);
  if (!answer) {
    return PermissionStatus::ASK;
  }
  return *answer ? PermissionStatus::GRANTED : PermissionStatus::DENIED;
}

void LrbPermissionManager::Decide(
    content::RenderFrameHost* frame,
    std::vector<blink::PermissionType> types,
    bool focus,
    base::OnceCallback<void(const std::vector<PermissionStatus>&)> callback) {
  const std::optional<url::Origin> top_level = TopLevelOrigin(frame);
  std::vector<PermissionStatus> statuses;
  std::vector<blink::PermissionType> to_ask;
  for (blink::PermissionType type : types) {
    statuses.push_back(
        top_level
            ? Status(type, frame->GetLastCommittedOrigin(), *top_level)
            : PermissionStatus::DENIED);
    if (statuses.back() == PermissionStatus::ASK) {
      to_ask.push_back(type);
    }
  }
  if (to_ask.empty()) {
    std::move(callback).Run(statuses);
    return;
  }
  // One question for all of them ("camera and microphone"). No answer
  // (the page went away, the window closed) denies, and isn't remembered.
  LrbPlatformDelegate::AskPermission(
      content::WebContents::FromRenderFrameHost(frame), *top_level, to_ask,
      focus,
      base::BindOnce(
          [](base::WeakPtr<SitePermissions> answers, url::Origin origin,
             std::vector<blink::PermissionType> asked,
             std::vector<PermissionStatus> statuses,
             base::OnceCallback<void(const std::vector<PermissionStatus>&)>
                 callback,
             std::optional<bool> allowed) {
            if (allowed && answers) {
              for (blink::PermissionType type : asked) {
                answers->Set(origin, type, *allowed);
              }
            }
            for (PermissionStatus& status : statuses) {
              if (status == PermissionStatus::ASK) {
                status = allowed.value_or(false) ? PermissionStatus::GRANTED
                                                 : PermissionStatus::DENIED;
              }
            }
            std::move(callback).Run(statuses);
          },
          answers_->GetWeakPtr(), *top_level, to_ask, statuses,
          std::move(callback)));
}

void LrbPermissionManager::RequestPermissionsFromCurrentDocument(
    content::RenderFrameHost* render_frame_host,
    const content::PermissionRequestDescription& request_description,
    base::OnceCallback<void(const std::vector<content::PermissionResult>&)>
        callback) {
  std::vector<blink::PermissionType> types;
  for (const auto& permission : request_description.permissions) {
    types.push_back(TypeOf(permission));
  }
  Decide(render_frame_host, std::move(types), request_description.user_gesture,
         base::BindOnce(
             [](base::OnceCallback<void(
                    const std::vector<content::PermissionResult>&)> callback,
                const std::vector<PermissionStatus>& statuses) {
               std::vector<content::PermissionResult> results;
               for (PermissionStatus status : statuses) {
                 results.emplace_back(status);
               }
               std::move(callback).Run(results);
             },
             std::move(callback)));
}

PermissionStatus LrbPermissionManager::GetPermissionStatus(
    const blink::mojom::PermissionDescriptorPtr& permission,
    const GURL& requesting_origin,
    const GURL& embedding_origin) {
  return Status(TypeOf(permission), url::Origin::Create(requesting_origin),
                url::Origin::Create(embedding_origin));
}

content::PermissionResult
LrbPermissionManager::GetPermissionResultForOriginWithoutContext(
    const blink::mojom::PermissionDescriptorPtr& permission,
    const url::Origin& requesting_origin,
    const url::Origin& embedding_origin) {
  return content::PermissionResult(
      Status(TypeOf(permission), requesting_origin, embedding_origin));
}

content::PermissionResult
LrbPermissionManager::GetPermissionResultForCurrentDocument(
    const blink::mojom::PermissionDescriptorPtr& permission,
    content::RenderFrameHost* render_frame_host,
    bool should_include_device_status) {
  const std::optional<url::Origin> top_level =
      TopLevelOrigin(render_frame_host);
  if (!top_level) {
    return content::PermissionResult(PermissionStatus::DENIED);
  }
  return content::PermissionResult(
      Status(TypeOf(permission), render_frame_host->GetLastCommittedOrigin(),
             *top_level));
}

content::PermissionResult LrbPermissionManager::GetPermissionResultForWorker(
    const blink::mojom::PermissionDescriptorPtr& permission,
    content::RenderProcessHost* render_process_host,
    const GURL& worker_origin) {
  // Workers have no window to ask in: only what the user already allowed.
  const url::Origin origin = url::Origin::Create(worker_origin);
  const PermissionStatus status = Status(TypeOf(permission), origin, origin);
  return content::PermissionResult(status == PermissionStatus::GRANTED
                                       ? status
                                       : PermissionStatus::DENIED);
}

content::PermissionResult
LrbPermissionManager::GetPermissionResultForEmbeddedRequester(
    const blink::mojom::PermissionDescriptorPtr& permission,
    content::RenderFrameHost* render_frame_host,
    const url::Origin& requesting_origin) {
  return content::PermissionResult(PermissionStatus::DENIED);
}

void LrbPermissionManager::ResetPermission(blink::PermissionType permission,
                                           const GURL& requesting_origin,
                                           const GURL& embedding_origin) {
  answers_->Forget(url::Origin::Create(embedding_origin), permission);
}

}  // namespace lrb
