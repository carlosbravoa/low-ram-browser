// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/list_updater.h"

#include <utility>

#include "base/base64.h"
#include "base/compiler_specific.h"
#include "base/json/json_reader.h"
#include "base/strings/strcat.h"
#include "base/values.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/referrer.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_util.h"
#include "services/network/public/cpp/data_element.h"
#include "services/network/public/cpp/resource_request_body.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/cpp/url_loader_factory_builder.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "ui/base/page_transition_types.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/task/thread_pool.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/storage_partition.h"
#include "lrb/adblock/lib.rs.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "url/gurl.h"

namespace lrb {

namespace {

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("lrb_filter_lists", R"(
      semantics {
        sender: "lrb content blocking"
        description: "Downloads the public ad and tracker filter lists "
          "(EasyList, EasyPrivacy, uBlock Origin) that lrb blocks with."
        trigger: "lrb_coordinator, when the compiled lists are missing or "
          "more than a day old."
        data: "None: plain GETs of public files, no cookies."
        destination: OTHER
      }
      policy {
        cookies_allowed: NO
        setting: "Content blocking is off without the engine file."
      })");

// Compiles and writes the engine file (thread pool: CPU and disk).
bool CompileAndWrite(std::vector<std::string> lists,
                     bool lean,
                     base::FilePath out) {
  rust::Vec<rust::String> rust_lists;
  for (const std::string& list : lists) {
    rust_lists.push_back(rust::String(list));
  }
  // uBlock Origin trusts its own lists (they may use scriptlets that need
  // trust, e.g. YouTube's ad stripping); EasyList and EasyPrivacy are not.
  rust::Vec<bool> trusted;
  for (const std::string& url : ListUpdater::ListUrls(lean)) {
    trusted.push_back(url.starts_with("https://ublockorigin.github.io/"));
  }
  const rust::Vec<uint8_t> file =
      adblock::compile_engine_file(rust_lists, trusted, lean);
  // Write next to the target, then rename: instances map the file, so it
  // must never be seen half-written.
  const base::FilePath tmp = out.AddExtension(FILE_PATH_LITERAL("tmp"));
  if (!base::CreateDirectory(out.DirName()) ||
      // SAFETY: a rust::Vec's data() and size() describe one allocation.
      !base::WriteFile(tmp, UNSAFE_BUFFERS(base::span(file.data(), file.size()))) ||
      !base::ReplaceFile(tmp, out, nullptr)) {
    base::DeleteFile(tmp);
    return false;
  }
  return true;
}

constexpr char kResourceBase[] =
    "https://raw.githubusercontent.com/gorhill/uBlock/master/src/";

base::FilePath ResourcesFile(const base::FilePath& engine_file) {
  return engine_file.AddExtension(FILE_PATH_LITERAL("resources.json"));
}

// Writes the fetched surrogate files to a scratch directory and assembles
// them into adblock-rust resources JSON (thread pool).
std::optional<std::string> AssembleSurrogates(std::string map,
                                              std::vector<std::string> names,
                                              std::vector<std::string> files,
                                              base::FilePath out) {
  const base::FilePath scratch =
      out.DirName().Append(FILE_PATH_LITERAL("resources-scratch"));
  base::DeletePathRecursively(scratch);
  const base::FilePath dir = scratch.Append(FILE_PATH_LITERAL("war"));
  const base::FilePath map_file = scratch.Append(FILE_PATH_LITERAL("map.js"));
  bool ok = base::CreateDirectory(dir) && base::WriteFile(map_file, map);
  for (size_t i = 0; ok && i < names.size(); ++i) {
    ok = base::WriteFile(dir.AppendASCII(names[i]), files[i]);
  }
  std::optional<std::string> json;
  if (ok) {
    json = std::string(adblock::assemble_resources_json(
        rust::Str(dir.value()), rust::Str(map_file.value())));
  }
  base::DeletePathRecursively(scratch);
  return json;
}

bool WriteAtomically(base::FilePath out, std::string data) {
  const base::FilePath tmp = out.AddExtension(FILE_PATH_LITERAL("tmp"));
  return base::WriteFile(tmp, data) && base::ReplaceFile(tmp, out, nullptr);
}

// Joins two JSON arrays (each "[...]", possibly empty).
std::string JoinJsonArrays(const std::string& a, const std::string& b) {
  const std::string_view inner_a = std::string_view(a).substr(1, a.size() - 2);
  const std::string_view inner_b = std::string_view(b).substr(1, b.size() - 2);
  if (inner_a.empty()) {
    return b;
  }
  if (inner_b.empty()) {
    return a;
  }
  return base::StrCat({"[", inner_a, ",", inner_b, "]"});
}

constexpr char kScriptletModule[] =
    "https://cdn.jsdelivr.net/gh/gorhill/uBlock@master/src/js/resources/"
    "scriptlets.js";
constexpr char kResultHost[] = "lrb-updater.invalid";

// The invisible page that converts uBlock Origin's scriptlets into
// adblock-rust resources. Scriptlets that need a trusted list get a
// permission our lists don't have; isolated-world ones are skipped (lrb
// runs scriptlets in the main world).
std::string ScriptletPage() {
  const std::string script = base::StrCat({R"(
import {builtinScriptlets} from ')", kScriptletModule, R"(';
const b64 = s => {
  let bin = '';
  for (const b of new TextEncoder().encode(s)) bin += String.fromCharCode(b);
  return btoa(bin);
};
const out = [];
for (const s of builtinScriptlets) {
  if (s.world === 'ISOLATED') continue;
  // Callable scriptlets (*.js) are application/javascript: adblock-rust
  // finds the function name and calls it with the rule's arguments.
  // Helpers (*.fn) are fn/javascript: only usable as dependencies.
  const kind = s.name.endsWith('.fn') ? 'fn/javascript' : 'application/javascript';
  const r = {name: s.name, aliases: s.aliases || [],
             kind: {mime: kind}, content: b64(s.fn.toString())};
  if (s.dependencies && s.dependencies.length) r.dependencies = s.dependencies;
  if (s.requiresTrust) r.permission = 1;
  out.push(r);
}
fetch('https://)", kResultHost, R"(/scriptlets',
      {method: 'POST', body: JSON.stringify(out)});
)"});
  return "data:text/html;base64," +
         base::Base64Encode("<script type=module>" + script + "</script>");
}

ListUpdater* g_updater = nullptr;

// Answers the scriptlet page's POST locally and hands its body to the
// updater; forwards everything else.
class ResultCatcher : public network::mojom::URLLoaderFactory {
 public:
  ResultCatcher(mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
                mojo::PendingRemote<network::mojom::URLLoaderFactory> target,
                base::RepeatingCallback<void(std::optional<std::string>)> done)
      : target_(std::move(target)), done_(std::move(done)) {
    receivers_.Add(this, std::move(receiver));
    receivers_.set_disconnect_handler(base::BindRepeating(
        &ResultCatcher::MaybeDelete, base::Unretained(this)));
  }

  void CreateLoaderAndStart(
      mojo::PendingReceiver<network::mojom::URLLoader> loader,
      int32_t request_id,
      uint32_t options,
      const network::ResourceRequest& request,
      mojo::PendingRemote<network::mojom::URLLoaderClient> client,
      const net::MutableNetworkTrafficAnnotationTag& annotation) override {
    if (request.url.host() != kResultHost) {
      target_->CreateLoaderAndStart(std::move(loader), request_id, options,
                                    request, std::move(client), annotation);
      return;
    }
    std::optional<std::string> body;
    if (request.request_body && !request.request_body->elements()->empty()) {
      const network::DataElement& element =
          request.request_body->elements()->front();
      if (element.type() == network::DataElement::Tag::kBytes) {
        body = std::string(
            element.As<network::DataElementBytes>().AsStringView());
      }
    }
    mojo::Remote<network::mojom::URLLoaderClient> remote(std::move(client));
    auto head = network::mojom::URLResponseHead::New();
    head->headers = base::MakeRefCounted<net::HttpResponseHeaders>(
        net::HttpUtil::AssembleRawHeaders(
            "HTTP/1.1 204 No Content\nAccess-Control-Allow-Origin: *\n"));
    // An empty body: a response without one isn't complete.
    mojo::ScopedDataPipeProducerHandle producer;
    mojo::ScopedDataPipeConsumerHandle consumer;
    if (mojo::CreateDataPipe(1, producer, consumer) != MOJO_RESULT_OK) {
      done_.Run(std::move(body));
      return;
    }
    remote->OnReceiveResponse(std::move(head), std::move(consumer),
                              std::nullopt);
    remote->OnComplete(network::URLLoaderCompletionStatus(net::OK));
    done_.Run(std::move(body));
  }

  void Clone(
      mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver) override {
    receivers_.Add(this, std::move(receiver));
  }

 private:
  void MaybeDelete() {
    if (receivers_.empty()) {
      delete this;
    }
  }

  mojo::ReceiverSet<network::mojom::URLLoaderFactory> receivers_;
  mojo::Remote<network::mojom::URLLoaderFactory> target_;
  base::RepeatingCallback<void(std::optional<std::string>)> done_;
};

}  // namespace

// static
void ListUpdater::MaybeInterceptRequests(
    network::URLLoaderFactoryBuilder& builder) {
  if (!g_updater) {
    return;
  }
  auto [receiver, target] = builder.Append();
  new ResultCatcher(  // deletes itself
      std::move(receiver), std::move(target),
      base::BindRepeating(&ListUpdater::OnScriptlets,
                          g_updater->weak_factory_.GetWeakPtr()));
}

// static
std::vector<std::string> ListUpdater::ListUrls(bool lean) {
  std::vector<std::string> urls = {
      "https://easylist.to/easylist/easylist.txt",
      "https://easylist.to/easylist/easyprivacy.txt",
      "https://ublockorigin.github.io/uAssets/filters/filters.txt",
  };
  if (!lean) {
    urls.insert(urls.end(), {
        "https://ublockorigin.github.io/uAssets/filters/privacy.txt",
        "https://ublockorigin.github.io/uAssets/filters/unbreak.txt",
        "https://ublockorigin.github.io/uAssets/filters/quick-fixes.txt",
        "https://ublockorigin.github.io/uAssets/filters/badware.txt",
    });
  }
  return urls;
}

ListUpdater::ListUpdater(content::BrowserContext* browser_context,
                         base::FilePath engine_file,
                         bool lean,
                         base::OnceClosure done)
    : browser_context_(browser_context),
      engine_file_(std::move(engine_file)),
      lean_(lean),
      done_(std::move(done)),
      urls_(ListUrls(lean)) {
  g_updater = this;
}

ListUpdater::~ListUpdater() {
  g_updater = nullptr;
}

void ListUpdater::Start() {
  Fetch(0);
}

void ListUpdater::Fetch(size_t index) {
  Download(urls_[index], base::BindOnce(&ListUpdater::OnFetched,
                                        weak_factory_.GetWeakPtr(), index));
}

void ListUpdater::Download(
    const std::string& url,
    base::OnceCallback<void(std::optional<std::string>)> done) {
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = GURL(url);
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  loader_ = network::SimpleURLLoader::Create(std::move(request),
                                             kTrafficAnnotation);
  loader_->SetRetryOptions(
      2, network::SimpleURLLoader::RETRY_ON_NETWORK_CHANGE |
             network::SimpleURLLoader::RETRY_ON_NAME_NOT_RESOLVED);
  loader_->DownloadToString(
      browser_context_->GetDefaultStoragePartition()
          ->GetURLLoaderFactoryForBrowserProcess()
          .get(),
      std::move(done), network::SimpleURLLoader::kMaxBoundedStringDownloadSize);
}

void ListUpdater::OnFetched(size_t index, std::optional<std::string> body) {
  if (!body || body->empty()) {
    LOG(ERROR) << "lrb: couldn't fetch " << urls_[index]
               << "; keeping the current engine file";
    std::move(done_).Run();
    return;
  }
  lists_.push_back(std::move(*body));
  if (index + 1 < urls_.size()) {
    Fetch(index + 1);
    return;
  }
  loader_.reset();
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&CompileAndWrite, std::move(lists_), lean_, engine_file_),
      base::BindOnce(&ListUpdater::OnWritten, weak_factory_.GetWeakPtr()));
}

