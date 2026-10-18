#include <sys/utsname.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <ostream>
#include <string>
#include <vector>

#include "bench/bench.h"

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace qey::bench {
namespace {

// ---- Formatting -------------------------------------------------------------

std::string Format(const char* pattern, double value) {
  char buffer[64];
  std::snprintf(buffer, sizeof buffer, pattern, value);
  return buffer;
}

// 1234567 -> "1,234,567"
std::string WithCommas(uint64_t value) {
  std::string digits = std::to_string(value);
  std::string out;
  for (size_t i = 0; i < digits.size(); ++i) {
    if (i > 0 && (digits.size() - i) % 3 == 0) out += ',';
    out += digits[i];
  }
  return out;
}

std::string Duration(double ns) {
  if (ns < 1e3) return Format("%.0f ns", ns);
  if (ns < 1e6) return Format("%.2f us", ns / 1e3);
  if (ns < 1e9) return Format("%.2f ms", ns / 1e6);
  return Format("%.2f s", ns / 1e9);
}

std::string Bytes(uint64_t bytes) {
  const double b = static_cast<double>(bytes);
  if (bytes < 1024) return std::to_string(bytes) + " B";
  if (bytes < (1u << 20)) return Format("%.1f KiB", b / 1024);
  if (bytes < (1u << 30)) return Format("%.1f MiB", b / (1u << 20));
  return Format("%.2f GiB", b / (1u << 30));
}

std::string Pad(const std::string& s, size_t width, bool right_align) {
  if (s.size() >= width) return s;
  const std::string fill(width - s.size(), ' ');
  return right_align ? fill + s : s + fill;
}

// ---- JSON -------------------------------------------------------------------

std::string Quote(const std::string& s) {
  std::string out = "\"";
  for (char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += ch;
    } else if (c < 0x20) {
      char buffer[8];
      std::snprintf(buffer, sizeof buffer, "\\u%04x", c);
      out += buffer;
    } else {
      out += ch;
    }
  }
  return out + "\"";
}

std::string Num(double value) {
  if (!std::isfinite(value)) return "null";
  char buffer[64];
  std::snprintf(buffer, sizeof buffer, "%.6g", value);
  return buffer;
}

uint64_t Used(const MemoryUse& use) {
  return use.after > use.before ? use.after - use.before : 0;
}

}  // namespace

Environment DescribeEnvironment() {
  Environment env;
#if defined(__APPLE__)
  char buffer[256];
  size_t length = sizeof buffer;
  if (sysctlbyname("machdep.cpu.brand_string", buffer, &length, nullptr, 0) ==
      0) {
    env.cpu = buffer;
  }
#else
  std::ifstream cpuinfo("/proc/cpuinfo");
  for (std::string line; std::getline(cpuinfo, line);) {
    if (line.rfind("model name", 0) == 0) {
      const size_t colon = line.find(':');
      if (colon != std::string::npos) env.cpu = line.substr(colon + 2);
      break;
    }
  }
#endif
  struct utsname names;
  if (uname(&names) == 0) {
    env.os =
        std::string(names.sysname) + " " + names.release + " " + names.machine;
  }
  env.compiler = __VERSION__;
#if defined(NDEBUG)
  env.optimized = true;
#endif
  return env;
}

