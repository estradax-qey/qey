#ifndef QEY_BENCH_PROCESS_H_
#define QEY_BENCH_PROCESS_H_

#include <cstdint>
#include <functional>
#include <optional>

namespace qey::bench {

// The memory this process is using right now, in bytes; 0 if it cannot be
// read. On macOS this is the physical footprint, which counts pages the system
// has compressed; on Linux it is the resident set size.
uint64_t CurrentFootprintBytes();

// The most memory this process has used so far, in bytes.
uint64_t PeakFootprintBytes();

struct MemoryUse {
  uint64_t before = 0;  // bytes in use when the work began
  uint64_t after = 0;   // bytes in use when it ended
  uint64_t peak = 0;    // the most bytes in use at any point
};

// Runs `work` in a child process and reports how much memory it took. A fresh
// process starts from a small, known baseline, and an allocator that keeps
// freed memory cannot carry it over to the next measurement.
//
// `work` is given two functions. It calls `begin` when its setup is done and
// the measured part starts, and `end` when that part is done *while the data
// it built is still alive*: memory that `work` has already freed by the time
// it returns is memory the measurement can no longer see. Returns
// std::nullopt if the child failed or `work` did not call both.
using MemoryWork = std::function<void(const std::function<void()>& begin,
                                      const std::function<void()>& end)>;
std::optional<MemoryUse> MeasureInChild(const MemoryWork& work);

}  // namespace qey::bench

#endif  // QEY_BENCH_PROCESS_H_
