#include "bench/bench.h"

#include <cctype>
#include <filesystem>
#include <set>
#include <sstream>
#include <string>

#include "gtest/gtest.h"

namespace qey::bench {
namespace {

Config Small() {
  Config c;
  c.keys = 600;
  c.value_size = 48;
  c.always_ops = 5;
  return c;
}

// Brackets and quotes balance, and nothing but JSON punctuation, strings and
// numbers appears outside strings. A parser would be better; this catches a
// stray comma-less value or unescaped quote.
bool LooksLikeJson(const std::string& text) {
  int depth = 0;
  bool in_string = false;
  for (size_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    if (in_string) {
      if (ch == '\\') {
        ++i;
      } else if (ch == '"') {
        in_string = false;
      } else if (static_cast<unsigned char>(ch) < 0x20) {
        return false;
      }
      continue;
    }
    switch (ch) {
      case '"':
        in_string = true;
        break;
      case '{':
      case '[':
        ++depth;
        break;
      case '}':
      case ']':
        if (--depth < 0) return false;
        break;
      case ',':
      case ':':
      case '.':
      case '-':
      case '+':
      case '\n':
        break;
      default:
        if (!std::isalnum(static_cast<unsigned char>(ch))) return false;
    }
  }
  return depth == 0 && !in_string;
}

TEST(SuiteTest, SpeedResultsAreSane) {
  Config c = Small();
  c.section = "speed";
  Report r = RunSuite(c);
  EXPECT_TRUE(r.memory.empty());
  EXPECT_TRUE(r.sizes.empty());
  ASSERT_GE(r.speed.size(), 20u);

  std::set<std::string> names;
  for (const SpeedResult& s : r.speed) {
    EXPECT_TRUE(names.insert(s.name).second) << "duplicate " << s.name;
    EXPECT_GT(s.timing.ops, 0u) << s.name;
    EXPECT_GT(s.timing.seconds, 0) << s.name;
    EXPECT_GT(s.timing.mean_ns, 0) << s.name;
    if (s.timing.sampled > 0) {
      EXPECT_LE(s.timing.p50_ns, s.timing.p99_ns) << s.name;
      EXPECT_LE(s.timing.p99_ns, s.timing.p999_ns) << s.name;
      EXPECT_LE(s.timing.p999_ns, s.timing.max_ns) << s.name;
    }
  }
  for (const char* expected :
       {"string set (new key)", "string get (hit)", "write batch of 10 sets",
        "hash hset (new field)", "list rpush", "set sadd", "zset zadd",
        "stream xadd", "stream xrevrange (newest 20)",
        "stream xrange (page of 100)", "stream xadd (keep newest 1000)",
        "stream xadd in a write batch", "persistent xadd (sync: manual)",
        "persistent set (sync: always)", "open and replay the log",
        "compact the log"}) {
    EXPECT_TRUE(names.count(expected)) << expected;
  }
}

TEST(SuiteTest, TheThreeSyncPoliciesAreMeasuredSeparately) {
  Config c = Small();
  c.section = "speed";
  Report r = RunSuite(c);
  size_t found = 0;
  for (const SpeedResult& s : r.speed) {
    if (s.name.rfind("persistent set (sync: ", 0) != 0) continue;
    ++found;
    EXPECT_GT(s.timing.mean_ns, 0) << s.name;
    EXPECT_FALSE(s.note.empty()) << s.name;
  }
  // How much slower flushing every write is depends on the disk, and on a
  // RAM-backed directory it costs almost nothing, so only the presence of the
  // three rows is checked here.
  EXPECT_EQ(found, 3u);
}

TEST(SuiteTest, FileSizesFollowFromHowTheLogWorks) {
  Config c = Small();
  c.section = "size";
  Report r = RunSuite(c);
  ASSERT_EQ(r.sizes.size(), 6u);
  for (const SizeResult& s : r.sizes) {
    EXPECT_GT(s.payload_bytes, 0u) << s.name;
    // Every record carries framing, so the log is bigger than what it holds.
    EXPECT_GT(s.after_load, s.payload_bytes) << s.name;
    // Writing everything again appends; nothing is overwritten in place.
    EXPECT_GT(s.after_rewrite, s.after_load) << s.name;
    // Compaction keeps one copy, written without per-call framing.
    EXPECT_LT(s.after_compact, s.after_rewrite) << s.name;
    EXPECT_LE(s.after_compact, s.after_load + 64) << s.name;
    EXPECT_GT(s.after_compact, s.payload_bytes) << s.name;
  }
}

TEST(SuiteTest, MemoryIsMeasuredPerScenarioAndCannotBeLessThanThePayload) {
  Config c = Small();
  c.keys = 6000;  // enough that page rounding does not matter
  c.section = "memory";
  Report r = RunSuite(c);
  ASSERT_EQ(r.memory.size(), 8u);
  for (const MemoryResult& m : r.memory) {
    EXPECT_GT(m.elements, 0u) << m.name;
    EXPECT_GT(m.payload_bytes, 0u) << m.name;
    ASSERT_TRUE(m.use.has_value()) << m.name << ": the child process failed";
    EXPECT_GE(m.use->after, m.use->before) << m.name;
    EXPECT_GE(m.use->peak, m.use->before) << m.name;
    // The data is in memory, so memory holds at least that much of it. A
    // smaller number means the measurement missed it, for example by being
    // taken after the database was already freed.
    EXPECT_GE(m.use->after - m.use->before, m.payload_bytes) << m.name;
  }
}

TEST(SuiteTest, TheScratchDirectoryIsRemoved) {
  const std::string dir = testing::TempDir() + "/bench_scratch";
  std::filesystem::remove_all(dir);
  Config c = Small();
  c.section = "size";
  c.dir = dir;
  RunSuite(c);
  // An explicit directory is the caller's, so it stays, but the files go.
  EXPECT_TRUE(std::filesystem::exists(dir));
  EXPECT_TRUE(std::filesystem::is_empty(dir));
  std::filesystem::remove_all(dir);
}

TEST(OutputTest, JsonHasEverySectionAndIsWellFormed) {
  std::ostringstream out, err;
  ASSERT_EQ(Main({"--keys", "100", "--value-size", "40", "--always-ops", "3",
                  "--json"},
                 out, err),
            0)
      << err.str();
  const std::string json = out.str();
  EXPECT_TRUE(LooksLikeJson(json)) << json.substr(0, 400);
  for (const char* key :
       {"\"environment\"", "\"config\"", "\"speed\"", "\"memory\"",
        "\"file_size\"", "\"ops_per_second\"", "\"p99_ns\"",
        "\"bytes_per_element\"", "\"overhead_vs_payload\"", "\"after_compact\"",
        "\"keys\":100", "\"value_size\":40"}) {
    EXPECT_NE(json.find(key), std::string::npos) << key;
  }
  EXPECT_EQ(json.back(), '\n');
}

TEST(OutputTest, TextHasATablePerSection) {
  std::ostringstream out, err;
  ASSERT_EQ(Main({"--keys", "100", "--always-ops", "2"}, out, err), 0)
      << err.str();
  const std::string text = out.str();
  for (const char* heading : {"SPEED", "MEMORY", "FILE SIZE", "qey benchmark",
                              "ops/sec", "per element", "after compact"}) {
    EXPECT_NE(text.find(heading), std::string::npos) << heading;
  }
}

TEST(OutputTest, QuickMeansSmall) {
  std::ostringstream out, err;
  ASSERT_EQ(Main({"--quick", "--section", "size", "--json"}, out, err), 0);
  EXPECT_NE(out.str().find("\"keys\":1000"), std::string::npos);
}

TEST(OutputTest, SectionsCanBeChosen) {
  std::ostringstream out, err;
  ASSERT_EQ(Main({"--quick", "--section", "size", "--json"}, out, err), 0);
  EXPECT_NE(out.str().find("\"file_size\":[{"), std::string::npos);
  EXPECT_NE(out.str().find("\"speed\":[]"), std::string::npos);
  EXPECT_NE(out.str().find("\"memory\":[]"), std::string::npos);
}

TEST(OutputTest, BadArgumentsAreRefusedWithAMessage) {
  for (std::vector<std::string> bad : {
           std::vector<std::string>{"--keys"},
           {"--keys", "many"},
           {"--keys", "-5"},
           {"--keys", "50"},  // too few to mean anything
           {"--section", "everything"},
           {"--frobnicate"},
       }) {
    std::ostringstream out, err;
    EXPECT_EQ(Main(bad, out, err), 1) << bad[0];
    EXPECT_NE(err.str().find("qey_bench:"), std::string::npos) << bad[0];
    EXPECT_EQ(out.str(), "");
  }
}

TEST(OutputTest, HelpDescribesTheOptions) {
  std::ostringstream out, err;
  ASSERT_EQ(Main({"--help"}, out, err), 0);
  for (const char* option : {"--keys", "--value-size", "--section", "--json",
                             "--quick", "--always-ops", "--dir"}) {
    EXPECT_NE(out.str().find(option), std::string::npos) << option;
  }
}

TEST(EnvironmentTest, IsDescribed) {
  Environment e = DescribeEnvironment();
  EXPECT_FALSE(e.os.empty());
  EXPECT_FALSE(e.compiler.empty());
}

}  // namespace
}  // namespace qey::bench
