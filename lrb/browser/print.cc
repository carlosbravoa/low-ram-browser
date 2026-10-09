// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/print.h"

#if BUILDFLAG(ENABLE_PRINTING)

#include <stdlib.h>

#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/ref_counted_memory.h"
#include "base/nix/xdg_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "components/printing/browser/print_manager.h"
#include "components/printing/browser/print_to_pdf/pdf_print_job.h"
#include "components/printing/browser/print_to_pdf/pdf_print_utils.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_user_data.h"
#include "lrb/browser/shell.h"
#include "lrb/browser/ui/file_picker.h"
#include "lrb/browser/ui/lrb_platform_delegate.h"
#include "lrb/browser/ui/window_view.h"
#include "net/base/filename_util.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_registry.h"

#endif  // BUILDFLAG(ENABLE_PRINTING)

namespace lrb {

#if BUILDFLAG(ENABLE_PRINTING)

namespace {

using DownloadState = WindowView::DownloadStatus::State;

// Letter where it's the paper sold (the Americas' letter countries, the
// Philippines), A4 elsewhere; by the locale, as the desktop's printing does.
bool UsesLetter() {
  for (const char* name : {"LC_ALL", "LC_PAPER", "LANG"}) {
    const char* value = getenv(name);
    if (!value || !*value) {
      continue;
    }
    const std::string locale(value);
    for (const char* country :
         {"_US", "_CA", "_MX", "_PH", "_CL", "_CO", "_VE", "_PR", "_CR",
          "_GT", "_PA", "_SV", "_NI", "_HN", "_DO"}) {
      if (locale.find(country) != std::string::npos) {
        return true;
      }
    }
    return false;
  }
  return false;
}

// In the Downloads folder, named after the page: the picker's suggestion.
// Blocking.
base::FilePath SuggestedPath(const GURL& url, const std::u16string& title) {
  base::FilePath folder =
      base::nix::GetXDGUserDirectory("DOWNLOAD", "Downloads");
  if (folder == base::GetHomeDir()) {
    folder = folder.Append("Downloads");  // no user-dirs.dirs
  }
  base::CreateDirectory(folder);
  base::FilePath name = net::GenerateFileName(
      url, std::string(), std::string(), base::UTF16ToUTF8(title),
      "application/pdf", "page");
  return folder.Append(name.ReplaceExtension(FILE_PATH_LITERAL("pdf")));
}

void Report(base::WeakPtr<WindowView> view,
            WindowView::DownloadStatus status) {
  if (view) {
    view->ShowDownload(status);
  }
}

// The PDF is written: where the user chose, or, confined, staged in the
// profile for the coordinator to move (file_picker.h).
void Written(base::WeakPtr<WindowView> view,
             WindowView::DownloadStatus status,
             bool ok) {
  if (!ok) {
    status.state = DownloadState::kFailed;
    Report(view, std::move(status));
    return;
  }
  const base::FilePath staged = status.path;
  std::optional<BrokeredSave> brokered = GetBrokeredSave(staged);
  if (!brokered) {
    status.state = DownloadState::kDone;
    Report(view, std::move(status));
    return;
  }
  status.path = brokered->target;
  status.broker_id = brokered->id;
  status.saved_without_asking = brokered->automatic;
  FinishBrokeredSave(staged, base::BindOnce(
                                 [](base::WeakPtr<WindowView> view,
                                    WindowView::DownloadStatus status,
                                    bool moved) {
                                   status.state = moved
                                                      ? DownloadState::kDone
                                                      : DownloadState::kFailed;
                                   Report(view, std::move(status));
                                 },
                                 view, status));
}

void Write(base::WeakPtr<WindowView> view,
           scoped_refptr<base::RefCountedMemory> pdf,
           bool saved_without_asking,
           base::FilePath path) {
  WindowView::DownloadStatus status;
  status.name = path.BaseName().LossyDisplayName();
  status.path = path;
  status.saved_without_asking = saved_without_asking;
  status.state = DownloadState::kInProgress;
  Report(view, status);
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::BLOCK_SHUTDOWN},
      base::BindOnce(
          [](scoped_refptr<base::RefCountedMemory> pdf, base::FilePath path) {
            return base::WriteFile(path, base::span<const uint8_t>(*pdf));
          },
          std::move(pdf), path),
      base::BindOnce(&Written, view, std::move(status)));
}