void ListUpdater::OnWritten(bool ok) {
  if (!ok) {
    LOG(ERROR) << "lrb: couldn't write " << engine_file_;
    std::move(done_).Run();
    return;
  }
  VLOG(1) << "lrb: content-blocking engine updated: " << engine_file_;
  FetchResourceMap();
}

void ListUpdater::FetchResourceMap() {
  Download(std::string(kResourceBase) + "js/redirect-resources.js",
           base::BindOnce(&ListUpdater::OnResourceMap,
                          weak_factory_.GetWeakPtr()));
}

void ListUpdater::OnResourceMap(std::optional<std::string> body) {
  if (!body) {
    LOG(ERROR) << "lrb: couldn't fetch the surrogate list; keeping the old one";
    std::move(done_).Run();
    return;
  }
  resource_map_ = std::move(*body);
  for (const rust::String& name : adblock::redirect_resource_names(
           rust::Str(resource_map_.data(), resource_map_.size()))) {
    resource_names_.push_back(std::string(name));
  }
  if (resource_names_.empty()) {
    LOG(ERROR) << "lrb: no surrogates in uBlock Origin's map";
    std::move(done_).Run();
    return;
  }
  FetchResource(0);
}

void ListUpdater::FetchResource(size_t index) {
  Download(std::string(kResourceBase) + "web_accessible_resources/" +
               resource_names_[index],
           base::BindOnce(&ListUpdater::OnResource, weak_factory_.GetWeakPtr(),
                          index));
}

