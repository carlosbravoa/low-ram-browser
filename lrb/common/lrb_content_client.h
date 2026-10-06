// Copyright 2026 The low-ram-browser Authors
// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_COMMON_LRB_CONTENT_CLIENT_H_
#define LRB_COMMON_LRB_CONTENT_CLIENT_H_

#include <string>
#include <string_view>

#include "components/embedder_support/origin_trials/origin_trial_policy_impl.h"
#include "content/public/common/content_client.h"

namespace lrb {

// Strings, resources and the origin trial policy, for every process. From
// content_shell's ShellContentClient; origin trials use Chromium's
// production policy (Chrome's public key): content_shell's accepted tokens
// signed with the test key whose private half is in Chromium's source.
class LrbContentClient : public content::ContentClient {
 public:
  LrbContentClient();
  LrbContentClient(const LrbContentClient&) = delete;
  LrbContentClient& operator=(const LrbContentClient&) = delete;
  ~LrbContentClient() override;

  // content::ContentClient:
  std::u16string GetLocalizedString(int message_id) override;
  std::string_view GetDataResource(
      int resource_id,
      ui::ResourceScaleFactor scale_factor) override;
  scoped_refptr<base::RefCountedMemory> GetDataResourceBytes(
      int resource_id) override;
  std::string GetDataResourceString(int resource_id) override;
  gfx::Image& GetNativeImageNamed(int resource_id) override;
  blink::OriginTrialPolicy* GetOriginTrialPolicy() override;

 private:
  embedder_support::OriginTrialPolicyImpl origin_trial_policy_;
};

}  // namespace lrb

#endif  // LRB_COMMON_LRB_CONTENT_CLIENT_H_
