// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/coordinator/pressure.h"

#include <algorithm>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>

namespace lrb::coordinator {

namespace {

std::optional<std::string> ReadFile(const std::string& path) {
  std::ifstream file(path);
  if (!file) {
    return std::nullopt;
  }
  std::stringstream contents;
  contents << file.rdbuf();
  return contents.str();
}

std::optional<int64_t> ParseInt(std::string_view text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) {
    text.remove_suffix(1);
  }
  if (text.empty()) {
    return std::nullopt;
  }
  int64_t value = 0;
  for (char c : text) {
    if (c < '0' || c > '9') {
      return std::nullopt;  // e.g. "max"
    }
    value = value * 10 + (c - '0');
  }
  return value;
}

std::optional<int64_t> ReadInt(const std::string& path) {
  std::optional<std::string> contents = ReadFile(path);
  return contents ? ParseInt(*contents) : std::nullopt;
}

// `key value` (memory.stat) or `Key:   value kB` (/proc/meminfo).
std::optional<int64_t> Keyed(const std::string& contents,
                             const std::string& key,
                             int64_t unit) {
  std::istringstream lines(contents);
  std::string line;
  while (std::getline(lines, line)) {
    std::istringstream fields(line);
    std::string name;
    std::string value;
    fields >> name >> value;
    if (!name.empty() && name.back() == ':') {
      name.pop_back();
    }
    if (name == key) {
      std::optional<int64_t> parsed = ParseInt(value);
      return parsed ? std::optional<int64_t>(*parsed * unit) : std::nullopt;
    }
  }
  return std::nullopt;
}

}  // namespace

std::vector<Budget> ReadBudgets(const std::string& sysfs_cgroup_root,
                                const std::string& procfs_root) {
  std::vector<Budget> budgets;

  std::string cgroup;
  if (std::optional<std::string> self = ReadFile(procfs_root + "/self/cgroup")) {
    std::istringstream lines(*self);
    std::string line;
    while (std::getline(lines, line)) {
      if (line.starts_with("0::")) {
        cgroup = line.substr(3);
      }
    }
  }
  // Every cgroup from ours up to the root may carry a limit.
  std::string dir = sysfs_cgroup_root + (cgroup == "/" ? "" : cgroup);
  while (!cgroup.empty()) {
    std::optional<int64_t> limit = ReadInt(dir + "/memory.max");
    if (std::optional<int64_t> high = ReadInt(dir + "/memory.high");
        high && (!limit || *high < *limit)) {
      limit = high;
    }
    std::optional<int64_t> current = ReadInt(dir + "/memory.current");
    std::optional<std::string> stat = ReadFile(dir + "/memory.stat");
    if (limit && current && stat && *limit > 0) {
      const int64_t inactive_file = Keyed(*stat, "inactive_file", 1).value_or(0);
      const int64_t used = std::max<int64_t>(*current - inactive_file, 0);
      budgets.push_back({*limit, std::max<int64_t>(*limit - used, 0)});
    }
    if (dir == sysfs_cgroup_root) {
      break;
    }
    dir = dir.substr(0, dir.rfind('/'));
  }

  if (std::optional<std::string> meminfo = ReadFile(procfs_root + "/meminfo")) {
    std::optional<int64_t> total = Keyed(*meminfo, "MemTotal", 1024);
    std::optional<int64_t> available = Keyed(*meminfo, "MemAvailable", 1024);
    if (total && available && *total > 0) {
      budgets.push_back({*total, *available});
    }
  }
  return budgets;
}

bool BelowPercent(const std::vector<Budget>& budgets, int percent) {
  for (const Budget& budget : budgets) {
    if (budget.available * 100 < budget.limit * percent) {
      return true;
    }
  }
  return false;
}

}  // namespace lrb::coordinator
