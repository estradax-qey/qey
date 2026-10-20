#include "cli/cli.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <set>
#include <sstream>
#include <streambuf>
#include <string>
#include <vector>

#include "cli/commands.h"
#include "gtest/gtest.h"
#include "qey/db.h"

namespace qey::cli {
namespace {

struct Run_ {
  int code;
  std::string out;
  std::string err;
};

// A stream that fails the test if anything tries to read it: a command that is
// not given --stdin must never wait for input.
class ForbiddenInput : public std::streambuf {
 protected:
  int_type underflow() override {
    ADD_FAILURE() << "the command tried to read standard input";
    return traits_type::eof();
  }
};

class CliTest : public testing::Test {
 protected:
  void SetUp() override {
    dir_ = testing::TempDir() + "/cli_test_" +
           testing::UnitTest::GetInstance()->current_test_info()->name();
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    db_ = dir_ + "/db.qey";
    unsetenv("QEY_DB");
  }
  void TearDown() override {
    unsetenv("QEY_DB");
    std::filesystem::remove_all(dir_);
  }

  // Runs `qey --db <db> args...`.
  Run_ Q(std::vector<std::string> args, const std::string& input = "") {
    args.insert(args.begin(), {"--db", db_});
    return Raw(args, input);
  }

  // Runs `qey --db <db> --json args...`.
  Run_ J(std::vector<std::string> args, const std::string& input = "") {
    args.insert(args.begin(), {"--db", db_, "--json"});
    return Raw(args, input);
  }

  Run_ Raw(const std::vector<std::string>& args,
           const std::string& input = "") {
    ForbiddenInput forbidden;
    std::istringstream given(input);
    std::istream no_input(&forbidden);
    std::ostringstream out, err;
    const int code =
        qey::cli::Run(args, input.empty() ? no_input : given, out, err);
    return {code, out.str(), err.str()};
  }

