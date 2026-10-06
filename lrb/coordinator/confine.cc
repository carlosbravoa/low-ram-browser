// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/coordinator/confine.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <string>
#include <vector>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace lrb::coordinator {

namespace {

// Landlock's interface (linux/landlock.h), defined here: Chromium's sysroot
// predates it. The system call numbers are the same on every architecture.
constexpr long kCreateRuleset = 444;
constexpr long kAddRule = 445;
constexpr long kRestrictSelf = 446;
constexpr uint32_t kCreateRulesetVersion = 1;
constexpr int kRulePathBeneath = 1;

constexpr uint64_t kExecute = 1ull << 0;
constexpr uint64_t kWriteFile = 1ull << 1;
constexpr uint64_t kReadFile = 1ull << 2;
constexpr uint64_t kReadDir = 1ull << 3;
constexpr uint64_t kAbi1All = (1ull << 13) - 1;  // EXECUTE ... MAKE_SYM
constexpr uint64_t kRefer = 1ull << 13;           // ABI 2
constexpr uint64_t kTruncate = 1ull << 14;        // ABI 3
constexpr uint64_t kIoctlDev = 1ull << 15;        // ABI 5
// Rights that apply to a file (the rest only to directories).
constexpr uint64_t kFileRights =
    kExecute | kWriteFile | kReadFile | kTruncate | kIoctlDev;

constexpr uint64_t kScopeAbstractUnixSocket = 1ull << 0;  // ABI 6
constexpr uint64_t kScopeSignal = 1ull << 1;              // ABI 6

struct RulesetAttr {
  uint64_t handled_access_fs;
  uint64_t handled_access_net;
  uint64_t scoped;
};

struct __attribute__((packed)) PathBeneathAttr {
  uint64_t allowed_access;
  int32_t parent_fd;
};

uint64_t HandledFs(int abi) {
  uint64_t rights = kAbi1All;
  if (abi >= 2) {
    rights |= kRefer;
  }
  if (abi >= 3) {
    rights |= kTruncate;
  }
  if (abi >= 5) {
    rights |= kIoctlDev;
  }
  return rights;
}

bool AddPath(int ruleset, const std::string& path, uint64_t access) {
  const int fd = open(path.c_str(), O_PATH | O_CLOEXEC);
  if (fd < 0) {
    return true;  // not on this system: nothing to allow
  }
  struct stat st = {};
  if (fstat(fd, &st) == 0 && !S_ISDIR(st.st_mode)) {
    access &= kFileRights;
  }
  PathBeneathAttr attr = {access, fd};
  const bool ok = syscall(kAddRule, ruleset, kRulePathBeneath, &attr, 0) == 0;
  close(fd);
  return ok;
}

bool Landlock(const ConfinePolicy& policy, int abi, std::string* error) {
  const uint64_t all = HandledFs(abi);
  RulesetAttr attr = {all, 0, 0};
  size_t size = sizeof(attr.handled_access_fs);
  if (abi >= 6) {
    attr.scoped = kScopeAbstractUnixSocket | kScopeSignal;
    size = sizeof(attr);
  }
  const int ruleset = static_cast<int>(syscall(kCreateRuleset, &attr, size, 0));
  if (ruleset < 0) {
    *error = std::string("landlock_create_ruleset: ") + strerror(errno);
    return false;
  }
  const uint64_t read = kReadFile | kReadDir;
  bool ok = true;
  for (const std::string& path : policy.read) {
    ok &= AddPath(ruleset, path, read);
  }
  for (const std::string& path : policy.read_exec) {
    ok &= AddPath(ruleset, path, read | kExecute);
  }
  for (const std::string& path : policy.read_write) {
    ok &= AddPath(ruleset, path, all);
  }
  if (!ok) {
    *error = std::string("landlock_add_rule: ") + strerror(errno);
    close(ruleset);
    return false;
  }
  const bool restricted = syscall(kRestrictSelf, ruleset, 0) == 0;
  if (!restricted) {
    *error = std::string("landlock_restrict_self: ") + strerror(errno);
  }
  close(ruleset);
  return restricted;
}

// System calls a browser never makes: they fail with EPERM (not a kill, so
// code that probes for a feature, e.g. userfaultfd or io_uring, carries on).
constexpr int kDenied[] = {
    __NR_ptrace,
    __NR_process_vm_readv,
    __NR_process_vm_writev,
    __NR_kexec_load,
#ifdef __NR_kexec_file_load
    __NR_kexec_file_load,
#endif
    __NR_init_module,
    __NR_finit_module,
    __NR_delete_module,
    __NR_mount,
    __NR_umount2,
    __NR_pivot_root,
    __NR_chroot,
    __NR_swapon,
    __NR_swapoff,
    __NR_reboot,
    __NR_bpf,
    __NR_perf_event_open,
    __NR_keyctl,
    __NR_add_key,
    __NR_request_key,
    __NR_userfaultfd,
#ifdef __NR_io_uring_setup
    __NR_io_uring_setup,
    __NR_io_uring_enter,
    __NR_io_uring_register,
#endif
    __NR_open_by_handle_at,
    __NR_name_to_handle_at,
#ifdef __NR_iopl
    __NR_iopl,
    __NR_ioperm,
#endif
    __NR_acct,
    __NR_settimeofday,
    __NR_clock_settime,
    __NR_clock_adjtime,
    __NR_adjtimex,
    __NR_quotactl,
    __NR_syslog,
    __NR_vhangup,
    __NR_setns,
    __NR_unshare,
#ifdef __NR_open_tree
    __NR_open_tree,
    __NR_move_mount,
    __NR_fsopen,
    __NR_fsconfig,
    __NR_fsmount,
    __NR_fspick,
#endif
#ifdef __NR_pidfd_getfd
    __NR_pidfd_getfd,
#endif
};

#if defined(__x86_64__)
constexpr uint32_t kAuditArch = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
constexpr uint32_t kAuditArch = AUDIT_ARCH_AARCH64;
#else
#error "lrb confines x86-64 and arm64 only"
#endif

bool Seccomp(std::string* error) {
  constexpr uint32_t kErrno = SECCOMP_RET_ERRNO | EPERM;
  std::vector<sock_filter> filter = {
      // Another architecture's system call numbers: refuse outright.
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kAuditArch, 1, 0),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
  };
#if defined(__x86_64__)
  // x32 system calls (another ABI on the same architecture): refused.
  filter.push_back(BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000, 0, 1));
  filter.push_back(BPF_STMT(BPF_RET | BPF_K, kErrno));
#endif
  for (int nr : kDenied) {
    filter.push_back(
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<uint32_t>(nr), 0, 1));
    filter.push_back(BPF_STMT(BPF_RET | BPF_K, kErrno));
  }
  filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
  sock_fprog program = {static_cast<unsigned short>(filter.size()),
                        filter.data()};
  if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0) {
    *error = std::string("seccomp: ") + strerror(errno);
    return false;
  }
  return true;
}

}  // namespace

int LandlockAbi() {
  const long abi = syscall(kCreateRuleset, nullptr, 0, kCreateRulesetVersion);
  return abi < 0 ? 0 : static_cast<int>(abi);
}

bool Confine(const ConfinePolicy& policy, std::string* error) {
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
    *error = std::string("no_new_privs: ") + strerror(errno);
    return false;
  }
  const int abi = LandlockAbi();
  if (abi > 0) {
    if (!Landlock(policy, abi, error)) {
      return false;
    }
  } else {
    *error = "no Landlock in this kernel: file access not confined";
  }
  return Seccomp(error);
}

}  // namespace lrb::coordinator
