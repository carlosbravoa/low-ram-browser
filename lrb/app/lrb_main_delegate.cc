// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/app/lrb_main_delegate.h"

#include <utility>

#include "base/base_paths.h"
#include "base/base_switches.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/logging/logging_settings.h"
#include "base/path_service.h"
#include "base/process/current_process.h"
#include "components/memory_system/initializer.h"
#include "components/memory_system/parameters.h"
#include "content/public/app/initialize_mojo_core.h"
#include "content/public/common/content_switches.h"
#include "base/feature_list.h"
#include "base/metrics/field_trial.h"
#include "base/no_destructor.h"
#include "components/variations/field_trial_config/field_trial_util.h"
#include "components/variations/service/buildflags.h"
#include "components/variations/variations_switches.h"
#include "content/public/common/content_switch_dependent_feature_overrides.h"
#include "content/public/common/main_function_params.h"
#include "lrb/browser/dark_pages.h"
#include "lrb/browser/lrb_browser_context.h"
#include "lrb/browser/lrb_content_browser_client.h"
#include "lrb/common/content_blocker.h"
#include "lrb/common/lrb_content_client.h"
#include "lrb/common/lrb_switches.h"
#include "lrb/common/settings.h"
#include "lrb/renderer/lrb_content_renderer_client.h"
#include "media/base/media_switches.h"
#include "sandbox/policy/switches.h"
#include "third_party/blink/public/common/switches.h"
#include "ui/base/resource/resource_bundle.h"