void ListUpdater::OnResource(size_t index, std::optional<std::string> body) {
  if (!body) {
    LOG(ERROR) << "lrb: couldn't fetch surrogate " << resource_names_[index]
               << "; keeping the old ones";
    std::move(done_).Run();
    return;
  }
  resources_.push_back(std::move(*body));
  if (index + 1 < resource_names_.size()) {
    FetchResource(index + 1);
    return;
  }
  loader_.reset();
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&AssembleSurrogates, std::move(resource_map_),
                     std::move(resource_names_), std::move(resources_),
                     ResourcesFile(engine_file_)),
      base::BindOnce(&ListUpdater::OnSurrogatesAssembled,
                     weak_factory_.GetWeakPtr()));
}

void ListUpdater::OnSurrogatesAssembled(std::optional<std::string> json) {
  if (!json) {
    LOG(ERROR) << "lrb: couldn't assemble the surrogates; keeping the old ones";
    std::move(done_).Run();
    return;
  }
  surrogates_json_ = std::move(*json);
  if (lean_) {
    OnScriptlets(std::string("[]"));  // the lean setting has no +js rules
    return;
  }
  FetchScriptlets();
}

void ListUpdater::FetchScriptlets() {
  scriptlet_page_ = content::WebContents::Create(
      content::WebContents::CreateParams(browser_context_));
  scriptlet_page_->GetController().LoadURL(
      GURL(ScriptletPage()), content::Referrer(),
      ui::PAGE_TRANSITION_AUTO_TOPLEVEL, std::string());
  scriptlet_timeout_.Start(
      FROM_HERE, base::Seconds(30),
      base::BindOnce(&ListUpdater::OnScriptlets, weak_factory_.GetWeakPtr(),
                     std::nullopt));
}