  std::string dir_;
  std::string db_;
};

TEST_F(CliTest, SetThenGet) {
  Run_ set = Q({"set", "greeting", "hello"});
  EXPECT_EQ(set.code, 0);
  EXPECT_EQ(set.out, "OK\n");
  EXPECT_EQ(set.err, "");

  Run_ get = Q({"get", "greeting"});
  EXPECT_EQ(get.code, 0);
  EXPECT_EQ(get.out, "hello\n");
}

TEST_F(CliTest, AMissingKeyIsNilWithExitCodeZero) {
  ASSERT_EQ(Q({"set", "a", "1"}).code, 0);
  Run_ text = Q({"get", "nope"});
  EXPECT_EQ(text.code, 0);
  EXPECT_EQ(text.out, "(nil)\n");
  Run_ json = J({"get", "nope"});
  EXPECT_EQ(json.code, 0);
  EXPECT_EQ(json.out, "{\"ok\":true,\"result\":null}\n");
}

TEST_F(CliTest, JsonSuccessEnvelope) {
  ASSERT_EQ(Q({"set", "k", "v"}).code, 0);
  EXPECT_EQ(J({"get", "k"}).out, "{\"ok\":true,\"result\":\"v\"}\n");
  EXPECT_EQ(J({"exists", "k"}).out, "{\"ok\":true,\"result\":true}\n");
  EXPECT_EQ(J({"incr", "n"}).out, "{\"ok\":true,\"result\":1}\n");
  EXPECT_EQ(J({"keys"}).out, "{\"ok\":true,\"result\":[\"k\",\"n\"]}\n");
}

TEST_F(CliTest, ErrorsGoToStderrInTextModeAndStdoutInJsonMode) {
  ASSERT_EQ(Q({"set", "s", "string"}).code, 0);

  Run_ text = Q({"lpush", "s", "x"});
  EXPECT_EQ(text.code, 2);
  EXPECT_EQ(text.out, "");
  EXPECT_NE(text.err.find("WRONG_TYPE"), std::string::npos) << text.err;

  Run_ json = J({"lpush", "s", "x"});
  EXPECT_EQ(json.code, 2);
  EXPECT_EQ(json.err, "");
  EXPECT_EQ(json.out,
            "{\"ok\":false,\"error\":{\"code\":\"WRONG_TYPE\",\"message\":"
            "\"the key holds a different type of value\"}}\n");
}

TEST_F(CliTest, UsageErrorsExitWithOneAndSayWhatIsExpected) {
  Run_ text = Q({"incr"});
  EXPECT_EQ(text.code, 1);
  EXPECT_NE(text.err.find("wrong number of arguments"), std::string::npos);
  EXPECT_NE(text.err.find("usage: qey incr <key> [delta]"), std::string::npos);

  Run_ json = J({"incr"});
  EXPECT_EQ(json.code, 1);
  EXPECT_NE(json.out.find("\"ok\":false"), std::string::npos);
  EXPECT_NE(json.out.find("\"code\":\"USAGE\""), std::string::npos);
  EXPECT_NE(json.out.find("\"usage\":\"incr <key> [delta]\""),
            std::string::npos);
}

TEST_F(CliTest, TooManyOrTooFewArgumentsAreRejectedForEveryCommand) {
  for (const CommandSpec& spec : Commands()) {
    if (spec.raw_args) continue;
    if (spec.min_args > 0) {
      EXPECT_EQ(Q({spec.name}).code, 1) << spec.name << " with no arguments";
    }
    if (spec.max_args != SIZE_MAX) {
      std::vector<std::string> many = {spec.name};
      for (size_t i = 0; i <= spec.max_args; ++i) many.push_back("x");
      EXPECT_EQ(Q(many).code, 1) << spec.name << " with too many";
    }
  }
}

TEST_F(CliTest, NumbersAreCheckedBeforeAnythingChanges) {
  ASSERT_EQ(Q({"rpush", "l", "a", "b"}).code, 0);
  for (std::vector<std::string> bad : {
           std::vector<std::string>{"incr", "n", "1.5"},
           {"incr", "n", "abc"},
           {"incr", "n", ""},
           {"lrange", "l", "x", "1"},
           {"lindex", "l", "1.0"},
           {"lpop", "l", "-1"},
           {"zadd", "z", "nan", "m"},
           {"zadd", "z", "1x", "m"},
           {"expire", "l", "soon"},
           {"expire", "l", "10x"},
           {"expire", "l", "-5"},
           {"set", "k", "v", "--ttl", "forever"},
       }) {
    Run_ r = Q(bad);
    EXPECT_EQ(r.code, 1) << bad[0] << " " << bad.back();
    EXPECT_EQ(r.out, "");
  }
  EXPECT_EQ(Q({"exists", "n"}).out, "false\n");
  EXPECT_EQ(Q({"exists", "z"}).out, "false\n");
  EXPECT_EQ(Q({"exists", "k"}).out, "false\n");
  EXPECT_EQ(Q({"llen", "l"}).out, "2\n");
}

TEST_F(CliTest, NegativeNumbersAreArgumentsNotOptions) {
  ASSERT_EQ(Q({"rpush", "l", "a", "b", "c"}).code, 0);
  EXPECT_EQ(Q({"lrange", "l", "0", "-1"}).out, "a\nb\nc\n");
  EXPECT_EQ(Q({"lindex", "l", "-1"}).out, "c\n");
  EXPECT_EQ(Q({"incr", "n", "-5"}).out, "-5\n");
}

TEST_F(CliTest, DoubleDashEndsOptionsSoAValueMayStartWithDashes) {
  EXPECT_EQ(Q({"set", "k", "--weird"}).code, 1);  // looks like an option
  ASSERT_EQ(Q({"set", "--", "k", "--weird"}).code, 0);
  EXPECT_EQ(Q({"get", "k"}).out, "--weird\n");
  ASSERT_EQ(Q({"set", "--ttl", "10s", "--", "k2", "--also"}).code, 0);
  EXPECT_EQ(Q({"get", "k2"}).out, "--also\n");
}

TEST_F(CliTest, UnknownAndMalformedOptionsAreUsageErrors) {
  EXPECT_EQ(Q({"get", "k", "--bogus"}).code, 1);
  EXPECT_EQ(Q({"set", "k", "v", "--ttl"}).code, 1);        // no value
  EXPECT_EQ(Q({"set", "k", "v", "--stdin=yes"}).code, 1);  // takes no value
  EXPECT_EQ(Raw({"--bogus", "get", "k"}).code, 1);
  EXPECT_EQ(Raw({"--db"}).code, 1);
  EXPECT_EQ(Raw({"--sync", "sometimes", "get", "k"}).code, 1);
}

TEST_F(CliTest, OptionsMayUseEqualsOrASeparateValue) {
  ASSERT_EQ(Q({"set", "a", "1", "--ttl=30s"}).code, 0);
  ASSERT_EQ(Q({"set", "b", "1", "--ttl", "30s"}).code, 0);
  ASSERT_EQ(Raw({"--db=" + db_, "--sync=manual", "set", "c", "1"}).code, 0);
  EXPECT_GT(std::stoll(Q({"ttl", "a"}).out), 0);
  EXPECT_GT(std::stoll(Q({"ttl", "b"}).out), 0);
  EXPECT_EQ(Q({"get", "c"}).out, "1\n");
}

TEST_F(CliTest, DurationsAcceptUnits) {
  struct Case {
    const char* text;
    int64_t at_most_ms;
    int64_t more_than_ms;
  };
  for (Case c : {Case{"500ms", 500, 0}, Case{"30s", 30000, 29000},
                 Case{"30", 30000, 29000},  // a bare number is seconds
                 Case{"2m", 120000, 119000}, Case{"1h", 3600000, 3599000},
                 Case{"1d", 86400000, 86399000}}) {
    ASSERT_EQ(Q({"set", "k", "v", "--ttl", c.text}).code, 0) << c.text;
    const int64_t ttl = std::stoll(Q({"ttl", "k"}).out);
    EXPECT_LE(ttl, c.at_most_ms) << c.text;
    EXPECT_GT(ttl, c.more_than_ms) << c.text;
  }
}

TEST_F(CliTest, TtlReportsMinusOneForNoExpiryAndMinusTwoForMissing) {
  ASSERT_EQ(Q({"set", "k", "v"}).code, 0);
  EXPECT_EQ(Q({"ttl", "k"}).out, "-1\n");
  EXPECT_EQ(Q({"ttl", "nope"}).out, "-2\n");
  EXPECT_EQ(Q({"expire", "k", "1h"}).out, "true\n");
  EXPECT_GT(std::stoll(Q({"ttl", "k"}).out), 0);
  EXPECT_EQ(Q({"persist", "k"}).out, "true\n");
  EXPECT_EQ(Q({"ttl", "k"}).out, "-1\n");
  EXPECT_EQ(Q({"expire", "nope", "1h"}).out, "false\n");
}

TEST_F(CliTest, AnOverlongTtlIsRefusedByTheDatabase) {
  Run_ r = Q({"set", "k", "v", "--ttl", "99999999999d"});
  EXPECT_EQ(r.code, 2);
  EXPECT_NE(r.err.find("INVALID_ARGUMENT"), std::string::npos) << r.err;
  EXPECT_EQ(Q({"exists", "k"}).out, "false\n");
}

TEST_F(CliTest, ValuesCanComeFromStandardInputIncludingBinaryData) {
  const std::string binary("line1\nline2\0\xff\xfe", 15);
  ASSERT_EQ(Q({"set", "blob", "--stdin"}, binary).code, 0);
  Run_ text = Q({"get", "blob"});
  EXPECT_EQ(text.out, binary + "\n");  // raw bytes, then the newline
  Run_ json = J({"get", "blob"});
  EXPECT_NE(json.out.find("{\"base64\":"), std::string::npos) << json.out;

  EXPECT_EQ(Q({"set", "blob", "v", "--stdin"}, "x").code, 1);
}

TEST_F(CliTest, JsonEscapesTextAndPreservesBinary) {
  ASSERT_EQ(Q({"set", "k", "say \"hi\"\n\ttab \\ done"}).code, 0);
  EXPECT_EQ(J({"get", "k"}).out,
            "{\"ok\":true,\"result\":\"say \\\"hi\\\"\\n\\ttab \\\\ done\"}\n");
  ASSERT_EQ(Q({"set", "u", "caf\xc3\xa9 \xe2\x82\xac"}).code, 0);
  EXPECT_EQ(J({"get", "u"}).out,
            "{\"ok\":true,\"result\":\"caf\xc3\xa9 \xe2\x82\xac\"}\n");
}

TEST_F(CliTest, GetOnAKeyOfAnotherTypeIsAnErrorNotNil) {
  ASSERT_EQ(Q({"rpush", "l", "a"}).code, 0);
  Run_ r = J({"get", "l"});
  EXPECT_EQ(r.code, 2);
  EXPECT_NE(r.out.find("WRONG_TYPE"), std::string::npos);
  EXPECT_NE(r.out.find("list"), std::string::npos);
  EXPECT_EQ(Q({"type", "l"}).out, "list\n");
  EXPECT_EQ(Q({"type", "nope"}).out, "none\n");
}

TEST_F(CliTest, KeyCommands) {
  EXPECT_EQ(Q({"set", "a", "1"}).code, 0);
  EXPECT_EQ(Q({"append", "a", "23"}).out, "3\n");
  EXPECT_EQ(Q({"get", "a"}).out, "123\n");
  EXPECT_EQ(Q({"incr", "a"}).out, "124\n");
  EXPECT_EQ(Q({"decr", "a", "4"}).out, "120\n");
  EXPECT_EQ(Q({"rename", "a", "b"}).out, "true\n");
  EXPECT_EQ(Q({"rename", "a", "b"}).out, "false\n");
  EXPECT_EQ(Q({"exists", "b"}).out, "true\n");
  ASSERT_EQ(Q({"set", "c", "x"}).code, 0);
  EXPECT_EQ(Q({"del", "b", "c", "nope"}).out, "2\n");
  EXPECT_EQ(Q({"keys"}).out, "");
  Run_ bad = Q({"incr", "text"});
  EXPECT_EQ(bad.out, "1\n");
  ASSERT_EQ(Q({"set", "text", "abc"}).code, 0);
  Run_ not_int = J({"incr", "text"});
  EXPECT_EQ(not_int.code, 2);
  EXPECT_NE(not_int.out.find("NOT_INTEGER"), std::string::npos);
}

TEST_F(CliTest, KeysScanAndMget) {
  for (const char* k : {"user:1", "user:2", "user:3", "other"}) {
    ASSERT_EQ(Q({"set", k, std::string("v-") + k}).code, 0);
  }
  EXPECT_EQ(Q({"keys", "user:"}).out, "user:1\nuser:2\nuser:3\n");
  EXPECT_EQ(Q({"keys"}).out, "other\nuser:1\nuser:2\nuser:3\n");

  EXPECT_EQ(J({"scan", "user:", "2"}).out,
            "{\"ok\":true,\"result\":{\"keys\":[\"user:1\",\"user:2\"],"
            "\"next\":\"user:2\"}}\n");
  EXPECT_EQ(J({"scan", "user:", "2", "--after", "user:2"}).out,
            "{\"ok\":true,\"result\":{\"keys\":[\"user:3\"],\"next\":null}}\n");
  EXPECT_EQ(J({"mget", "user:1", "nope", "other"}).out,
            "{\"ok\":true,\"result\":[\"v-user:1\",null,\"v-other\"]}\n");
}

TEST_F(CliTest, Hashes) {
  EXPECT_EQ(Q({"hset", "h", "b", "2", "a", "1"}).out, "2\n");
  EXPECT_EQ(Q({"hset", "h", "a", "9"}).out, "0\n");
  EXPECT_EQ(Q({"hget", "h", "a"}).out, "9\n");
  EXPECT_EQ(Q({"hget", "h", "zz"}).out, "(nil)\n");
  EXPECT_EQ(Q({"hexists", "h", "b"}).out, "true\n");
  EXPECT_EQ(Q({"hlen", "h"}).out, "2\n");
  EXPECT_EQ(Q({"hgetall", "h"}).out, "a\t9\nb\t2\n");
  EXPECT_EQ(J({"hgetall", "h"}).out,
            "{\"ok\":true,\"result\":[[\"a\",\"9\"],[\"b\",\"2\"]]}\n");
  EXPECT_EQ(Q({"hincr", "h", "n", "5"}).out, "5\n");
  EXPECT_EQ(Q({"hdel", "h", "a", "n", "nope"}).out, "2\n");
  EXPECT_EQ(Q({"hset", "h", "odd"}).code, 1);
  EXPECT_EQ(Q({"hset", "h", "f1", "v1", "f2"}).code, 1);
}

TEST_F(CliTest, Lists) {
  EXPECT_EQ(Q({"rpush", "l", "b", "c"}).out, "2\n");
  EXPECT_EQ(Q({"lpush", "l", "a", "z"}).out, "4\n");  // z a b c
  EXPECT_EQ(Q({"lrange", "l", "0", "-1"}).out, "z\na\nb\nc\n");
  EXPECT_EQ(Q({"lindex", "l", "1"}).out, "a\n");
  EXPECT_EQ(Q({"lindex", "l", "9"}).out, "(nil)\n");
  EXPECT_EQ(Q({"lset", "l", "0", "Z"}).out, "true\n");
  EXPECT_EQ(Q({"lset", "l", "9", "x"}).out, "false\n");
  EXPECT_EQ(Q({"lpop", "l"}).out, "Z\n");
  EXPECT_EQ(Q({"rpop", "l", "2"}).out, "c\nb\n");
  EXPECT_EQ(J({"lpop", "l", "5"}).out, "{\"ok\":true,\"result\":[\"a\"]}\n");
  EXPECT_EQ(Q({"llen", "l"}).out, "0\n");
  EXPECT_EQ(Q({"exists", "l"}).out, "false\n");  // an empty list is gone
  EXPECT_EQ(Q({"rpush", "t", "1", "2", "3", "4"}).out, "4\n");
  ASSERT_EQ(Q({"ltrim", "t", "1", "2"}).code, 0);
  EXPECT_EQ(Q({"lrange", "t", "0", "-1"}).out, "2\n3\n");
}

TEST_F(CliTest, Sets) {
  EXPECT_EQ(Q({"sadd", "a", "1", "2", "3"}).out, "3\n");
  EXPECT_EQ(Q({"sadd", "b", "3", "4"}).out, "2\n");
  EXPECT_EQ(Q({"smembers", "a"}).out, "1\n2\n3\n");
  EXPECT_EQ(Q({"sismember", "a", "2"}).out, "true\n");
  EXPECT_EQ(Q({"scard", "a"}).out, "3\n");
  EXPECT_EQ(Q({"sunion", "a", "b"}).out, "1\n2\n3\n4\n");
  EXPECT_EQ(Q({"sinter", "a", "b"}).out, "3\n");
  EXPECT_EQ(Q({"sdiff", "a", "b"}).out, "1\n2\n");
  EXPECT_EQ(Q({"srem", "a", "1", "9"}).out, "1\n");
}

TEST_F(CliTest, SortedSets) {
  EXPECT_EQ(Q({"zadd", "z", "91.5", "ann", "78", "bo", "100", "cy"}).out,
            "3\n");
  EXPECT_EQ(Q({"zrange", "z", "0", "-1"}).out, "bo\nann\ncy\n");
  EXPECT_EQ(Q({"zrange", "z", "-2", "-1", "--withscores"}).out,
            "ann\t91.5\ncy\t100\n");
  EXPECT_EQ(J({"zrange", "z", "0", "0", "--withscores"}).out,
            "{\"ok\":true,\"result\":[[\"bo\",78]]}\n");
  EXPECT_EQ(Q({"zrangebyscore", "z", "80", "inf"}).out, "ann\ncy\n");
  EXPECT_EQ(Q({"zrangebyscore", "z", "-inf", "90", "--withscores"}).out,
            "bo\t78\n");
  EXPECT_EQ(Q({"zscore", "z", "ann"}).out, "91.5\n");
  EXPECT_EQ(Q({"zscore", "z", "nobody"}).out, "(nil)\n");
  EXPECT_EQ(Q({"zrank", "z", "cy"}).out, "2\n");
  EXPECT_EQ(Q({"zcard", "z"}).out, "3\n");
  EXPECT_EQ(Q({"zincr", "z", "bo", "30"}).out, "108\n");
  EXPECT_EQ(Q({"zrem", "z", "ann", "x"}).out, "1\n");
  EXPECT_EQ(Q({"zadd", "z", "1"}).code, 1);
  EXPECT_EQ(Q({"zadd", "z", "inf", "top"}).out, "1\n");
  EXPECT_EQ(J({"zscore", "z", "top"}).out,
            "{\"ok\":true,\"result\":\"inf\"}\n");
}

TEST_F(CliTest, BatchAppliesEverythingOrNothing) {
  ASSERT_EQ(Q({"set", "text", "abc"}).code, 0);
  Run_ ok = J({"batch", "decr", "stock", "2", "--", "incr", "sold", "2", "--",
               "set", "order:1", "paid", "--", "hset", "h", "f", "v"});
  EXPECT_EQ(ok.code, 0);
  EXPECT_EQ(ok.out, "{\"ok\":true,\"result\":{\"applied\":4}}\n");
  EXPECT_EQ(Q({"mget", "stock", "sold", "order:1"}).out, "-2\n2\npaid\n");

  Run_ failing = J({"batch", "set", "new", "x", "--", "incr", "text", "--",
                    "set", "newer", "y"});
  EXPECT_EQ(failing.code, 2);
  EXPECT_NE(failing.out.find("NOT_INTEGER"), std::string::npos);
  EXPECT_EQ(Q({"exists", "new"}).out, "false\n");
  EXPECT_EQ(Q({"exists", "newer"}).out, "false\n");
  EXPECT_EQ(Q({"get", "text"}).out, "abc\n");
}

TEST_F(CliTest, BatchChecksEveryCommandBeforeApplyingAny) {
  EXPECT_EQ(Q({"batch", "set", "a", "1", "--", "frobnicate", "x"}).code, 1);
  EXPECT_EQ(Q({"batch", "set", "a", "1", "--", "incr", "n", "x"}).code, 1);
  EXPECT_EQ(Q({"batch", "set", "a", "1", "--", "--", "set", "b", "2"}).code, 1);
  EXPECT_EQ(Q({"batch", "set", "a", "1", "--"}).code, 1);
  EXPECT_EQ(Q({"batch", "get", "a"}).code, 1);  // reads cannot be batched
  EXPECT_EQ(Q({"batch", "set", "a"}).code, 1);
  EXPECT_EQ(Q({"exists", "a"}).out, "false\n");

  Run_ r = Q({"batch", "set", "a", "1", "--", "incr", "n", "x"});
  EXPECT_NE(r.err.find("command 2 (incr)"), std::string::npos) << r.err;
}

TEST_F(CliTest, BatchValuesMayStartWithDashes) {
  ASSERT_EQ(
      Q({"batch", "set", "k", "--weird", "--", "rpush", "l", "--a", "-b"}).code,
      0);
  EXPECT_EQ(Q({"get", "k"}).out, "--weird\n");
  EXPECT_EQ(Q({"lrange", "l", "0", "-1"}).out, "--a\n-b\n");
}

TEST_F(CliTest, EveryBatchableCommandWorksInABatch) {
  Run_ r = J(
      {"batch", "set",   "s",       "v",     "10s", "--",    "append", "s",
       "!",     "--",    "rename",  "s",     "s2",  "--",    "expire", "s2",
       "1h",    "--",    "persist", "s2",    "--",  "incr",  "n",      "5",
       "--",    "decr",  "n",       "1",     "--",  "hset",  "h",      "a",
       "1",     "--",    "hincr",   "h",     "n",   "2",     "--",     "hdel",
       "h",     "a",     "--",      "rpush", "l",   "1",     "2",      "3",
       "--",    "lpush", "l",       "0",     "--",  "lset",  "l",      "0",
       "Z",     "--",    "lpop",    "l",     "--",  "rpop",  "l",      "--",
       "ltrim", "l",     "0",       "0",     "--",  "sadd",  "set",    "a",
       "b",     "--",    "srem",    "set",   "a",   "--",    "zadd",   "z",
       "1",     "a",     "2",       "b",     "--",  "zincr", "z",      "a",
       "5",     "--",    "zrem",    "z",     "b",   "--",    "del",    "gone"});
  ASSERT_EQ(r.code, 0) << r.out << r.err;
  EXPECT_EQ(Q({"get", "s2"}).out, "v!\n");
  EXPECT_EQ(Q({"get", "n"}).out, "4\n");
  EXPECT_EQ(Q({"hgetall", "h"}).out, "n\t2\n");
  EXPECT_EQ(Q({"lrange", "l", "0", "-1"}).out, "1\n");
  EXPECT_EQ(Q({"smembers", "set"}).out, "b\n");
  EXPECT_EQ(Q({"zrange", "z", "0", "-1", "--withscores"}).out, "a\t6\n");
}

TEST_F(CliTest, ReadCommandsDoNotCreateTheFile) {
  for (const char* cmd :
       {"get", "exists", "type", "ttl", "keys", "hlen", "llen", "scard",
        "zcard", "xlen", "xlastid", "stats"}) {
    std::vector<std::string> args = {cmd};
    if (std::string(cmd) != "keys" && std::string(cmd) != "stats")
      args.push_back("k");
    Run_ r = Q(args);
    EXPECT_EQ(r.code, 2) << cmd;
    EXPECT_NE(r.err.find("NOT_FOUND"), std::string::npos) << cmd;
  }
  EXPECT_FALSE(std::filesystem::exists(db_));

  ASSERT_EQ(Q({"set", "k", "v"}).code, 0);  // a write creates it
  EXPECT_TRUE(std::filesystem::exists(db_));
}

TEST_F(CliTest, TheDatabaseCanComeFromTheEnvironment) {
  setenv("QEY_DB", db_.c_str(), 1);
  ASSERT_EQ(Raw({"set", "k", "from-env"}).code, 0);
  EXPECT_EQ(Raw({"get", "k"}).out, "from-env\n");

  const std::string other = dir_ + "/other.qey";
  ASSERT_EQ(Raw({"--db", other, "set", "k", "from-flag"}).code, 0);
  EXPECT_EQ(Raw({"--db", other, "get", "k"}).out, "from-flag\n");
  EXPECT_EQ(Raw({"get", "k"}).out,
            "from-env\n");  // the flag won, not the variable
}

TEST_F(CliTest, NoDatabaseGivenIsAUsageError) {
  Run_ r = Raw({"get", "k"});
  EXPECT_EQ(r.code, 1);
  EXPECT_NE(r.err.find("--db"), std::string::npos);
  EXPECT_NE(r.err.find("QEY_DB"), std::string::npos);
}

TEST_F(CliTest, DataPersistsBetweenInvocations) {
  ASSERT_EQ(Q({"set", "k", "v", "--ttl", "1h"}).code, 0);
  ASSERT_EQ(Q({"hset", "h", "f", "v"}).code, 0);
  ASSERT_EQ(Q({"rpush", "l", "a", "b"}).code, 0);
  EXPECT_EQ(Q({"get", "k"}).out, "v\n");
  EXPECT_GT(std::stoll(Q({"ttl", "k"}).out), 0);
  EXPECT_EQ(Q({"hgetall", "h"}).out, "f\tv\n");
  EXPECT_EQ(Q({"lrange", "l", "0", "-1"}).out, "a\nb\n");
}

TEST_F(CliTest, ADatabaseThatIsOpenElsewhereIsReportedNotWaitedFor) {
  StatusOr<std::unique_ptr<Db>> held = Db::Open(db_);
  ASSERT_TRUE(held.ok());
  Run_ r = J({"set", "k", "v"});
  EXPECT_EQ(r.code, 2);
  EXPECT_NE(r.out.find("UNAVAILABLE"), std::string::npos) << r.out;
}

TEST_F(CliTest, StatsDescribesTheDatabase) {
  ASSERT_EQ(Q({"set", "s", "v"}).code, 0);
  ASSERT_EQ(Q({"hset", "h", "f", "v"}).code, 0);
  ASSERT_EQ(Q({"rpush", "l", "a"}).code, 0);
  ASSERT_EQ(Q({"sadd", "x", "a"}).code, 0);
  ASSERT_EQ(Q({"zadd", "z", "1", "a"}).code, 0);
  Run_ r = J({"stats"});
  EXPECT_EQ(r.code, 0);
  EXPECT_NE(r.out.find("\"keys\":5"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("\"types\":{\"string\":1,\"list\":1,\"hash\":1,\"set\":"
                       "1,\"zset\":1,\"stream\":0}"),
            std::string::npos)
      << r.out;
  EXPECT_NE(r.out.find("\"file_bytes\":" +
                       std::to_string(std::filesystem::file_size(db_))),
            std::string::npos)
      << r.out;
  EXPECT_NE(r.out.find("\"bytes_dropped_at_open\":0"), std::string::npos);
}

TEST_F(CliTest, CompactShrinksTheFileAndClearEmptiesIt) {
  for (int i = 0; i < 50; ++i)
    ASSERT_EQ(Q({"set", "k", std::to_string(i)}).code, 0);
  const auto before = std::filesystem::file_size(db_);
  ASSERT_EQ(Q({"compact"}).code, 0);
  EXPECT_LT(std::filesystem::file_size(db_), before / 5);
  EXPECT_EQ(Q({"get", "k"}).out, "49\n");

  ASSERT_EQ(Q({"clear"}).code, 0);
  EXPECT_EQ(Q({"keys"}).out, "");
  EXPECT_EQ(Q({"exists", "k"}).out, "false\n");
}

TEST_F(CliTest, SyncModesAllWork) {
  for (const char* mode : {"always", "interval", "manual"}) {
    ASSERT_EQ(Raw({"--db", db_, "--sync", mode, "set", mode, "v"}).code, 0)
        << mode;
  }
  EXPECT_EQ(Q({"get", "manual"}).out, "v\n");  // flushed on exit even so
}

TEST_F(CliTest, HelpVersionAndDiscovery) {
  Run_ help = Raw({"--help"});
  EXPECT_EQ(help.code, 0);
  EXPECT_NE(help.out.find("never prompts"), std::string::npos);
  EXPECT_NE(help.out.find("exit codes"), std::string::npos);

  Run_ version = Raw({"--version"});
  EXPECT_EQ(version.code, 0);
  EXPECT_EQ(version.out.rfind("qey ", 0), 0u);

  Run_ none = Raw({});
  EXPECT_EQ(none.code, 1);
  EXPECT_NE(none.err.find("no command"), std::string::npos);

  Run_ one = Raw({"help", "set"});
  EXPECT_EQ(one.code, 0);
  EXPECT_NE(one.out.find("usage: qey set"), std::string::npos);
  EXPECT_EQ(Raw({"help", "nope"}).code, 1);

  Run_ all = Raw({"help"});
  EXPECT_EQ(all.code, 0);
  for (const CommandSpec& spec : Commands()) {
    EXPECT_NE(all.out.find(spec.usage), std::string::npos) << spec.name;
  }
}

TEST_F(CliTest, CommandsListsEverythingAsJsonForAgents) {
  Run_ r = Raw({"--json", "commands"});
  EXPECT_EQ(r.code, 0);
  EXPECT_EQ(r.out.rfind("{\"ok\":true,\"result\":[{\"name\":\"get\"", 0), 0u)
      << r.out.substr(0, 80);
  for (const CommandSpec& spec : Commands()) {
    EXPECT_NE(r.out.find("\"name\":\"" + spec.name + "\""), std::string::npos)
        << spec.name;
  }
  EXPECT_NE(r.out.find("\"writes\":true"), std::string::npos);
  EXPECT_NE(r.out.find("\"writes\":false"), std::string::npos);
}

TEST_F(CliTest, TheCommandTableIsConsistent) {
  std::set<std::string> names;
  for (const CommandSpec& spec : Commands()) {
    EXPECT_TRUE(names.insert(spec.name).second) << "duplicate " << spec.name;
    EXPECT_EQ(spec.usage.rfind(spec.name, 0), 0u) << spec.name;
    EXPECT_FALSE(spec.summary.empty()) << spec.name;
    EXPECT_LE(spec.min_args, spec.max_args) << spec.name;
    EXPECT_TRUE(spec.run) << spec.name;
    EXPECT_EQ(FindCommand(spec.name), &spec);
  }
  EXPECT_EQ(FindCommand("nope"), nullptr);
}

TEST_F(CliTest, NoCommandReadsInputUnlessAskedTo) {
  // Q() passes an input stream that fails the test if it is read.
  ASSERT_EQ(Q({"set", "k", "v"}).code, 0);
  for (const char* cmd :
       {"get", "type", "exists", "ttl", "keys", "stats", "compact"}) {
    std::vector<std::string> args = {cmd};
    if (std::string(cmd) != "keys" && std::string(cmd) != "stats" &&
        std::string(cmd) != "compact") {
      args.push_back("k");
    }
    EXPECT_EQ(Q(args).code, 0) << cmd;
  }
}

// An ID as printed: milliseconds, a dash, a sequence number.
bool LooksLikeId(const std::string& text) {
  const size_t dash = text.find('-');
  if (dash == std::string::npos || dash == 0 || dash + 1 >= text.size()) {
    return false;
  }
  for (size_t i = 0; i < text.size(); ++i) {
    if (i != dash && !std::isdigit(static_cast<unsigned char>(text[i]))) {
      return false;
    }
  }
  return true;
}

// Splits "id\tvalue\n" lines into IDs and values.
void SplitEntries(const std::string& out, std::vector<std::string>* ids,
                  std::vector<std::string>* values) {
  std::istringstream lines(out);
  std::string line;
  while (std::getline(lines, line)) {
    const size_t tab = line.find('\t');
    ASSERT_NE(tab, std::string::npos) << line;
    ids->push_back(line.substr(0, tab));
    values->push_back(line.substr(tab + 1));
  }
}

TEST_F(CliTest, StreamsAddReadAndPage) {
  std::vector<std::string> ids;
  for (const char* payload : {"one", "two", "three", "four"}) {
    Run_ r = Q({"xadd", "s", payload});
    ASSERT_EQ(r.code, 0) << r.err;
    ASSERT_EQ(r.out.back(), '\n');
    ids.push_back(r.out.substr(0, r.out.size() - 1));
    EXPECT_TRUE(LooksLikeId(ids.back())) << r.out;
  }
  for (size_t i = 1; i < ids.size(); ++i) {
    EXPECT_NE(ids[i], ids[i - 1]);
  }
  EXPECT_EQ(Q({"type", "s"}).out, "stream\n");
  EXPECT_EQ(Q({"xlen", "s"}).out, "4\n");
  EXPECT_EQ(Q({"xlastid", "s"}).out, ids.back() + "\n");
  EXPECT_EQ(Q({"xlastid", "nope"}).out, "(nil)\n");
  EXPECT_EQ(Q({"xlen", "nope"}).out, "0\n");

  // Text: one "id<TAB>value" line per entry, oldest first.
  std::vector<std::string> got_ids, values;
  SplitEntries(Q({"xrange", "s", "--count", "10"}).out, &got_ids, &values);
  EXPECT_EQ(got_ids, ids);
  EXPECT_EQ(values, (std::vector<std::string>{"one", "two", "three", "four"}));

  // JSON: objects with the ID as a string.
  EXPECT_EQ(J({"xrange", "s", "--count", "1"}).out,
            "{\"ok\":true,\"result\":[{\"id\":\"" + ids[0] +
                "\",\"value\":\"one\"}]}\n");

  // Resume after an entry, inclusive from, and to.
  got_ids.clear();
  values.clear();
  SplitEntries(Q({"xrange", "s", "--count", "10", "--after", ids[1]}).out,
               &got_ids, &values);
  EXPECT_EQ(values, (std::vector<std::string>{"three", "four"}));
  got_ids.clear();
  values.clear();
  SplitEntries(
      Q({"xrange", "s", "--count", "10", "--from", ids[1], "--to", ids[2]}).out,
      &got_ids, &values);
  EXPECT_EQ(values, (std::vector<std::string>{"two", "three"}));
  EXPECT_EQ(Q({"xrange", "s", "--count", "2"}).out,
            ids[0] + "\tone\n" + ids[1] + "\ttwo\n");
  EXPECT_EQ(Q({"xrange", "s", "--count", "0"}).out, "");
  EXPECT_EQ(Q({"xrange", "s", "--count", "5", "--from", "0"}).code, 0);
  EXPECT_EQ(Q({"xrange", "nope", "--count", "5"}).out, "");

  // The newest first.
  got_ids.clear();
  values.clear();
  SplitEntries(Q({"xrevrange", "s", "--count", "2"}).out, &got_ids, &values);
  EXPECT_EQ(values, (std::vector<std::string>{"four", "three"}));
  got_ids.clear();
  values.clear();
  SplitEntries(Q({"xrevrange", "s", "--count", "5", "--before", ids[1]}).out,
               &got_ids, &values);
  EXPECT_EQ(values, (std::vector<std::string>{"two", "one"}));
}

TEST_F(CliTest, StreamArgumentsAreCheckedBeforeAnythingChanges) {
  ASSERT_EQ(Q({"xadd", "s", "a"}).code, 0);
  for (const std::vector<std::string>& bad :
       std::vector<std::vector<std::string>>{
           {"xrange", "s"},                   // no --count
           {"xrange", "s", "--count", "x"},   // not a number
           {"xrange", "s", "--count", "-1"},  // negative
           {"xrange", "s", "--count", "5", "--from", "abc"},
           {"xrange", "s", "--count", "5", "--from", "1-2-3"},
           {"xrange", "s", "--count", "5", "--to", "-1"},
           {"xrange", "s", "--count", "5", "--from", "1", "--after", "1"},
           {"xrevrange", "s"},
           {"xrevrange", "s", "--count", "5", "--before", "x"},
           {"xadd", "s", "p", "--maxlen", "x"},
           {"xadd", "s"},                                      // no payload
           {"xadd", "s", "p", "--stdin"},                      // both
           {"xtrim", "s"},                                     // neither
           {"xtrim", "s", "--maxlen", "1", "--minid", "1-0"},  // both
           {"xtrim", "s", "--maxlen", "-1"},
           {"xtrim", "s", "--minid", "9223372036854775808-0"},  // too big
       }) {
    Run_ r = Q(bad);
    EXPECT_EQ(r.code, 1) << bad[0] << " " << r.err;
  }
  EXPECT_EQ(Q({"xlen", "s"}).out, "1\n");
}

TEST_F(CliTest, XaddKeepsOnlyTheNewestWithMaxlen) {
  for (const char* p : {"a", "b", "c", "d"}) {
    ASSERT_EQ(Q({"xadd", "s", p, "--maxlen", "2"}).code, 0);
  }
  EXPECT_EQ(Q({"xlen", "s"}).out, "2\n");
  std::vector<std::string> ids, values;
  SplitEntries(Q({"xrange", "s", "--count", "9"}).out, &ids, &values);
  EXPECT_EQ(values, (std::vector<std::string>{"c", "d"}));
}

TEST_F(CliTest, XaddReadsAPayloadFromStandardInput) {
  // A JSONL line with a newline and binary bytes survives exactly.
  const std::string payload =
      "{\"role\":\"user\",\"x\":\"\xff\"}\nsecond line\n";
  ASSERT_EQ(Q({"xadd", "s", "--stdin"}, payload).code, 0);
  Run_ r = J({"xrange", "s", "--count", "1"});
  EXPECT_NE(r.out.find("\"base64\""), std::string::npos) << r.out;  // not UTF-8
  const std::string utf8 = "{\"role\":\"user\"}\nline two";
  ASSERT_EQ(Q({"xadd", "t", "--stdin"}, utf8).code, 0);
  Run_ t = J({"xrange", "t", "--count", "1"});
  EXPECT_NE(t.out.find("\"value\":\"{\\\"role\\\":\\\"user\\\"}\\nline two\""),
            std::string::npos)
      << t.out;
  EXPECT_EQ(Q({"xadd", "t", "x", "--stdin"}, "y").code, 1);  // key only
}

TEST_F(CliTest, TrimmedStreamsStayAndRememberTheirLastId) {
  std::string last;
  for (const char* p : {"a", "b", "c", "d", "e"}) {
    Run_ r = Q({"xadd", "s", p});
    last = r.out.substr(0, r.out.size() - 1);
  }
  EXPECT_EQ(Q({"xtrim", "s", "--maxlen", "3"}).out, "2\n");
  EXPECT_EQ(Q({"xtrim", "s", "--maxlen", "3"}).out, "0\n");
  EXPECT_EQ(J({"xtrim", "s", "--minid", last}).out,
            "{\"ok\":true,\"result\":2}\n");
  EXPECT_EQ(Q({"xlen", "s"}).out, "1\n");
  EXPECT_EQ(Q({"xtrim", "s", "--maxlen", "0"}).out, "1\n");
  EXPECT_EQ(Q({"xlen", "s"}).out, "0\n");
  EXPECT_EQ(Q({"type", "s"}).out, "stream\n");  // still there
  EXPECT_EQ(Q({"xlastid", "s"}).out, last + "\n");
  EXPECT_EQ(Q({"xtrim", "nope", "--maxlen", "1"}).out, "0\n");
}

TEST_F(CliTest, StreamsAndOtherTypesAreKeptApart) {
  ASSERT_EQ(Q({"set", "str", "v"}).code, 0);
  Run_ r = J({"xadd", "str", "x"});
  EXPECT_EQ(r.code, 2);
  EXPECT_NE(r.out.find("WRONG_TYPE"), std::string::npos) << r.out;
  EXPECT_EQ(Q({"xlen", "str"}).code, 2);
  EXPECT_EQ(Q({"xrange", "str", "--count", "1"}).code, 2);

  ASSERT_EQ(Q({"xadd", "s", "a"}).code, 0);
  EXPECT_EQ(Q({"get", "s"}).code, 2);  // not nil: it is a stream
  EXPECT_EQ(Q({"lpush", "s", "x"}).code, 2);
  EXPECT_EQ(Q({"del", "s"}).out, "1\n");
  EXPECT_EQ(Q({"exists", "s"}).out, "false\n");
}

TEST_F(CliTest, StreamsInABatch) {
  ASSERT_EQ(Q({"set", "text", "abc"}).code, 0);
  Run_ ok = J({"batch", "xadd", "s", "hello", "--", "xadd", "s", "world", "--",
               "incr", "count", "--", "xtrim", "s", "maxlen", "1"});
  EXPECT_EQ(ok.code, 0) << ok.out << ok.err;
  EXPECT_EQ(ok.out, "{\"ok\":true,\"result\":{\"applied\":4}}\n");
  std::vector<std::string> ids, values;
  SplitEntries(Q({"xrange", "s", "--count", "9"}).out, &ids, &values);
  EXPECT_EQ(values, std::vector<std::string>{"world"});
  EXPECT_EQ(Q({"get", "count"}).out, "1\n");

  Run_ minid = Q({"batch", "xadd", "s", "later", "--", "xtrim", "s", "minid",
                  "9999999999999-0"});
  EXPECT_EQ(minid.code, 0) << minid.err;
  EXPECT_EQ(Q({"xlen", "s"}).out, "0\n");

  // A failing change undoes the stream change before it.
  Run_ failing = Q({"batch", "xadd", "s", "never", "--", "incr", "text"});
  EXPECT_EQ(failing.code, 2);
  EXPECT_EQ(Q({"xlen", "s"}).out, "0\n");

  EXPECT_EQ(Q({"batch", "xadd", "s"}).code, 1);
  EXPECT_EQ(Q({"batch", "xtrim", "s", "maxlen", "x"}).code, 1);
  EXPECT_EQ(Q({"batch", "xtrim", "s", "other", "1"}).code, 1);
  EXPECT_EQ(Q({"batch", "xtrim", "s", "minid", "junk"}).code, 1);
  EXPECT_EQ(Q({"batch", "xrange", "s"}).code, 1);  // reads cannot be batched
}

TEST_F(CliTest, StreamsPersistBetweenInvocations) {
  ASSERT_EQ(Q({"xadd", "s", "a"}).code, 0);
  const std::string id = Q({"xadd", "s", "b"}).out;
  ASSERT_EQ(Q({"expire", "s", "1h"}).code, 0);
  EXPECT_EQ(Q({"xlastid", "s"}).out, id);
  EXPECT_EQ(Q({"xlen", "s"}).out, "2\n");
  EXPECT_GT(std::stoll(Q({"ttl", "s"}).out), 0);
  Run_ stats = J({"stats"});
  EXPECT_NE(stats.out.find("\"stream\":1"), std::string::npos) << stats.out;
}

}  // namespace
}  // namespace qey::cli