namespace lrb {

namespace {

// Logs to stderr. --log-file=<path> logs to that file instead. Unlike
// content_shell, nothing is written next to the binary by default.
void InitLogging(const base::CommandLine& command_line) {
  logging::LoggingSettings settings;
  const base::FilePath log_file =
      command_line.GetSwitchValuePath(::switches::kLogFile);
  if (log_file.empty()) {
    settings.logging_dest = logging::LOG_TO_STDERR;
  } else {
    settings.logging_dest = logging::LOG_TO_FILE;
    settings.log_file_path = log_file.value();
    settings.delete_old = logging::DELETE_OLD_LOG_FILE;
  }
  logging::InitLogging(settings);
  logging::SetLogItems(/*enable_process_id=*/true, /*enable_thread_id=*/true,
                       /*enable_timestamp=*/true, /*enable_tickcount=*/false);
}

// Appends `item` to a comma-separated list switch, unless the command line
// already names it there or in `opposite_switch` (the command line decides).
// Chromium reads only the last occurrence of these switches, so merge.
void AddToListSwitch(base::CommandLine& command_line,
                     const char* list_switch,
                     const char* opposite_switch,
                     const std::string& item) {
  std::string list = command_line.GetSwitchValueASCII(list_switch);
  const std::string opposite = command_line.GetSwitchValueASCII(opposite_switch);
  if (list.find(item) != std::string::npos ||
      opposite.find(item) != std::string::npos) {
    return;
  }
  list += (list.empty() ? "" : ",") + item;
  command_line.AppendSwitchASCII(list_switch, list);
}

void AddSwitchUnlessSet(base::CommandLine& command_line, const char* name) {
  if (!command_line.HasSwitch(name)) {
    command_line.AppendSwitch(name);
  }
}

// lrb's defaults where they differ from content_shell's: the configuration
// measured leanest (results/phase1-*, instance-base-*), so a plain
// `lrb <url>` gets it. Each only when the command line doesn't say
// otherwise.
void ApplyDefaults(base::CommandLine& command_line) {
  // One process per window (decided 2026-10-03): the renderer, network
  // and GPU work in the browser process. --single-process alone still
  // starts two zygotes (~45 MB). Chromium's sandbox needs separate
  // renderers; in their place: the V8 sandbox, and Landlock + seccomp
  // around the whole instance (docs/design.md, "Open questions"). --lrb-multi-process: Chromium's
  // usual processes, for debugging.
  if (!command_line.HasSwitch(switches::kMultiProcess)) {
    AddSwitchUnlessSet(command_line, ::switches::kSingleProcess);
    AddSwitchUnlessSet(command_line, ::switches::kNoZygote);
    AddSwitchUnlessSet(command_line, sandbox::policy::switches::kNoSandbox);
    AddSwitchUnlessSet(command_line, ::switches::kInProcessGPU);
    AddToListSwitch(command_line, ::switches::kEnableFeatures,
                    ::switches::kDisableFeatures, "NetworkServiceInProcess2");
    AddToListSwitch(command_line, ::switches::kDisableFeatures,
                    ::switches::kEnableFeatures, "AudioServiceOutOfProcess");
  }
  // Software compositing: ~45 MB less than the GPU path in Phase 0.
  // Whether 2 GB laptops and the Pi want their GPUs is still open
  // (docs/design.md, "Open questions"); --lrb-gpu keeps it for that comparison.
  // The user's choice in the settings (lrb/common/settings.h); unset:
  // software, until they choose.
  if (!command_line.HasSwitch(switches::kGpu) &&
      !Settings::Read().gpu.value_or(false)) {
    AddSwitchUnlessSet(command_line, ::switches::kDisableGpu);
  }
  AddSwitchUnlessSet(command_line, ::switches::kEnableLowEndDeviceMode);
  // Each window keeps one page alive; a back-forward cache would keep a
  // second (discarding manages background windows instead).
  AddToListSwitch(command_line, ::switches::kDisableFeatures,
                  ::switches::kEnableFeatures, "BackForwardCache");
  // V8 favours size over speed.
  std::string js_flags = command_line.GetSwitchValueASCII(blink::switches::kJavaScriptFlags);
  if (js_flags.find("optimize-for-size") == std::string::npos) {
    command_line.AppendSwitchASCII(
        blink::switches::kJavaScriptFlags,
        js_flags.empty() ? "--optimize-for-size"
                         : js_flags + " --optimize-for-size");
  }

  // Media waits for the user: no autoplay, muted or not, before the page
  // gets user activation (a click on the page or the link that opened it).
  // Muted autoplay loops decode at full resolution: CNN's front page plays
  // four (180-270 MB). MutedAutoplayRequiresUserActivation is
  // patches/0002; without it muted video would still be exempt.
  if (!command_line.HasSwitch(::switches::kAutoplayPolicy)) {
    command_line.AppendSwitchASCII(::switches::kAutoplayPolicy,
                                   "document-user-activation-required");
  }
  AddToListSwitch(command_line, ::switches::kEnableBlinkFeatures,
                  ::switches::kDisableBlinkFeatures,
                  "MutedAutoplayRequiresUserActivation");
}

// Chromium's features: Stable's defaults, the command line's
// (--enable-features...) and the switches that imply features. Not the field
// trial testing config, which non-Chrome-branded builds (and content_shell)
// apply by default: the testing group of every experiment Chrome is trying,
// features Stable doesn't ship. Without it lrb measured 1.4 MB less on a
// blank page, 5 on Wikipedia, 17 on GitHub, 22 on CNN
// (MEASUREMENTS.md). --enable-field-trial-config
// applies it, for comparisons.
void CreateFeatureList() {
  static base::NoDestructor<base::FieldTrialList> field_trial_list;
  const base::CommandLine& command_line =
      *base::CommandLine::ForCurrentProcess();
  auto feature_list = std::make_unique<base::FeatureList>();
  feature_list->InitFromCommandLine(
      command_line.GetSwitchValueASCII(::switches::kEnableFeatures),
      command_line.GetSwitchValueASCII(::switches::kDisableFeatures));
  feature_list->RegisterExtraFeatureOverrides(
      content::GetSwitchDependentFeatureOverrides(command_line));
#if BUILDFLAG(FIELDTRIAL_TESTING_ENABLED)
  if (command_line.HasSwitch(
          variations::switches::kEnableFieldTrialTestingConfig)) {
    variations::AssociateDefaultFieldTrialConfig(
        variations::Study::PLATFORM_LINUX, variations::Study::DESKTOP,
        feature_list.get());
  }
#endif
  base::FeatureList::SetInstance(std::move(feature_list));
}

void InitializeResourceBundle() {
  base::FilePath pak_file;
  bool found = base::PathService::Get(base::DIR_ASSETS, &pak_file);
  DCHECK(found);
  ui::ResourceBundle::InitSharedInstanceWithPakPath(
      pak_file.Append(FILE_PATH_LITERAL("lrb.pak")));
}

}  // namespace

LrbMainDelegate::LrbMainDelegate() = default;
LrbMainDelegate::~LrbMainDelegate() = default;

std::optional<int> LrbMainDelegate::BasicStartupComplete() {
  base::CommandLine& command_line = *base::CommandLine::ForCurrentProcess();
  InitLogging(command_line);
  ApplyDefaults(command_line);
  if (!command_line.HasSwitch(::switches::kProcessType)) {
    LrbBrowserContext::SetUpProfileDir(command_line);
  }
  if (!command_line.HasSwitch(::switches::kProcessType)) {
    LoadDarkPages(
        command_line.GetSwitchValuePath(lrb::switches::kUserDataDir));
  }
  // Before any request, and while blocking file access is still allowed. In
  // single-process mode (lrb's design) the renderer is this process.
  if (command_line.HasSwitch(lrb::switches::kAdblockFile)) {
    ContentBlocker::Load(command_line.GetSwitchValuePath(lrb::switches::kAdblockFile));
    // The user turned blocking off for this site (its profile says so).
    const base::FilePath profile =
        command_line.GetSwitchValuePath(lrb::switches::kUserDataDir);
    if (!profile.empty() &&
        base::PathExists(profile.Append(lrb::kBlockingOffFile))) {
      ContentBlocker::SetEnabled(false);
    }
  }
  return std::nullopt;
}

bool LrbMainDelegate::ShouldCreateFeatureList(InvokedIn invoked_in) {
  return std::holds_alternative<InvokedInChildProcess>(invoked_in);
}

bool LrbMainDelegate::ShouldInitializeMojo(InvokedIn invoked_in) {
  return ShouldCreateFeatureList(invoked_in);
}

void LrbMainDelegate::PreSandboxStartup() {
  InitializeResourceBundle();
}

std::variant<int, content::MainFunctionParams> LrbMainDelegate::RunProcess(
    const std::string& process_type,
    content::MainFunctionParams main_function_params) {
  if (process_type.empty()) {
    base::CurrentProcess::GetInstance().SetProcessType(
        base::CurrentProcessType::PROCESS_BROWSER);
  }
  // Have the caller run BrowserMain() or the child's main loop.
  return std::move(main_function_params);
}

std::optional<int> LrbMainDelegate::PostEarlyInitialization(
    InvokedIn invoked_in) {
  if (!ShouldCreateFeatureList(invoked_in)) {
    CreateFeatureList();
  }
  if (!ShouldInitializeMojo(invoked_in)) {
    content::InitializeMojoCore();
  }
  // Same allocator dispatch setup as content_shell, so memory numbers stay
  // comparable with the cs-*/lrb-* measurements.
  memory_system::Initializer()
      .SetDispatcherParameters(memory_system::DispatcherParameters::
                                   PoissonAllocationSamplerInclusion::kEnforce,
                               memory_system::DispatcherParameters::
                                   AllocationTraceRecorderInclusion::kIgnore,
                               base::CommandLine::ForCurrentProcess()
                                   ->GetSwitchValueASCII(::switches::kProcessType))
      .Initialize(memory_system_);
  return std::nullopt;
}

content::ContentClient* LrbMainDelegate::CreateContentClient() {
  content_client_ = std::make_unique<LrbContentClient>();
  return content_client_.get();
}

content::ContentBrowserClient* LrbMainDelegate::CreateContentBrowserClient() {
  browser_client_ = std::make_unique<LrbContentBrowserClient>();
  return browser_client_.get();
}

content::ContentRendererClient* LrbMainDelegate::CreateContentRendererClient() {
  renderer_client_ = std::make_unique<LrbContentRendererClient>();
  return renderer_client_.get();
}

}  // namespace lrb
