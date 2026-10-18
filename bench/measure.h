#ifndef QEY_BENCH_MEASURE_H_
#define QEY_BENCH_MEASURE_H_

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace qey::bench {

// Keeps a value alive so that the compiler cannot delete the work that made it.
inline volatile uint64_t g_sink = 0;
template <typename T>
inline void Consume(const T& value) {
  g_sink = g_sink + static_cast<uint64_t>(sizeof(value));
}

// The value at percentile `p` (0 to 100) of `sorted`, by nearest rank. The
// input must be sorted and not empty.
inline double Percentile(const std::vector<double>& sorted, double p) {
  const double rank = p / 100.0 * static_cast<double>(sorted.size());
  size_t index = rank <= 1 ? 0 : static_cast<size_t>(std::ceil(rank)) - 1;
  return sorted[std::min(index, sorted.size() - 1)];
}

struct Timing {
  size_t ops = 0;
  double seconds = 0;
  double ops_per_second = 0;
  double mean_ns = 0;
  // Of the operations that were timed one by one (see Measure()).
  double p50_ns = 0;
  double p99_ns = 0;
  double p999_ns = 0;
  double max_ns = 0;
  size_t sampled = 0;
};

// Runs `op(i)` for i in [0, ops) and times it. The whole run is timed once,
// which gives the throughput without any per-call overhead. To get a latency
// distribution, every `sample_every`-th call is also timed on its own; use 1
// for slow operations, where reading the clock costs nothing by comparison,
// and more for fast ones, where it would distort the result.
template <typename Op>
Timing Measure(size_t ops, size_t sample_every, Op&& op) {
  using Clock = std::chrono::steady_clock;
  sample_every = std::max<size_t>(sample_every, 1);
  std::vector<double> samples;
  samples.reserve(ops / sample_every + 1);

  const auto start = Clock::now();
  for (size_t i = 0; i < ops; ++i) {
    if (i % sample_every == 0) {
      const auto t0 = Clock::now();
      op(i);
      const auto t1 = Clock::now();
      samples.push_back(
          std::chrono::duration<double, std::nano>(t1 - t0).count());
    } else {
      op(i);
    }
  }
  const auto stop = Clock::now();

  Timing t;
  t.ops = ops;
  t.seconds = std::chrono::duration<double>(stop - start).count();
  t.ops_per_second = t.seconds > 0 ? static_cast<double>(ops) / t.seconds : 0;
  t.mean_ns = ops > 0 ? t.seconds * 1e9 / static_cast<double>(ops) : 0;
  if (!samples.empty()) {
    std::sort(samples.begin(), samples.end());
    t.p50_ns = Percentile(samples, 50);
    t.p99_ns = Percentile(samples, 99);
    t.p999_ns = Percentile(samples, 99.9);
    t.max_ns = samples.back();
    t.sampled = samples.size();
  }
  return t;
}

}  // namespace qey::bench

#endif  // QEY_BENCH_MEASURE_H_
