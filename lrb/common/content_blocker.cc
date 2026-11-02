// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/common/content_blocker.h"

#include <atomic>
#include <memory>
#include <set>
#include <tuple>

#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/files/memory_mapped_file.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/numerics/byte_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/synchronization/lock.h"
#include "lrb/adblock/lib.rs.h"
#include "lrb/browser/site.h"
#include "url/gurl.h"

namespace lrb {

namespace {

constexpr char kMagic[8] = {'L', 'R', 'B', 'A', 'D', 'B', '0', '1'};

// Set at startup, or later when the lists were just built; never freed (the engine reads its
// rules from the mapping for the life of the process).
std::atomic<const adblock::Engine*> g_engine{nullptr};
std::atomic<bool> g_enabled{true};

// Standard mode: the sites blocking is off for. Read on every request from
// the network's thread, changed on the UI thread. Most of the time empty:
// the flag spares requests the lock and the site lookup.
std::atomic<bool> g_any_disabled{false};

base::Lock& DisabledLock() {
  static base::NoDestructor<base::Lock> lock;
  return *lock;
}

std::set<std::string>& DisabledSet() {
  static base::NoDestructor<std::set<std::string>> sites;
  return *sites;
}

// With DisabledLock() held.
void ReplaceDisabled(std::set<std::string> sites) {
  g_any_disabled.store(!sites.empty(), std::memory_order_release);
  DisabledSet() = std::move(sites);
}

// Sites as the coordinator and SiteForUrl write them: lowercase letters,
// digits, '-', '.', and ':' or '[' ']' for IP addresses. Anything else in
// a list is skipped.
bool PlausibleSite(std::string_view site) {
  if (site.empty() || site.size() > 253) {
    return false;
  }
  for (char c : site) {
    if (!(base::IsAsciiLower(c) || base::IsAsciiDigit(c) || c == '-' ||
          c == '.' || c == ':' || c == '[' || c == ']')) {
      return false;
    }
  }
  return true;
}

std::set<std::string> ParseSites(std::string_view text, const char* separators) {
  std::set<std::string> sites;
  for (std::string_view site : base::SplitStringPiece(
           text, separators, base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    if (PlausibleSite(site)) {
      sites.emplace(site);
    }
  }
  return sites;
}

const char* RequestType(network::mojom::RequestDestination destination) {
  using D = network::mojom::RequestDestination;
  switch (destination) {
    case D::kScript:
    case D::kWorker:
    case D::kSharedWorker:
    case D::kServiceWorker:
    case D::kAudioWorklet:
    case D::kPaintWorklet:
      return "script";
    case D::kImage:
      return "image";
    case D::kStyle:
    case D::kXslt:
      return "stylesheet";
    case D::kFont:
      return "font";
    case D::kIframe:
    case D::kFrame:
    case D::kFencedframe:
      return "sub_frame";
    case D::kDocument:
      return "document";
    case D::kAudio:
    case D::kVideo:
    case D::kTrack:
      return "media";
    case D::kObject:
    case D::kEmbed:
      return "object";
    case D::kEmpty:  // fetch(), XMLHttpRequest, beacons
      return "xmlhttprequest";
    default:
      return "other";
  }
}

}  // namespace

const adblock::Engine* LoadEngineFile(const base::FilePath& given_path,
                                      bool with_resources) {
  // base::File refuses paths with "..".
  const base::FilePath path = base::MakeAbsoluteFilePath(given_path);
  auto file = std::make_unique<base::MemoryMappedFile>();
  if (!file->Initialize(path)) {
    LOG(WARNING) << "lrb: can't map " << path;
    return nullptr;
  }
  base::span<const uint8_t> bytes = file->bytes();
  if (bytes.size() < 16 || bytes.first<8>() != base::as_byte_span(kMagic)) {
    LOG(WARNING) << "lrb: " << path << " isn't an lrb engine file";
    return nullptr;
  }
  const uint64_t offset =
      base::U64FromLittleEndian(bytes.subspan<8, 8>());
  if (offset >= bytes.size()) {
    LOG(WARNING) << "lrb: corrupt " << path;
    return nullptr;
  }
  base::span<const uint8_t> data = bytes.subspan(static_cast<size_t>(offset));
  // SAFETY: `file` is never unmapped (released below).
  rust::Box<adblock::Engine> engine = adblock::load_engine(
      rust::Slice<const uint8_t>(data.data(), data.size()));
  if (!engine->ok()) {
    LOG(WARNING) << "lrb: " << path << ": " << std::string(engine->error());
    return nullptr;
  }
  // Surrogates for $redirect rules (ListUpdater writes them next to the
  // engine). Small: ~50 scripts and images.
  std::string resources;
  if (with_resources &&
      base::ReadFileToString(
          path.AddExtension(FILE_PATH_LITERAL("resources.json")), &resources) &&
      !engine->use_resources_json(rust::Str(resources.data(), resources.size()))) {
    LOG(WARNING) << "lrb: content blocking without surrogates: unreadable "
                    "resources file";
  }
  // Both live for the rest of the process: the engine reads its rules from
  // the mapping.
  std::ignore = file.release();
  return engine.into_raw();
}

// static
bool ContentBlocker::Load(const base::FilePath& path) {
  const adblock::Engine* engine = LoadEngineFile(path, /*with_resources=*/true);
  if (!engine) {
    LOG(WARNING) << "lrb: no content blocking";
    return false;
  }
  g_engine.store(engine, std::memory_order_release);
  VLOG(1) << "lrb: content blocking with " << path;
  return true;
}

// static
bool ContentBlocker::loaded() {
  return g_engine.load(std::memory_order_acquire) != nullptr;
}

// static
void ContentBlocker::SetEnabled(bool enabled) {
  g_enabled.store(enabled, std::memory_order_release);
}

// static
bool ContentBlocker::enabled() {
  return g_enabled.load(std::memory_order_acquire);
}

// static
bool ContentBlocker::EnabledFor(const GURL& page_url) {
  if (!enabled()) {
    return false;
  }
  if (!g_any_disabled.load(std::memory_order_acquire)) {
    return true;
  }
  const std::string site = SiteForUrl(page_url);
  if (site.empty()) {
    return true;
  }
  base::AutoLock lock(DisabledLock());
  return !DisabledSet().contains(site);
}

// static
void ContentBlocker::SetSiteEnabled(const std::string& site, bool enabled) {
  if (!PlausibleSite(site)) {
    return;
  }
  base::AutoLock lock(DisabledLock());
  std::set<std::string> sites = DisabledSet();
  if (enabled) {
    sites.erase(site);
  } else {
    sites.insert(site);
  }
  ReplaceDisabled(std::move(sites));
}

// static
std::vector<std::string> ContentBlocker::DisabledSites() {
  base::AutoLock lock(DisabledLock());
  return std::vector<std::string>(DisabledSet().begin(), DisabledSet().end());
}

// static
void ContentBlocker::SetDisabledSitesFromSwitch(const std::string& value) {
  std::set<std::string> sites = ParseSites(value, ",");
  base::AutoLock lock(DisabledLock());
  ReplaceDisabled(std::move(sites));
}

// static
void ContentBlocker::ReadDisabledSites(const base::FilePath& file) {
  std::string text;
  if (file.empty() || !base::ReadFileToStringWithMaxSize(file, &text, 1 << 20)) {
    return;  // none yet
  }
  std::set<std::string> sites = ParseSites(text, "\n");
  base::AutoLock lock(DisabledLock());
  ReplaceDisabled(std::move(sites));
}

// static
bool ContentBlocker::WriteDisabledSites(const base::FilePath& file) {
  std::string text;
  for (const std::string& site : DisabledSites()) {
    text += site + "\n";
  }
  // Next to it, then renamed: a crash never leaves half a list.
  const base::FilePath tmp = file.AddExtension(FILE_PATH_LITERAL("tmp"));
  if (!base::WriteFile(tmp, text) || !base::ReplaceFile(tmp, file, nullptr)) {
    base::DeleteFile(tmp);
    return false;
  }
  return true;
}

// static
std::string ContentBlocker::CosmeticCss(const GURL& url) {
  const adblock::Engine* engine =
      EnabledFor(url) ? g_engine.load(std::memory_order_acquire) : nullptr;
  if (!engine || !url.SchemeIsHTTPOrHTTPS()) {
    return std::string();
  }
  const std::string& spec = url.spec();
  return std::string(engine->cosmetic_css(rust::Str(spec.data(), spec.size())));
}

// static
std::string ContentBlocker::Scriptlets(const GURL& url) {
  const adblock::Engine* engine =
      EnabledFor(url) ? g_engine.load(std::memory_order_acquire) : nullptr;
  if (!engine || !url.SchemeIsHTTPOrHTTPS()) {
    return std::string();
  }
  const std::string& spec = url.spec();
  return std::string(
      engine->injected_script(rust::Str(spec.data(), spec.size())));
}

// static
ContentBlocker::Decision ContentBlocker::Check(
    const GURL& url,
    const GURL& source,
    network::mojom::RequestDestination destination,
    const std::string& method) {
  Decision decision;
  const adblock::Engine* engine =
      EnabledFor(source) ? g_engine.load(std::memory_order_acquire) : nullptr;
  if (!engine || !url.SchemeIsHTTPOrHTTPS()) {
    return decision;
  }
  const std::string& spec = url.spec();
  const std::string& source_spec = source.spec();
  const adblock::CheckResult result = engine->check(
      rust::Str(spec.data(), spec.size()),
      rust::Str(source_spec.data(), source_spec.size()),
      rust::Str(RequestType(destination)),
      rust::Str(method.data(), method.size()));
  decision.block = result.block;
  decision.redirect = std::string(result.redirect);
  return decision;
}

}  // namespace lrb
