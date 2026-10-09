// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_BROWSER_PRINT_H_
#define LRB_BROWSER_PRINT_H_

#include "printing/buildflags/buildflags.h"

namespace blink {
class AssociatedInterfaceRegistry;
}  // namespace blink

namespace content {
class RenderFrameHost;
class WebContents;
}  // namespace content

namespace lrb {

// Printing is to a PDF file, saved where the user chooses (the file picker,
// as downloads): the page is laid out for paper in this process (Skia's PDF
// backend, as headless Chrome's printToPDF) and the desktop's PDF viewer
// prints it. No print dialog or preview: Chrome's is a WebUI page (tens of
// MB) and needs CUPS. Started from the menu, Ctrl+P, or the page's
// window.print().
//
// Without printing in the build (enable_printing = false) these do nothing.

// Whether this build prints.
constexpr bool kCanPrint = BUILDFLAG(ENABLE_PRINTING);

// For every page (OnWebContentsCreated): what answers its renderer's
// printing requests.
void SetUpPrinting(content::WebContents* web_contents);
void RegisterPrintingInterface(content::RenderFrameHost& frame,
                               blink::AssociatedInterfaceRegistry& registry);

// Prints `web_contents`' page to a PDF and asks where to save it; the bar
// shows the file, as a download.
void PrintToPdf(content::WebContents* web_contents);

}  // namespace lrb

#endif  // LRB_BROWSER_PRINT_H_
