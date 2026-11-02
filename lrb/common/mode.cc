// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/common/mode.h"

#include "base/command_line.h"
#include "lrb/common/lrb_switches.h"

namespace lrb {

bool StandardMode() {
  static const bool standard =
      base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
          switches::kMode) == "standard";
  return standard;
}

}  // namespace lrb
