#ifndef QEY_BENCH_BENCH_H_
#define QEY_BENCH_BENCH_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "bench/measure.h"
#include "bench/process.h"

namespace qey::bench {

struct Config {
  // How many keys (or collection elements) the workloads use.
  size_t keys = 200000;
  // The size of each value, in bytes.
  size_t value_size = 64;
  uint64_t seed = 1;
  // Writes with SyncPolicy::kAlways flush to disk each time, so far fewer of
  // them are timed.
  size_t always_ops = 1000;
  // Where the database files go. Empty means a fresh directory under the
  // system's temporary one, removed afterwards.
  std::string dir;
  // Which part to run: "speed", "memory", "size", or "all".
  std::string section = "all";
};

struct SpeedResult {
  std::string name;
  std::string note;  // anything needed to read the numbers
  Timing timing;
};

struct MemoryResult {
  std::string name;
  size_t elements = 0;
  uint64_t payload_bytes = 0;    // the keys and values themselves
  std::optional<MemoryUse> use;  // empty if the child process failed
};

struct SizeResult {
  std::string name;
  size_t elements = 0;
  uint64_t payload_bytes = 0;
  uint64_t after_load = 0;     // the file after writing every element once
  uint64_t after_rewrite = 0;  // and after rewriting each one
  uint64_t after_compact = 0;  // and after compacting
};

struct Environment {
  std::string cpu;
  std::string os;
  std::string compiler;
  bool optimized = false;
};

struct Report {
  Config config;
  Environment environment;
  std::vector<SpeedResult> speed;
  std::vector<MemoryResult> memory;
  std::vector<SizeResult> sizes;
};

Environment DescribeEnvironment();

// Runs the selected sections.
Report RunSuite(const Config& config);

void RenderText(const Report& report, std::ostream& out);
void RenderJson(const Report& report, std::ostream& out);

// The command line: --keys N --value-size BYTES --seed N --always-ops N
// --dir PATH --section speed|memory|size|all --json --quick --help.
// Returns an exit code.
int Main(const std::vector<std::string>& args, std::ostream& out,
         std::ostream& err);

}  // namespace qey::bench

#endif  // QEY_BENCH_BENCH_H_