void RenderText(const Report& report, std::ostream& out) {
  const Config& c = report.config;
  out << "qey benchmark\n";
  out << "  cpu:      " << report.environment.cpu << "\n";
  out << "  os:       " << report.environment.os << "\n";
  out << "  compiler: " << report.environment.compiler << "\n";
  out << "  workload: " << WithCommas(c.keys) << " keys, " << c.value_size
      << "-byte values, seed " << c.seed << "\n";
  if (!report.environment.optimized) {
    out << "\n  WARNING: this is not an optimized build, so the timings mean "
           "little.\n  Run it with:  bazel run -c opt //bench:qey_bench\n";
  }

  if (!report.speed.empty()) {
    out << "\nSPEED\n";
    out << Pad("scenario", 36, false) << Pad("ops/sec", 13, true)
        << Pad("mean", 11, true) << Pad("p50", 11, true) << Pad("p99", 11, true)
        << Pad("p99.9", 11, true) << Pad("max", 11, true) << "\n";
    for (const SpeedResult& r : report.speed) {
      const Timing& t = r.timing;
      // A rate means nothing for something done once.
      const std::string rate =
          t.ops <= 1 ? "-"
                     : WithCommas(static_cast<uint64_t>(t.ops_per_second));
      out << Pad(r.name, 36, false) << Pad(rate, 13, true)
          << Pad(Duration(t.mean_ns), 11, true);
      if (t.sampled > 0) {
        out << Pad(Duration(t.p50_ns), 11, true)
            << Pad(Duration(t.p99_ns), 11, true)
            << Pad(Duration(t.p999_ns), 11, true)
            << Pad(Duration(t.max_ns), 11, true);
      }
      out << "\n";
      if (!r.note.empty()) out << "    (" << r.note << ")\n";
    }
  }

  if (!report.memory.empty()) {
    out << "\nMEMORY (footprint of a fresh process, database in memory only)\n";
    out << Pad("scenario", 36, false) << Pad("elements", 12, true)
        << Pad("payload", 12, true) << Pad("memory", 12, true)
        << Pad("per element", 13, true) << Pad("x payload", 11, true) << "\n";
    for (const MemoryResult& r : report.memory) {
      out << Pad(r.name, 36, false) << Pad(WithCommas(r.elements), 12, true)
          << Pad(Bytes(r.payload_bytes), 12, true);
      if (!r.use.has_value()) {
        out << Pad("failed", 12, true) << "\n";
        continue;
      }
      const uint64_t used = Used(*r.use);
      out << Pad(Bytes(used), 12, true)
          << Pad(Format("%.1f B", static_cast<double>(used) /
                                      static_cast<double>(
                                          std::max<size_t>(r.elements, 1))),
                 13, true)
          << Pad(Format("%.2fx", static_cast<double>(used) /
                                     static_cast<double>(std::max<uint64_t>(
                                         r.payload_bytes, 1))),
                 11, true)
          << "\n";
    }
    out << "  payload = the keys and values themselves; \"x payload\" is the "
           "overhead of storing them.\n";
  }

  if (!report.sizes.empty()) {
    out << "\nFILE SIZE (the .qey log)\n";
    out << Pad("scenario", 38, false) << Pad("payload", 11, true)
        << Pad("after load", 20, true) << Pad("after rewrite", 20, true)
        << Pad("after compact", 20, true) << "\n";
    for (const SizeResult& r : report.sizes) {
      auto cell = [&](uint64_t bytes) {
        return Bytes(bytes) +
               Format(" (%.2fx)", static_cast<double>(bytes) /
                                      static_cast<double>(std::max<uint64_t>(
                                          r.payload_bytes, 1)));
      };
      out << Pad(r.name, 38, false) << Pad(Bytes(r.payload_bytes), 11, true)
          << Pad(cell(r.after_load), 20, true)
          << Pad(cell(r.after_rewrite), 20, true)
          << Pad(cell(r.after_compact), 20, true) << "\n";
    }
    out << "  \"load\" writes every element once, one call each; \"rewrite\" "
           "writes each one again;\n  compaction keeps one copy of each. "
           "Multiples are of the payload.\n";
  }
}

void RenderJson(const Report& report, std::ostream& out) {
  const Config& c = report.config;
  out << "{\"environment\":{\"cpu\":" << Quote(report.environment.cpu)
      << ",\"os\":" << Quote(report.environment.os)
      << ",\"compiler\":" << Quote(report.environment.compiler)
      << ",\"optimized\":" << (report.environment.optimized ? "true" : "false")
      << "},\"config\":{\"keys\":" << c.keys
      << ",\"value_size\":" << c.value_size << ",\"seed\":" << c.seed
      << ",\"always_ops\":" << c.always_ops << "}";

  out << ",\"speed\":[";
  for (size_t i = 0; i < report.speed.size(); ++i) {
    const SpeedResult& r = report.speed[i];
    const Timing& t = r.timing;
    if (i > 0) out << ",";
    out << "{\"name\":" << Quote(r.name) << ",\"note\":" << Quote(r.note)
        << ",\"ops\":" << t.ops << ",\"seconds\":" << Num(t.seconds)
        << ",\"ops_per_second\":" << Num(t.ops_per_second)
        << ",\"mean_ns\":" << Num(t.mean_ns) << ",\"p50_ns\":" << Num(t.p50_ns)
        << ",\"p99_ns\":" << Num(t.p99_ns) << ",\"p999_ns\":" << Num(t.p999_ns)
        << ",\"max_ns\":" << Num(t.max_ns) << ",\"sampled\":" << t.sampled
        << "}";
  }
  out << "]";

  out << ",\"memory\":[";
  for (size_t i = 0; i < report.memory.size(); ++i) {
    const MemoryResult& r = report.memory[i];
    if (i > 0) out << ",";
    out << "{\"name\":" << Quote(r.name) << ",\"elements\":" << r.elements
        << ",\"payload_bytes\":" << r.payload_bytes;
    if (r.use.has_value()) {
      const uint64_t used = Used(*r.use);
      out << ",\"footprint_before\":" << r.use->before
          << ",\"footprint_after\":" << r.use->after
          << ",\"footprint_peak\":" << r.use->peak << ",\"bytes_used\":" << used
          << ",\"bytes_per_element\":"
          << Num(static_cast<double>(used) /
                 static_cast<double>(std::max<size_t>(r.elements, 1)))
          << ",\"overhead_vs_payload\":"
          << Num(static_cast<double>(used) /
                 static_cast<double>(std::max<uint64_t>(r.payload_bytes, 1)));
    } else {
      out << ",\"failed\":true";
    }
    out << "}";
  }
  out << "]";

  out << ",\"file_size\":[";
  for (size_t i = 0; i < report.sizes.size(); ++i) {
    const SizeResult& r = report.sizes[i];
    const double payload =
        static_cast<double>(std::max<uint64_t>(r.payload_bytes, 1));
    if (i > 0) out << ",";
    out << "{\"name\":" << Quote(r.name) << ",\"elements\":" << r.elements
        << ",\"payload_bytes\":" << r.payload_bytes
        << ",\"after_load\":" << r.after_load
        << ",\"after_rewrite\":" << r.after_rewrite
        << ",\"after_compact\":" << r.after_compact << ",\"load_vs_payload\":"
        << Num(static_cast<double>(r.after_load) / payload)
        << ",\"compact_vs_payload\":"
        << Num(static_cast<double>(r.after_compact) / payload) << "}";
  }
  out << "]}\n";
}

