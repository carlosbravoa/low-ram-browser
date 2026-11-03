// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_COMMON_LRB_DESCRIPTORS_H_
#define LRB_COMMON_LRB_DESCRIPTORS_H_

#include "content/public/common/content_descriptors.h"

// Keys of the descriptors the browser passes to its child processes
// (base::GlobalDescriptors), after //content's own.
enum {
  // The content blocking engine file, read-only, for renderers: those forked
  // by a zygote started before the lists were built have no engine.
  kLrbEngineDescriptor = kContentIPCDescriptorMax,
  // Its resources file (<engine>.resources.json, ListUpdater writes it): the
  // scriptlets the renderer injects come from it.
  kLrbResourcesDescriptor,
};

#endif  // LRB_COMMON_LRB_DESCRIPTORS_H_
