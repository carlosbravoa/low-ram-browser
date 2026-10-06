// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/common/content_blocker.h"

#include <atomic>
#include <memory>
#include <tuple>

#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/files/memory_mapped_file.h"
#include "base/logging.h"
#include "base/numerics/byte_conversions.h"
#include "lrb/adblock/lib.rs.h"
#include "url/gurl.h"

namespace lrb {

namespace {

constexpr char kMagic[8] = {'L', 'R', 'B', 'A', 'D', 'B', '0', '1'};

// Set once at startup, then only read; never freed (the engine reads its
// rules from the mapping for the life of the process).
std::atomic<const adblock::Engine*> g_engine{nullptr};
std::atomic<bool> g_enabled{true};

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

// static
bool ContentBlocker::Load(const base::FilePath& given_path) {
  // base::File refuses paths with "..".
  const base::FilePath path = base::MakeAbsoluteFilePath(given_path);
  auto file = std::make_unique<base::MemoryMappedFile>();
  if (!file->Initialize(path)) {
    LOG(WARNING) << "lrb: no content blocking: can't map " << path;
    return false;
  }
  base::span<const uint8_t> bytes = file->bytes();
  if (bytes.size() < 16 || bytes.first<8>() != base::as_byte_span(kMagic)) {
    LOG(WARNING) << "lrb: no content blocking: " << path
                 << " isn't an lrb engine file";
    return false;
  }
  const uint64_t offset =
      base::U64FromLittleEndian(bytes.subspan<8, 8>());
  if (offset >= bytes.size()) {
    LOG(WARNING) << "lrb: no content blocking: corrupt " << path;
    return false;
  }
  base::span<const uint8_t> data = bytes.subspan(static_cast<size_t>(offset));
  // SAFETY: `file` is never unmapped (released below).
  rust::Box<adblock::Engine> engine = adblock::load_engine(
      rust::Slice<const uint8_t>(data.data(), data.size()));
  if (!engine->ok()) {
    LOG(WARNING) << "lrb: no content blocking: " << path << ": "
                 << std::string(engine->error());
    return false;
  }
  // Surrogates for $redirect rules (ListUpdater writes them next to the
  // engine). Small: ~50 scripts and images.
  std::string resources;
  if (base::ReadFileToString(
          path.AddExtension(FILE_PATH_LITERAL("resources.json")), &resources) &&
      !engine->use_resources_json(rust::Str(resources.data(), resources.size()))) {
    LOG(WARNING) << "lrb: content blocking without surrogates: unreadable "
                    "resources file";
  }
  // Both live for the rest of the process: the engine reads its rules from
  // the mapping.
  std::ignore = file.release();
  g_engine.store(engine.into_raw(), std::memory_order_release);
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
std::string ContentBlocker::CosmeticCss(const GURL& url) {
  const adblock::Engine* engine =
      enabled() ? g_engine.load(std::memory_order_acquire) : nullptr;
  if (!engine || !url.SchemeIsHTTPOrHTTPS()) {
    return std::string();
  }
  const std::string& spec = url.spec();
  return std::string(engine->cosmetic_css(rust::Str(spec.data(), spec.size())));
}

// static
std::string ContentBlocker::Scriptlets(const GURL& url) {
  const adblock::Engine* engine =
      enabled() ? g_engine.load(std::memory_order_acquire) : nullptr;
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
      enabled() ? g_engine.load(std::memory_order_acquire) : nullptr;
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