int Main(const std::vector<std::string>& args, std::ostream& out,
         std::ostream& err) {
  Config config;
  bool json = false;

  auto number = [&](const std::string& name, const std::string& text,
                    uint64_t* value) {
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
    if (text.empty() || *end != '\0' || text[0] == '-') {
      err << "qey_bench: " << name << " needs a non-negative integer, got \""
          << text << "\"\n";
      return false;
    }
    *value = parsed;
    return true;
  };

  for (size_t i = 0; i < args.size(); ++i) {
    std::string name = args[i];
    std::optional<std::string> value;
    if (const size_t eq = name.find('=');
        eq != std::string::npos && name.rfind("--", 0) == 0) {
      value = name.substr(eq + 1);
      name = name.substr(0, eq);
    }
    auto take = [&]() -> std::optional<std::string> {
      if (value.has_value()) return value;
      if (i + 1 < args.size()) return args[++i];
      return std::nullopt;
    };
    uint64_t parsed = 0;
    if (name == "--help" || name == "-h") {
      out << "usage: qey_bench [--keys N] [--value-size BYTES] [--seed N]\n"
             "                 [--always-ops N] [--dir PATH]\n"
             "                 [--section speed|memory|size|all] [--json] "
             "[--quick]\n"
             "\n"
             "Measures speed (throughput and latency), memory (resident size "
             "per\n"
             "element) and the size of the .qey file. Build with -c opt:\n"
             "  bazel run -c opt //bench:qey_bench -- --keys 200000\n"
             "\n"
             "  --keys N         keys or collection elements per workload "
             "(200000)\n"
             "  --value-size B   bytes per value (64)\n"
             "  --always-ops N   writes to time with a flush on every write "
             "(1000)\n"
             "  --dir PATH       where the database files go (a temporary "
             "directory)\n"
             "  --section S      run only the speed, memory or size part\n"
             "  --json           print one JSON document instead of tables\n"
             "  --quick          a small workload, for checking that it runs\n";
      return 0;
    } else if (name == "--json") {
      json = true;
    } else if (name == "--quick") {
      config.keys = 1000;
      config.always_ops = 10;
    } else if (name == "--keys" || name == "--value-size" || name == "--seed" ||
               name == "--always-ops") {
      std::optional<std::string> text = take();
      if (!text.has_value() || !number(name, *text, &parsed)) {
        if (!text.has_value())
          err << "qey_bench: " << name << " needs a value\n";
        return 1;
      }
      if (name == "--keys")
        config.keys = parsed;
      else if (name == "--value-size")
        config.value_size = parsed;
      else if (name == "--seed")
        config.seed = parsed;
      else
        config.always_ops = parsed;
    } else if (name == "--dir" || name == "--section") {
      std::optional<std::string> text = take();
      if (!text.has_value()) {
        err << "qey_bench: " << name << " needs a value\n";
        return 1;
      }
      if (name == "--dir") {
        config.dir = *text;
      } else {
        if (*text != "speed" && *text != "memory" && *text != "size" &&
            *text != "all") {
          err << "qey_bench: --section must be speed, memory, size or all\n";
          return 1;
        }
        config.section = *text;
      }
    } else {
      err << "qey_bench: unknown option " << args[i] << "\n";
      return 1;
    }
  }
  if (config.keys < 100) {
    err << "qey_bench: --keys must be at least 100\n";
    return 1;
  }

  const Report report = RunSuite(config);
  if (json) {
    RenderJson(report, out);
  } else {
    RenderText(report, out);
  }
  return 0;
}

}  // namespace qey::bench
