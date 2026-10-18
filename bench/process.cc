#include "bench/process.h"

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <optional>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace qey::bench {

#if defined(__APPLE__)
namespace {

bool ReadVmInfo(task_vm_info_data_t* info) {
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  return task_info(mach_task_self(), TASK_VM_INFO,
                   reinterpret_cast<task_info_t>(info), &count) == KERN_SUCCESS;
}

}  // namespace
#endif

uint64_t CurrentFootprintBytes() {
#if defined(__APPLE__)
  // phys_footprint is what Activity Monitor shows as a process's memory. It
  // includes pages the system has compressed, which resident_size leaves out:
  // measuring with that would make a large heap look far smaller than it is.
  task_vm_info_data_t info;
  return ReadVmInfo(&info) ? info.phys_footprint : 0;
#else
  // The second field of /proc/self/statm is the resident size in pages.
  FILE* f = std::fopen("/proc/self/statm", "r");
  if (f == nullptr) return 0;
  unsigned long long size = 0, resident = 0;
  const int read = std::fscanf(f, "%llu %llu", &size, &resident);
  std::fclose(f);
  if (read != 2) return 0;
  return resident * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
#endif
}

uint64_t PeakFootprintBytes() {
#if defined(__APPLE__)
  task_vm_info_data_t info;
  if (ReadVmInfo(&info) && info.ledger_phys_footprint_peak > 0) {
    return info.ledger_phys_footprint_peak;
  }
  return CurrentFootprintBytes();
#else
  struct rusage usage;
  if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
  return static_cast<uint64_t>(usage.ru_maxrss) * 1024;  // kibibytes
#endif
}

std::optional<MemoryUse> MeasureInChild(const MemoryWork& work) {
  int fds[2];
  if (pipe(fds) != 0) return std::nullopt;
  std::fflush(nullptr);  // do not let the child repeat our buffered output

  const pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return std::nullopt;
  }
  if (pid == 0) {
    close(fds[0]);
    MemoryUse use;
    bool began = false;
    bool ended = false;
    work(
        [&] {
          use.before = CurrentFootprintBytes();
          began = true;
        },
        [&] {
          use.after = CurrentFootprintBytes();
          // The kernel's high-water mark is kept separately from the current
          // value and can trail it by a page, so take the larger of the two.
          use.peak = std::max(PeakFootprintBytes(), use.after);
          ended = true;
        });
    if (!began || !ended) _exit(2);
    const char* bytes = reinterpret_cast<const char*>(&use);
    size_t written = 0;
    while (written < sizeof use) {
      const ssize_t n = write(fds[1], bytes + written, sizeof use - written);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) _exit(3);
      written += static_cast<size_t>(n);
    }
    _exit(0);
  }

  close(fds[1]);
  MemoryUse use;
  char* bytes = reinterpret_cast<char*>(&use);
  size_t got = 0;
  while (got < sizeof use) {
    const ssize_t n = read(fds[0], bytes + got, sizeof use - got);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    got += static_cast<size_t>(n);
  }
  close(fds[0]);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  if (got != sizeof use || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    return std::nullopt;
  }
  return use;
}

}  // namespace qey::bench