void AskWhereToSave(base::WeakPtr<WindowView> view,
                    gfx::NativeWindow window,
                    scoped_refptr<base::RefCountedMemory> pdf,
                    base::FilePath suggested) {
  PickFiles(
      ui::SelectFileDialog::SELECT_SAVEAS_FILE, u"Save as PDF", suggested,
      window,
      base::BindOnce(
          [](base::WeakPtr<WindowView> view,
             scoped_refptr<base::RefCountedMemory> pdf,
             base::FilePath suggested, PickResult result,
             std::vector<base::FilePath> paths) {
            switch (result) {
              case PickResult::kChosen:
                Write(view, std::move(pdf), /*saved_without_asking=*/false,
                      paths.front());
                return;
              case PickResult::kCanceled:
                return;
              case PickResult::kUnavailable:
                // Nothing to ask with: Downloads, under a free name.
                base::ThreadPool::PostTaskAndReplyWithResult(
                    FROM_HERE,
                    {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
                    base::BindOnce(&base::GetUniquePath, suggested),
                    base::BindOnce(&Write, view, std::move(pdf),
                                   /*saved_without_asking=*/true));
                return;
            }
          },
          view, std::move(pdf), suggested));
}

// Answers the renderer's printing requests for one page. A page's own
// window.print() is turned down (it would wait for the settings a print
// dialog gives) and printed to a PDF instead, as from the menu.
class LrbPrintManager
    : public printing::PrintManager,
      public content::WebContentsUserData<LrbPrintManager> {
 public:
  LrbPrintManager(const LrbPrintManager&) = delete;
  LrbPrintManager& operator=(const LrbPrintManager&) = delete;
  ~LrbPrintManager() override = default;

  static void Bind(
      mojo::PendingAssociatedReceiver<printing::mojom::PrintManagerHost>
          receiver,
      content::RenderFrameHost* frame) {
    auto* contents = content::WebContents::FromRenderFrameHost(frame);
    if (auto* manager = contents ? FromWebContents(contents) : nullptr) {
      manager->BindReceiver(std::move(receiver), frame);
    }
  }

  void Print() {
    content::RenderFrameHost* frame = web_contents()->GetPrimaryMainFrame();
    if (printing_ || !frame || !frame->IsRenderFrameLive()) {
      return;
    }
    const GURL url = web_contents()->GetLastCommittedURL();
    const bool letter = UsesLetter();
    // Inches. The page's own @page size wins (prefer_css_page_size).
    auto params = print_to_pdf::GetPrintPagesParams(
        url, /*landscape=*/std::nullopt, /*display_header_footer=*/false,
        /*print_background=*/false, /*scale=*/std::nullopt,
        /*paper_width=*/letter ? 8.5 : 8.27,
        /*paper_height=*/letter ? 11.0 : 11.69,
        /*margin_top=*/0.4, /*margin_bottom=*/0.4, /*margin_left=*/0.4,
        /*margin_right=*/0.4, /*header_template=*/std::nullopt,
        /*footer_template=*/std::nullopt, /*prefer_css_page_size=*/true,
        /*generate_tagged_pdf=*/false, /*generate_document_outline=*/false);
    if (std::holds_alternative<std::string>(params)) {
      LOG(ERROR) << "lrb: print settings: " << std::get<std::string>(params);
      return;
    }
    printing_ = true;
    PrintToPdf(frame, std::string(),
               std::move(std::get<printing::mojom::PrintPagesParamsPtr>(params)),
               base::BindOnce(&LrbPrintManager::OnPrinted,
                              weak_factory_.GetWeakPtr(), url,
                              web_contents()->GetTitle()));
  }

 private:
  friend class content::WebContentsUserData<LrbPrintManager>;

  explicit LrbPrintManager(content::WebContents* contents)
      : printing::PrintManager(contents),
        content::WebContentsUserData<LrbPrintManager>(*contents) {}

  void PrintToPdf(content::RenderFrameHost* frame,
                  const std::string& page_ranges,
                  printing::mojom::PrintPagesParamsPtr params,
                  print_to_pdf::PdfPrintJob::PrintToPdfCallback callback) {
    print_to_pdf::PdfPrintJob::StartJob(web_contents(), frame,
                                        GetPrintRenderFrame(frame), page_ranges,
                                        std::move(params), std::move(callback));
  }

  void OnPrinted(GURL url,
                 std::u16string title,
                 print_to_pdf::PdfPrintResult result,
                 scoped_refptr<base::RefCountedMemory> pdf) {
    printing_ = false;
    Shell* shell = Shell::FromWebContents(web_contents());
    WindowView* view = shell ? LrbPlatformDelegate::ViewFor(shell) : nullptr;
    if (result != print_to_pdf::PdfPrintResult::kPrintSuccess || !pdf) {
      LOG(ERROR) << "lrb: printing failed: "
                 << print_to_pdf::PdfPrintResultToString(result);
      if (view) {
        WindowView::DownloadStatus status;
        status.name = u"PDF";
        status.state = DownloadState::kFailed;
        view->ShowDownload(status);
      }
      return;
    }
    if (!view) {
      return;  // the window closed meanwhile
    }
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&SuggestedPath, url, title),
        base::BindOnce(&AskWhereToSave, view->GetWeakPtr(), shell->window(),
                       std::move(pdf)));
  }

  // printing::mojom::PrintManagerHost. Only the page's own printing asks
  // for settings (the menu's gives them): it gets none, so it ends, and the
  // page is printed the menu's way once the renderer is done with that.
  void GetDefaultPrintSettings(
      GetDefaultPrintSettingsCallback callback) override {
    std::move(callback).Run(nullptr);
    PrintSoon();
  }
  void ScriptedPrint(printing::mojom::ScriptedPrintParamsPtr params,
                     ScriptedPrintCallback callback) override {
    std::move(callback).Run(nullptr);
    PrintSoon();
  }

  void PrintSoon() {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&LrbPrintManager::Print,
                                  weak_factory_.GetWeakPtr()));
  }

  bool printing_ = false;
  base::WeakPtrFactory<LrbPrintManager> weak_factory_{this};

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(LrbPrintManager);

}  // namespace

void SetUpPrinting(content::WebContents* web_contents) {
  LrbPrintManager::CreateForWebContents(web_contents);
}

void RegisterPrintingInterface(content::RenderFrameHost& frame,
                               blink::AssociatedInterfaceRegistry& registry) {
  registry.AddInterface<printing::mojom::PrintManagerHost>(base::BindRepeating(
      [](content::RenderFrameHost* frame,
         mojo::PendingAssociatedReceiver<printing::mojom::PrintManagerHost>
             receiver) { LrbPrintManager::Bind(std::move(receiver), frame); },
      &frame));
}

void PrintToPdf(content::WebContents* web_contents) {
  if (auto* manager = LrbPrintManager::FromWebContents(web_contents)) {
    manager->Print();
  }
}

#else  // !BUILDFLAG(ENABLE_PRINTING)

void SetUpPrinting(content::WebContents* web_contents) {}
void RegisterPrintingInterface(content::RenderFrameHost& frame,
                               blink::AssociatedInterfaceRegistry& registry) {}
void PrintToPdf(content::WebContents* web_contents) {}

#endif  // BUILDFLAG(ENABLE_PRINTING)

}  // namespace lrb
