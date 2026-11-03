// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_RELEASE_CHECK_H_
#define LRB_BROWSER_RELEASE_CHECK_H_

#include <memory>
#include <optional>
#include <string>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"

namespace content {
class BrowserContext;
}

namespace network {
class SimpleURLLoader;
}

namespace lrb {

// `lrb --lrb-check-release=<file>`: no window; asks GitHub for lrb's newest
// release and writes its version ("0.1.0-alpha.7", without the tag's "v")
// to the file, replacing it atomically, then exits. On any failure the file
// stays as it was. lrb_coordinator runs it at most once a day and passes
// the answer to the instances it launches (--lrb-latest-release), so the
// bar can offer the update; like the filter lists, the coordinator itself
// needs no network code.
class ReleaseCheck {
 public:
  ReleaseCheck(content::BrowserContext* browser_context,
               base::FilePath file,
               base::OnceClosure done);
  ReleaseCheck(const ReleaseCheck&) = delete;
  ReleaseCheck& operator=(const ReleaseCheck&) = delete;
  ~ReleaseCheck();

  void Start();

  // The version of the first release in GitHub's answer (a list, newest
  // first, or one release): its tag_name without a leading "v". Nothing if
  // the answer has none that looks like a version.
  static std::optional<std::string> ParseLatestRelease(const std::string& json);

 private:
  void OnDownloaded(std::optional<std::string> body);
  void OnWritten(bool ok);

  const raw_ptr<content::BrowserContext> browser_context_;
  const base::FilePath file_;
  base::OnceClosure done_;
  std::unique_ptr<network::SimpleURLLoader> loader_;
  base::WeakPtrFactory<ReleaseCheck> weak_factory_{this};
};

}  // namespace lrb

#endif  // LRB_BROWSER_RELEASE_CHECK_H_
