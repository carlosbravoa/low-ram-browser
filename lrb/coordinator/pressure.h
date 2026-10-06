// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_COORDINATOR_PRESSURE_H_
#define LRB_COORDINATOR_PRESSURE_H_

#include <cstdint>
#include <string>
#include <vector>

// The coordinator's view of memory: the same budgets patches/0001 uses in
// each instance (every cgroup v2 limit from ours up to the root, and the
// system), read from kernel files. All instances are the coordinator's
// children and share these budgets.

namespace lrb::coordinator {

struct Budget {
  int64_t limit = 0;      // bytes
  int64_t available = 0;  // bytes left, counting inactive page cache as free
};

// Reads the budgets of the calling process. Roots are parameters for tests.
std::vector<Budget> ReadBudgets(const std::string& sysfs_cgroup_root = "/sys/fs/cgroup",
                                const std::string& procfs_root = "/proc");

// Whether any budget has less than `percent` of its limit left.
bool BelowPercent(const std::vector<Budget>& budgets, int percent);

}  // namespace lrb::coordinator

#endif  // LRB_COORDINATOR_PRESSURE_H_