void ListUpdater::OnScriptlets(std::optional<std::string> json) {
  if (!done_) {
    return;  // already finished (a late POST, or the timeout)
  }
  scriptlet_timeout_.Stop();
  std::string resources = surrogates_json_;
  std::optional<base::ListValue> scriptlets;
  if (json) {
    scriptlets = base::JSONReader::ReadList(*json, base::JSON_PARSE_RFC);
  }
  if (scriptlets) {
    VLOG(1) << "lrb: " << scriptlets->size() << " scriptlets";
    resources = JoinJsonArrays(resources, *json);
  } else if (!lean_) {
    LOG(ERROR) << "lrb: no scriptlets this time (couldn't load uBlock "
                  "Origin's module); surrogates only";
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&WriteAtomically, ResourcesFile(engine_file_),
                     std::move(resources)),
      base::BindOnce(&ListUpdater::OnResourcesWritten,
                     weak_factory_.GetWeakPtr()));
}

void ListUpdater::OnResourcesWritten(bool ok) {
  scriptlet_page_.reset();
  if (ok) {
    VLOG(1) << "lrb: surrogates updated: " << ResourcesFile(engine_file_);
  } else {
    LOG(ERROR) << "lrb: couldn't write the surrogates";
  }
  std::move(done_).Run();
}

}  // namespace lrb
