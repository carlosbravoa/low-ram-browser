// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LRB_COORDINATOR_CONFINE_H_
#define LRB_COORDINATOR_CONFINE_H_

#include <string>
#include <vector>

namespace lrb::coordinator {

// What a confined process may reach in the file system. Landlock is an
// allow-list: anything not under one of these paths is out of reach
// (missing paths are skipped).
struct ConfinePolicy {
  std::vector<std::string> read;        // read files and list directories
  std::vector<std::string> read_exec;   // ... and execute (binaries, libraries)
  std::vector<std::string> read_write;  // everything, devices' ioctls included
};

// Confines the calling process and everything it starts, for good (call it
// in a child between fork() and exec()):
// - no_new_privs: setuid binaries can't give privileges back;
// - Landlock: the file system as `policy` says; no ptrace of, signals to or
//   abstract-socket connections with processes outside the confined domain
//   (where the kernel supports them, ABI 6+);
// - seccomp: system calls a browser never needs fail with EPERM (ptrace,
//   mount, kexec, kernel modules, bpf, perf events, keyrings, userfaultfd,
//   io_uring, ...).
// Returns false with `error` set if a step failed: the caller must not
// exec then. A kernel without Landlock (`landlock_abi()` 0) still gets
// no_new_privs and seccomp; `error` says so and the result is true.
bool Confine(const ConfinePolicy& policy, std::string* error);

// The kernel's Landlock ABI version, 0 if Landlock is unavailable.
int LandlockAbi();

}  // namespace lrb::coordinator

#endif  // LRB_COORDINATOR_CONFINE_H_
