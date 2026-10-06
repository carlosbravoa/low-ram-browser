// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_UI_LOGIN_PROMPT_H_
#define LRB_BROWSER_UI_LOGIN_PROMPT_H_

#include <memory>

#include "content/public/browser/login_delegate.h"

namespace content {
class WebContents;
}

namespace net {
class AuthChallengeInfo;
}

class GURL;

namespace lrb {

// A site asking for a username and password (HTTP authentication):
// "example.com asks you to sign in", in the window's question row.
// content_shell cancelled it. Only the page itself, or a request to its
// own site, may ask: a sign-in prompt from another site's embedded content
// is a classic phishing trick, so it is refused without asking, as are
// proxy challenges. The server's "realm" text isn't shown (it says
// whatever the server likes). Returns null when it won't ask (cancelled).
std::unique_ptr<content::LoginDelegate> AskToSignIn(
    const net::AuthChallengeInfo& auth_info,
    content::WebContents* web_contents,
    bool is_request_for_primary_main_frame_navigation,
    const GURL& url,
    content::LoginDelegate::LoginAuthRequiredCallback callback);

}  // namespace lrb

#endif  // LRB_BROWSER_UI_LOGIN_PROMPT_H_
