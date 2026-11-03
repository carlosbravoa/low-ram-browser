// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/release_check.h"

#include <utility>

#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/storage_partition.h"
#include "net/http/http_request_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "url/gurl.h"

namespace lrb {

namespace {

// The newest release first. Not `releases/latest`: that is the newest
// release that is neither a draft nor a prerelease, and every lrb release
// so far is a prerelease (alpha), so it answered 404.
constexpr char kLatestReleaseUrl[] =
    "https://api.github.com/repos/carlosbravoa/low-ram-browser/"
    "releases?per_page=1";

// GitHub's answer is a few kB; anything much bigger isn't it.
constexpr size_t kMaxAnswer = 256 * 1024;

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("lrb_release_check", R"(
      semantics {
        sender: "lrb release check"
        description: "Asks GitHub which lrb release is the newest, so the "
          "window's bar can say when an update is available."
        trigger: "lrb_coordinator, at most once a day, after the filter "
          "lists are up to date."
        data: "None: a plain GET of GitHub's public release information, "
          "no cookies."
        destination: OTHER
      }
      policy {
        cookies_allowed: NO
        setting: "lrb_coordinator --no-release-check turns it off."
      })");

// Release versions as lrb tags them: digits, letters, '.', '-' and '+'
// ("0.1.0-alpha.7"). Anything else isn't written where the bar reads it.
bool PlausibleVersion(const std::string& version) {
  if (version.empty() || version.size() > 64 ||
      !base::IsAsciiDigit(version[0])) {
    return false;
  }
  for (char c : version) {
    if (!(base::IsAsciiAlphaNumeric(c) || c == '.' || c == '-' || c == '+')) {
      return false;
    }
  }
  return true;
}

// Next to the file, then renamed: whoever reads it never sees half of it.
bool WriteAtomically(base::FilePath file, std::string text) {
  const base::FilePath tmp = file.AddExtension(FILE_PATH_LITERAL("tmp"));
  if (!base::CreateDirectory(file.DirName()) || !base::WriteFile(tmp, text) ||
      !base::ReplaceFile(tmp, file, nullptr)) {
    base::DeleteFile(tmp);
    return false;
  }
  return true;
}

}  // namespace

ReleaseCheck::ReleaseCheck(content::BrowserContext* browser_context,
                           base::FilePath file,
                           base::OnceClosure done)
    : browser_context_(browser_context),
      file_(std::move(file)),
      done_(std::move(done)) {}

ReleaseCheck::~ReleaseCheck() = default;

void ReleaseCheck::Start() {
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = GURL(kLatestReleaseUrl);
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->headers.SetHeader(net::HttpRequestHeaders::kAccept,
                             "application/vnd.github+json");
  loader_ = network::SimpleURLLoader::Create(std::move(request),
                                             kTrafficAnnotation);
  loader_->SetRetryOptions(
      2, network::SimpleURLLoader::RETRY_ON_NETWORK_CHANGE |
             network::SimpleURLLoader::RETRY_ON_NAME_NOT_RESOLVED);
  loader_->DownloadToString(
      browser_context_->GetDefaultStoragePartition()
          ->GetURLLoaderFactoryForBrowserProcess()
          .get(),
      base::BindOnce(&ReleaseCheck::OnDownloaded, weak_factory_.GetWeakPtr()),
      kMaxAnswer);
}

// static
std::optional<std::string> ReleaseCheck::ParseLatestRelease(
    const std::string& json) {
  std::optional<base::Value> answer =
      base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  if (!answer) {
    return std::nullopt;
  }
  // A list of releases, newest first (or one release, as `releases/latest`
  // and `releases/tags/<tag>` answer).
  const base::DictValue* release = answer->GetIfDict();
  if (const base::ListValue* list = answer->GetIfList()) {
    release = list->empty() ? nullptr : list->front().GetIfDict();
  }
  const std::string* tag = release ? release->FindString("tag_name") : nullptr;
  if (!tag) {
    return std::nullopt;
  }
  std::string version = *tag;
  if (version.starts_with('v') || version.starts_with('V')) {
    version.erase(0, 1);
  }
  if (!PlausibleVersion(version)) {
    return std::nullopt;
  }
  return version;
}

void ReleaseCheck::OnDownloaded(std::optional<std::string> body) {
  loader_.reset();
  std::optional<std::string> version =
      body ? ParseLatestRelease(*body) : std::nullopt;
  if (!version) {
    LOG(WARNING) << "lrb: no answer about the latest release";
    std::move(done_).Run();
    return;
  }
  VLOG(1) << "lrb: the latest release is " << *version;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskShutdownBehavior::BLOCK_SHUTDOWN},
      base::BindOnce(&WriteAtomically, file_, *version),
      base::BindOnce(&ReleaseCheck::OnWritten, weak_factory_.GetWeakPtr()));
}

void ReleaseCheck::OnWritten(bool ok) {
  if (!ok) {
    LOG(WARNING) << "lrb: can't write " << file_;
  }
  std::move(done_).Run();
}

}  // namespace lrb
