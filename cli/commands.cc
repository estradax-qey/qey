#include "cli/commands.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "qey/db.h"
#include "qey/write_batch.h"

namespace qey::cli {
namespace {

using std::chrono::milliseconds;

// ---- Reading arguments --------------------------------------------------

std::optional<int64_t> ParseInt(const std::string& text) {
  int64_t value;
  const char* end = text.data() + text.size();
  auto [ptr, ec] = std::from_chars(text.data(), end, value);
  if (text.empty() || ec != std::errc() || ptr != end) return std::nullopt;
  return value;
}

std::optional<size_t> ParseCount(const std::string& text) {
  std::optional<int64_t> value = ParseInt(text);
  if (!value.has_value() || *value < 0) return std::nullopt;
  return static_cast<size_t>(*value);
}

// A finite number or an infinity, never NaN.
std::optional<double> ParseDouble(const std::string& text) {
  if (text.empty() || std::isspace(static_cast<unsigned char>(text[0]))) {
    return std::nullopt;
  }
  char* end = nullptr;
  errno = 0;
  const double value = std::strtod(text.c_str(), &end);
  if (end != text.c_str() + text.size() || std::isnan(value)) {
    return std::nullopt;
  }
  return value;
}

// "500ms", "30s", "5m", "2h" or "1d". A bare number is seconds.
std::optional<milliseconds> ParseDuration(const std::string& text) {
  size_t digits = 0;
  while (digits < text.size() &&
         std::isdigit(static_cast<unsigned char>(text[digits]))) {
    ++digits;
  }
  if (digits == 0) return std::nullopt;
  std::optional<int64_t> amount = ParseInt(text.substr(0, digits));
  if (!amount.has_value()) return std::nullopt;
  const std::string unit = text.substr(digits);
  int64_t scale;
  if (unit.empty() || unit == "s") {
    scale = 1000;
  } else if (unit == "ms") {
    scale = 1;
  } else if (unit == "m") {
    scale = 60 * 1000;
  } else if (unit == "h") {
    scale = 60 * 60 * 1000;
  } else if (unit == "d") {
    scale = 24 * 60 * 60 * 1000;
  } else {
    return std::nullopt;
  }
  // Keep the product inside what a duration can hold; the database refuses
  // anything longer than its own limit.
  if (*amount > std::numeric_limits<int64_t>::max() / scale / 1000) {
    return milliseconds::max();
  }
  return milliseconds(*amount * scale);
}

Outcome BadValue(std::string_view what, const std::string& got,
                 std::string_view expected) {
  return Outcome::Usage(std::string(what) + " must be " +
                        std::string(expected) + ", got \"" + got + "\"");
}

// ---- Turning results into output ----------------------------------------

Outcome Done(Result result) { return Outcome::Ok(std::move(result)); }

Outcome Wrap(const Status& s) {
  return s.ok() ? Done(Result::Ok()) : Outcome::FromStatus(s);
}
Outcome Wrap(const StatusOr<bool>& r) {
  return r.ok() ? Done(Result::Bool(*r)) : Outcome::FromStatus(r.status());
}
Outcome Wrap(const StatusOr<size_t>& r) {
  return r.ok() ? Done(Result::Int(static_cast<int64_t>(*r)))
                : Outcome::FromStatus(r.status());
}
Outcome Wrap(const StatusOr<int64_t>& r) {
  return r.ok() ? Done(Result::Int(*r)) : Outcome::FromStatus(r.status());
}
Outcome Wrap(const StatusOr<double>& r) {
  return r.ok() ? Done(Result::Double(*r)) : Outcome::FromStatus(r.status());
}
Outcome Wrap(const StatusOr<std::vector<std::string>>& r) {
  return r.ok() ? Done(Result::Strings(*r)) : Outcome::FromStatus(r.status());
}
Outcome Wrap(const StatusOr<std::optional<std::string>>& r) {
  if (!r.ok()) return Outcome::FromStatus(r.status());
  return Done(r->has_value() ? Result::String(**r) : Result::Nil());
}
Outcome Wrap(const StatusOr<std::optional<double>>& r) {
  if (!r.ok()) return Outcome::FromStatus(r.status());
  return Done(r->has_value() ? Result::Double(**r) : Result::Nil());
}
Outcome Wrap(const StatusOr<std::optional<size_t>>& r) {
  if (!r.ok()) return Outcome::FromStatus(r.status());
  return Done(r->has_value() ? Result::Int(static_cast<int64_t>(**r))
                             : Result::Nil());
}

std::string_view TypeName(KeyType type) {
  switch (type) {
    case KeyType::kNone:
      return "none";
    case KeyType::kString:
      return "string";
    case KeyType::kList:
      return "list";
    case KeyType::kHash:
      return "hash";
    case KeyType::kSet:
      return "set";
    case KeyType::kZSet:
      return "zset";
  }
  return "none";
}

Result Pair(const std::string& a, Result b) {
  Result pair = Result::Array();
  pair.items.push_back(Result::String(a));
  pair.items.push_back(std::move(b));
  return pair;
}

// The arguments from `first` on.
std::vector<std::string> Rest(const Invocation& inv, size_t first) {
  return std::vector<std::string>(
      inv.args.begin() + static_cast<std::ptrdiff_t>(first), inv.args.end());
}

// ---- Batch ---------------------------------------------------------------

// Adds one command of a batch to `batch`. Returns an error message, or
// std::nullopt if it was understood.
using BatchBuilder = std::optional<std::string> (*)(
    WriteBatch& batch, const std::vector<std::string>& args);

std::optional<std::string> NeedArgs(const std::vector<std::string>& args,
                                    size_t min, size_t max,
                                    std::string_view usage) {
  if (args.size() < min || args.size() > max) {
    return "usage: " + std::string(usage);
  }
  return std::nullopt;
}

const std::map<std::string, BatchBuilder>& BatchBuilders() {
  static const std::map<std::string, BatchBuilder> kBuilders = {
      {"set",
       [](WriteBatch& b,
          const std::vector<std::string>& a) -> std::optional<std::string> {
         if (auto e = NeedArgs(a, 2, 3, "set <key> <value> [ttl]")) return e;
         if (a.size() == 3) {
           auto ttl = ParseDuration(a[2]);
           if (!ttl)
             return "ttl must be a duration such as 30s, got \"" + a[2] + "\"";
           b.Set(a[0], a[1], *ttl);
         } else {
           b.Set(a[0], a[1]);
         }
         return std::nullopt;
       }},
      {"del",
       [](WriteBatch& b,
          const std::vector<std::string>& a) -> std::optional<std::string> {
         if (auto e = NeedArgs(a, 1, SIZE_MAX, "del <key>...")) return e;
         for (const std::string& key : a) b.Del(key);
         return std::nullopt;
       }},
      {"expire",
       [](WriteBatch& b,
          const std::vector<std::string>& a) -> std::optional<std::string> {
         if (auto e = NeedArgs(a, 2, 2, "expire <key> <duration>")) return e;
         auto ttl = ParseDuration(a[1]);
         if (!ttl) return "duration must look like 30s, got \"" + a[1] + "\"";
         b.Expire(a[0], *ttl);
         return std::nullopt;
       }},
      {"persist",
       [](WriteBatch& b,
          const std::vector<std::string>& a) -> std::optional<std::string> {
         if (auto e = NeedArgs(a, 1, 1, "persist <key>")) return e;
         b.Persist(a[0]);
         return std::nullopt;
       }},
      {"append",
       [](WriteBatch& b,
          const std::vector<std::string>& a) -> std::optional<std::string> {
         if (auto e = NeedArgs(a, 2, 2, "append <key> <value>")) return e;
         b.Append(a[0], a[1]);
         return std::nullopt;
       }},
      {"rename",
       [](WriteBatch& b,
          const std::vector<std::string>& a) -> std::optional<std::string> {
         if (auto e = NeedArgs(a, 2, 2, "rename <from> <to>")) return e;
         b.Rename(a[0], a[1]);
         return std::nullopt;
       }},
      {"incr",
       [](WriteBatch& b,
          const std::vector<std::string>& a) -> std::optional<std::string> {
         if (auto e = NeedArgs(a, 1, 2, "incr <key> [delta]")) return e;
         auto delta =
             a.size() == 2 ? ParseInt(a[1]) : std::optional<int64_t>(1);
         if (!delta) return "delta must be an integer, got \"" + a[1] + "\"";
         b.Incr(a[0], *delta);
         return std::nullopt;
       }},
      {"decr",
       [](WriteBatch& b,
          const std::vector<std::string>& a) -> std::optional<std::string> {
         if (auto e = NeedArgs(a, 1, 2, "decr <key> [delta]")) return e;
         auto delta =
             a.size() == 2 ? ParseInt(a[1]) : std::optional<int64_t>(1);
         if (!delta) return "delta must be an integer, got \"" + a[1] + "\"";
         b.Decr(a[0], *delta);
         return std::nullopt;
       }},
      {"hset",
       [](WriteBatch& b,
          const std::vector<std::string>& a) -> std::optional<std::string> {
         if (a.size() < 3 || a.size() % 2 == 0) {
           return "usage: hset <key> <field> <value> [<field> <value>...]";
         }
         std::vector<FieldValue> fields;
         for (size_t i = 1; i + 1 < a.size(); i += 2) {
           fields.emplace_back(a[i], a[i + 1]);
         }
         b.HSet(a[0], fields);
         return std::nullopt;
       }},
      {"hdel",
       [](WriteBatch& b,
          const std::vector<std::string>& a) -> std::optional<std::string> {
         if (auto e = NeedArgs(a, 2, SIZE_MAX, "hdel <key> <field>..."))
           return e;
         b.HDel(a[0], std::vector<std::string>(a.begin() + 1, a.end()));
         return std::nullopt;
       }},
      {"hincr",
       [](WriteBatch& b,
          const std::vector<std::string>& a) -> std::optional<std::string> {
         if (auto e = NeedArgs(a, 2, 3, "hincr <key> <field> [delta]"))
           return e;
         auto delta =
             a.size() == 3 ? ParseInt(a[2]) : std::optional<int64_t>(1);
         if (!delta) return "delta must be an integer, got \"" + a[2] + "\"";
         b.HIncr(a[0], a[1], *delta);
         return std::nullopt;
       }},
      {"lpush",
       [](WriteBatch& b,
          const std::vector<std::string>& a) -> std::optional<std::string> {
         if (auto e = NeedArgs(a, 2, SIZE_MAX, "lpush <key> <value>..."))
           return e;
         b.LPush(a[0], std::vector<std::string>(a.begin() + 1, a.end()));
         return std::nullopt;
       }},
      {"rpush",
       [](WriteBatch& b,
          const std::vector<std::string>& a) -> std::optional<std::string> {
         if (auto e = NeedArgs(a, 2, SIZE_MAX, "rpush <key> <value>..."))
           return e;
         b.RPush(a[0], std::vector<std::string>(a.begin() + 1, a.end()));
         return std::nullopt;
       }},
      {"lpop",
       [](WriteBatch& b,
          const std::vector<std::string>& a) -> std::optional<std::string> {
         if (auto e = NeedArgs(a, 1, 2, "lpop <key> [count]")) return e;
         auto count =
             a.size() == 2 ? ParseCount(a[1]) : std::optional<size_t>(1);
         if (!count)
           return "count must be a non-negative integer, got \"" + a[1] + "\"";
         b.LPop(a[0], *count);
         return std::nullopt;
       }},
      {"rpop",
       [](WriteBatch& b,
          const std::vector<std::string>& a) -> std::optional<std::string> {
         if (auto e = NeedArgs(a, 1, 2, "rpop <key> [count]")) return e;
         auto count =
             a.size() == 2 ? ParseCount(a[1]) : std::optional<size_t>(1);
         if (!count)
           return "count must be a non-negative integer, got \"" + a[1] + "\"";
         b.RPop(a[0], *count);
         return std::nullopt;
       }},
      {"lset",
       [](WriteBatch& b,
          const std::vector<std::string>& a) -> std::optional<std::string> {
         if (auto e = NeedArgs(a, 3, 3, "lset <key> <index> <value>")) return e;
         auto index = ParseInt(a[1]);
         if (!index) return "index must be an integer, got \"" + a[1] + "\"";
         b.LSet(a[0], *index, a[2]);
         return std::nullopt;
       }},
      {"ltrim",
       [](WriteBatch& b,
          const std::vector<std::string>& a) -> std::optional<std::string> {
         if (auto e = NeedArgs(a, 3, 3, "ltrim <key> <start> <stop>")) return e;
         auto start = ParseInt(a[1]);
         auto stop = ParseInt(a[2]);
         if (!start || !stop) return "start and stop must be integers";
         b.LTrim(a[0], *start, *stop);
         return std::nullopt;
       }},
      {"sadd",
       [](WriteBatch& b,
          const std::vector<std::string>& a) -> std::optional<std::string> {
         if (auto e = NeedArgs(a, 2, SIZE_MAX, "sadd <key> <member>..."))
           return e;
         b.SAdd(a[0], std::vector<std::string>(a.begin() + 1, a.end()));
         return std::nullopt;
       }},
      {"srem",
       [](WriteBatch& b,
          const std::vector<std::string>& a) -> std::optional<std::string> {
         if (auto e = NeedArgs(a, 2, SIZE_MAX, "srem <key> <member>..."))
           return e;
         b.SRem(a[0], std::vector<std::string>(a.begin() + 1, a.end()));
         return std::nullopt;
       }},
      {"zadd",
       [](WriteBatch& b,
          const std::vector<std::string>& a) -> std::optional<std::string> {
         if (a.size() < 3 || a.size() % 2 == 0) {
           return "usage: zadd <key> <score> <member> [<score> <member>...]";
         }
         std::vector<ScoredMember> members;
         for (size_t i = 1; i + 1 < a.size(); i += 2) {
           auto score = ParseDouble(a[i]);
           if (!score) return "score must be a number, got \"" + a[i] + "\"";
           members.push_back({a[i + 1], *score});
         }
         b.ZAdd(a[0], members);
         return std::nullopt;
       }},
      {"zrem",
       [](WriteBatch& b,
          const std::vector<std::string>& a) -> std::optional<std::string> {
         if (auto e = NeedArgs(a, 2, SIZE_MAX, "zrem <key> <member>..."))
           return e;
         b.ZRem(a[0], std::vector<std::string>(a.begin() + 1, a.end()));
         return std::nullopt;
       }},
      {"zincr",
       [](WriteBatch& b,
          const std::vector<std::string>& a) -> std::optional<std::string> {
         if (auto e = NeedArgs(a, 2, 3, "zincr <key> <member> [delta]"))
           return e;
         auto delta =
             a.size() == 3 ? ParseDouble(a[2]) : std::optional<double>(1);
         if (!delta) return "delta must be a number, got \"" + a[2] + "\"";
         b.ZIncr(a[0], a[1], *delta);
         return std::nullopt;
       }},
  };
  return kBuilders;
}

Outcome RunBatch(Invocation& inv) {
  // The commands are separated by a lone "--".
  std::vector<std::vector<std::string>> groups(1);
  for (const std::string& arg : inv.args) {
    if (arg == "--") {
      groups.emplace_back();
    } else {
      groups.back().push_back(arg);
    }
  }
  WriteBatch batch;
  for (size_t i = 0; i < groups.size(); ++i) {
    const std::vector<std::string>& group = groups[i];
    if (group.empty()) {
      return Outcome::Usage("batch: command " + std::to_string(i + 1) +
                            " is empty");
    }
    auto builder = BatchBuilders().find(group[0]);
    if (builder == BatchBuilders().end()) {
      return Outcome::Usage("batch: \"" + group[0] +
                            "\" cannot be part of a batch");
    }
    std::optional<std::string> error = builder->second(
        batch, std::vector<std::string>(group.begin() + 1, group.end()));
    if (error.has_value()) {
      return Outcome::Usage("batch: command " + std::to_string(i + 1) + " (" +
                            group[0] + "): " + *error);
    }
  }
  Result applied = Result::Object();
  applied.fields.emplace_back("applied",
                              Result::Int(static_cast<int64_t>(batch.size())));
  Status s = inv.db->Write(batch);
  if (!s.ok()) return Outcome::FromStatus(s);
  return Done(std::move(applied));
}

// ---- The command table ---------------------------------------------------

std::vector<CommandSpec> BuildCommands() {
  std::vector<CommandSpec> c;
  auto add = [&](CommandSpec spec) { c.push_back(std::move(spec)); };

  // ---- Strings and keys
  add({"get",
       "get <key>",
       "Print the string stored at <key>.",
       false,
       1,
       1,
       {},
       false,
       [](Invocation& inv) {
         Db& db = *inv.db;
         const KeyType type = db.Type(inv.args[0]);
         if (type != KeyType::kNone && type != KeyType::kString) {
           return Outcome::FromStatus(
               Status(StatusCode::kWrongType,
                      "the key holds a " + std::string(TypeName(type))));
         }
         std::optional<std::string> value = db.Get(inv.args[0]);
         return Done(value.has_value() ? Result::String(*value)
                                       : Result::Nil());
       }});
  add({"set",
       "set <key> <value> [--ttl DURATION]  |  set <key> --stdin [--ttl "
       "DURATION]",
       "Store a string, optionally expiring after DURATION (30s, 5m, 2h, 1d).",
       true,
       1,
       2,
       {{"--ttl", true}, {"--stdin", false}},
       false,
       [](Invocation& inv) {
         const bool from_stdin = inv.options.count("--stdin") > 0;
         if (from_stdin ? inv.args.size() != 1 : inv.args.size() != 2) {
           return Outcome::Usage(
               from_stdin ? "with --stdin, give only the key"
                          : "set needs a key and a value (or --stdin)");
         }
         std::string value;
         if (from_stdin) {
           value.assign(std::istreambuf_iterator<char>(*inv.in),
                        std::istreambuf_iterator<char>());
         } else {
           value = inv.args[1];
         }
         std::optional<milliseconds> ttl;
         if (auto it = inv.options.find("--ttl"); it != inv.options.end()) {
           ttl = ParseDuration(it->second);
           if (!ttl)
             return BadValue("--ttl", it->second,
                             "a duration such as 30s, 5m, 2h or 1d");
         }
         return Wrap(inv.db->Set(inv.args[0], value, ttl));
       }});
  add({"del",
       "del <key>...",
       "Delete keys of any type. Prints how many existed.",
       true,
       1,
       SIZE_MAX,
       {},
       false,
       [](Invocation& inv) {
         int64_t removed = 0;
         for (const std::string& key : inv.args) {
           StatusOr<bool> r = inv.db->Del(key);
           if (!r.ok()) return Outcome::FromStatus(r.status());
           if (*r) ++removed;
         }
         return Done(Result::Int(removed));
       }});
  add({"exists",
       "exists <key>",
       "Print whether <key> exists, whatever it holds.",
       false,
       1,
       1,
       {},
       false,
       [](Invocation& inv) {
         return Done(Result::Bool(inv.db->Exists(inv.args[0])));
       }});
  add({"type",
       "type <key>",
       "Print none, string, list, hash, set or zset.",
       false,
       1,
       1,
       {},
       false,
       [](Invocation& inv) {
         return Done(
             Result::String(std::string(TypeName(inv.db->Type(inv.args[0])))));
       }});
  add({"incr",
       "incr <key> [delta]",
       "Add delta (default 1) to an integer string.",
       true,
       1,
       2,
       {},
       false,
       [](Invocation& inv) {
         std::optional<int64_t> delta = 1;
         if (inv.args.size() == 2) delta = ParseInt(inv.args[1]);
         if (!delta) return BadValue("delta", inv.args[1], "an integer");
         return Wrap(inv.db->Incr(inv.args[0], *delta));
       }});
  add({"decr",
       "decr <key> [delta]",
       "Subtract delta (default 1) from an integer string.",
       true,
       1,
       2,
       {},
       false,
       [](Invocation& inv) {
         std::optional<int64_t> delta = 1;
         if (inv.args.size() == 2) delta = ParseInt(inv.args[1]);
         if (!delta) return BadValue("delta", inv.args[1], "an integer");
         return Wrap(inv.db->Decr(inv.args[0], *delta));
       }});
  add({"append",
       "append <key> <value>",
       "Append to a string. Prints the new length.",
       true,
       2,
       2,
       {},
       false,
       [](Invocation& inv) {
         return Wrap(inv.db->Append(inv.args[0], inv.args[1]));
       }});
  add({"rename",
       "rename <from> <to>",
       "Move a key, with its value and TTL. Prints false if <from> is missing.",
       true,
       2,
       2,
       {},
       false,
       [](Invocation& inv) {
         return Wrap(inv.db->Rename(inv.args[0], inv.args[1]));
       }});
  add({"expire",
       "expire <key> <duration>",
       "Make a key expire after the duration.",
       true,
       2,
       2,
       {},
       false,
       [](Invocation& inv) {
         std::optional<milliseconds> ttl = ParseDuration(inv.args[1]);
         if (!ttl)
           return BadValue("duration", inv.args[1], "like 30s, 5m, 2h or 1d");
         return Wrap(inv.db->Expire(inv.args[0], *ttl));
       }});
  add({"persist",
       "persist <key>",
       "Remove a key's TTL. Prints whether it had one.",
       true,
       1,
       1,
       {},
       false,
       [](Invocation& inv) { return Wrap(inv.db->Persist(inv.args[0])); }});
  add({"ttl",
       "ttl <key>",
       "Print the milliseconds left, -1 if the key never expires, -2 if it is "
       "missing.",
       false,
       1,
       1,
       {},
       false,
       [](Invocation& inv) {
         Db::TtlInfo info = inv.db->GetTtl(inv.args[0]);
         switch (info.state) {
           case Db::TtlState::kMissing:
             return Done(Result::Int(-2));
           case Db::TtlState::kNoExpiry:
             return Done(Result::Int(-1));
           case Db::TtlState::kExpiring:
             return Done(Result::Int(info.remaining.count()));
         }
         return Done(Result::Int(-2));
       }});
  add({"keys",
       "keys [prefix]",
       "List keys, in byte order, that start with the prefix.",
       false,
       0,
       1,
       {},
       false,
       [](Invocation& inv) {
         return Done(Result::Strings(
             inv.db->Keys(inv.args.empty() ? "" : inv.args[0])));
       }});
  add({"scan",
       "scan <prefix> <limit> [--after KEY]",
       "List up to <limit> keys with the prefix, starting after KEY. Prints "
       "\"next\" when there "
       "are more.",
       false,
       2,
       2,
       {{"--after", true}},
       false,
       [](Invocation& inv) {
         std::optional<size_t> limit = ParseCount(inv.args[1]);
         if (!limit)
           return BadValue("limit", inv.args[1], "a non-negative integer");
         std::optional<std::string> after;
         if (auto it = inv.options.find("--after"); it != inv.options.end()) {
           after = it->second;
         }
         ScanPage page = inv.db->Scan(inv.args[0], *limit, after);
         Result out = Result::Object();
         out.fields.emplace_back("keys", Result::Strings(page.keys));
         out.fields.emplace_back("next", page.next.has_value()
                                             ? Result::String(*page.next)
                                             : Result::Nil());
         return Done(std::move(out));
       }});
  add({"mget",
       "mget <key>...",
       "Print the strings at several keys at one instant; (nil) for others.",
       false,
       1,
       SIZE_MAX,
       {},
       false,
       [](Invocation& inv) {
         Result out = Result::Array();
         for (const std::optional<std::string>& v : inv.db->MGet(inv.args)) {
           out.items.push_back(v.has_value() ? Result::String(*v)
                                             : Result::Nil());
         }
         return Done(std::move(out));
       }});

  // ---- Hashes
  add({"hset",
       "hset <key> <field> <value> [<field> <value>...]",
       "Set hash fields. Prints how many are new.",
       true,
       3,
       SIZE_MAX,
       {},
       false,
       [](Invocation& inv) {
         if (inv.args.size() % 2 == 0) {
           return Outcome::Usage(
               "hset needs a key, then fields and values in pairs");
         }
         std::vector<FieldValue> fields;
         for (size_t i = 1; i + 1 < inv.args.size(); i += 2) {
           fields.emplace_back(inv.args[i], inv.args[i + 1]);
         }
         return Wrap(inv.db->HSet(inv.args[0], fields));
       }});
  add({"hget",
       "hget <key> <field>",
       "Print a hash field.",
       false,
       2,
       2,
       {},
       false,
       [](Invocation& inv) {
         return Wrap(inv.db->HGet(inv.args[0], inv.args[1]));
       }});
  add({"hdel",
       "hdel <key> <field>...",
       "Delete hash fields. Prints how many existed.",
       true,
       2,
       SIZE_MAX,
       {},
       false,
       [](Invocation& inv) {
         return Wrap(inv.db->HDel(inv.args[0], Rest(inv, 1)));
       }});
  add({"hexists",
       "hexists <key> <field>",
       "Print whether a hash has the field.",
       false,
       2,
       2,
       {},
       false,
       [](Invocation& inv) {
         return Wrap(inv.db->HExists(inv.args[0], inv.args[1]));
       }});
  add({"hlen",
       "hlen <key>",
       "Print the number of fields.",
       false,
       1,
       1,
       {},
       false,
       [](Invocation& inv) { return Wrap(inv.db->HLen(inv.args[0])); }});
  add({"hgetall",
       "hgetall <key>",
       "Print every [field, value], in field order.",
       false,
       1,
       1,
       {},
       false,
       [](Invocation& inv) {
         StatusOr<std::vector<FieldValue>> all = inv.db->HGetAll(inv.args[0]);
         if (!all.ok()) return Outcome::FromStatus(all.status());
         Result out = Result::Array();
         for (const FieldValue& fv : *all) {
           out.items.push_back(Pair(fv.first, Result::String(fv.second)));
         }
         return Done(std::move(out));
       }});
  add({"hincr",
       "hincr <key> <field> [delta]",
       "Add delta (default 1) to an integer field.",
       true,
       2,
       3,
       {},
       false,
       [](Invocation& inv) {
         std::optional<int64_t> delta = 1;
         if (inv.args.size() == 3) delta = ParseInt(inv.args[2]);
         if (!delta) return BadValue("delta", inv.args[2], "an integer");
         return Wrap(inv.db->HIncr(inv.args[0], inv.args[1], *delta));
       }});

  // ---- Lists
  add({"lpush",
       "lpush <key> <value>...",
       "Push values onto the head, one at a time. Prints the new length.",
       true,
       2,
       SIZE_MAX,
       {},
       false,
       [](Invocation& inv) {
         return Wrap(inv.db->LPush(inv.args[0], Rest(inv, 1)));
       }});
  add({"rpush",
       "rpush <key> <value>...",
       "Push values onto the tail. Prints the new length.",
       true,
       2,
       SIZE_MAX,
       {},
       false,
       [](Invocation& inv) {
         return Wrap(inv.db->RPush(inv.args[0], Rest(inv, 1)));
       }});
  add({"lpop",
       "lpop <key> [count]",
       "Remove and print up to count (default 1) from the head.",
       true,
       1,
       2,
       {},
       false,
       [](Invocation& inv) {
         std::optional<size_t> count = 1;
         if (inv.args.size() == 2) count = ParseCount(inv.args[1]);
         if (!count)
           return BadValue("count", inv.args[1], "a non-negative integer");
         return Wrap(inv.db->LPop(inv.args[0], *count));
       }});
  add({"rpop",
       "rpop <key> [count]",
       "Remove and print up to count (default 1) from the tail.",
       true,
       1,
       2,
       {},
       false,
       [](Invocation& inv) {
         std::optional<size_t> count = 1;
         if (inv.args.size() == 2) count = ParseCount(inv.args[1]);
         if (!count)
           return BadValue("count", inv.args[1], "a non-negative integer");
         return Wrap(inv.db->RPop(inv.args[0], *count));
       }});
  add({"llen",
       "llen <key>",
       "Print the length of a list.",
       false,
       1,
       1,
       {},
       false,
       [](Invocation& inv) { return Wrap(inv.db->LLen(inv.args[0])); }});
  add({"lindex",
       "lindex <key> <index>",
       "Print one element; negative indexes count from the end.",
       false,
       2,
       2,
       {},
       false,
       [](Invocation& inv) {
         std::optional<int64_t> index = ParseInt(inv.args[1]);
         if (!index) return BadValue("index", inv.args[1], "an integer");
         return Wrap(inv.db->LIndex(inv.args[0], *index));
       }});
  add({"lrange",
       "lrange <key> <start> <stop>",
       "Print elements start..stop, both included.",
       false,
       3,
       3,
       {},
       false,
       [](Invocation& inv) {
         std::optional<int64_t> start = ParseInt(inv.args[1]);
         std::optional<int64_t> stop = ParseInt(inv.args[2]);
         if (!start) return BadValue("start", inv.args[1], "an integer");
         if (!stop) return BadValue("stop", inv.args[2], "an integer");
         return Wrap(inv.db->LRange(inv.args[0], *start, *stop));
       }});
  add({"lset",
       "lset <key> <index> <value>",
       "Replace an element. Prints false if the key or index does not exist.",
       true,
       3,
       3,
       {},
       false,
       [](Invocation& inv) {
         std::optional<int64_t> index = ParseInt(inv.args[1]);
         if (!index) return BadValue("index", inv.args[1], "an integer");
         return Wrap(inv.db->LSet(inv.args[0], *index, inv.args[2]));
       }});
  add({"ltrim",
       "ltrim <key> <start> <stop>",
       "Keep only elements start..stop.",
       true,
       3,
       3,
       {},
       false,
       [](Invocation& inv) {
         std::optional<int64_t> start = ParseInt(inv.args[1]);
         std::optional<int64_t> stop = ParseInt(inv.args[2]);
         if (!start) return BadValue("start", inv.args[1], "an integer");
         if (!stop) return BadValue("stop", inv.args[2], "an integer");
         return Wrap(inv.db->LTrim(inv.args[0], *start, *stop));
       }});

  // ---- Sets
  add({"sadd",
       "sadd <key> <member>...",
       "Add members. Prints how many are new.",
       true,
       2,
       SIZE_MAX,
       {},
       false,
       [](Invocation& inv) {
         return Wrap(inv.db->SAdd(inv.args[0], Rest(inv, 1)));
       }});
  add({"srem",
       "srem <key> <member>...",
       "Remove members. Prints how many existed.",
       true,
       2,
       SIZE_MAX,
       {},
       false,
       [](Invocation& inv) {
         return Wrap(inv.db->SRem(inv.args[0], Rest(inv, 1)));
       }});
  add({"sismember",
       "sismember <key> <member>",
       "Print whether the set has the member.",
       false,
       2,
       2,
       {},
       false,
       [](Invocation& inv) {
         return Wrap(inv.db->SIsMember(inv.args[0], inv.args[1]));
       }});
  add({"scard",
       "scard <key>",
       "Print the number of members.",
       false,
       1,
       1,
       {},
       false,
       [](Invocation& inv) { return Wrap(inv.db->SCard(inv.args[0])); }});
  add({"smembers",
       "smembers <key>",
       "Print every member, in byte order.",
       false,
       1,
       1,
       {},
       false,
       [](Invocation& inv) { return Wrap(inv.db->SMembers(inv.args[0])); }});
  add({"sunion",
       "sunion <key>...",
       "Print the union of sets.",
       false,
       1,
       SIZE_MAX,
       {},
       false,
       [](Invocation& inv) { return Wrap(inv.db->SUnion(inv.args)); }});
  add({"sinter",
       "sinter <key>...",
       "Print the intersection of sets.",
       false,
       1,
       SIZE_MAX,
       {},
       false,
       [](Invocation& inv) { return Wrap(inv.db->SInter(inv.args)); }});
  add({"sdiff",
       "sdiff <key>...",
       "Print members of the first set that are in none of the others.",
       false,
       1,
       SIZE_MAX,
       {},
       false,
       [](Invocation& inv) { return Wrap(inv.db->SDiff(inv.args)); }});

  // ---- Sorted sets
  add({"zadd",
       "zadd <key> <score> <member> [<score> <member>...]",
       "Add members or change their scores. Prints how many are new.",
       true,
       3,
       SIZE_MAX,
       {},
       false,
       [](Invocation& inv) {
         if (inv.args.size() % 2 == 0) {
           return Outcome::Usage(
               "zadd needs a key, then scores and members in pairs");
         }
         std::vector<ScoredMember> members;
         for (size_t i = 1; i + 1 < inv.args.size(); i += 2) {
           std::optional<double> score = ParseDouble(inv.args[i]);
           if (!score) return BadValue("score", inv.args[i], "a number");
           members.push_back({inv.args[i + 1], *score});
         }
         return Wrap(inv.db->ZAdd(inv.args[0], members));
       }});
  add({"zrem",
       "zrem <key> <member>...",
       "Remove members. Prints how many existed.",
       true,
       2,
       SIZE_MAX,
       {},
       false,
       [](Invocation& inv) {
         return Wrap(inv.db->ZRem(inv.args[0], Rest(inv, 1)));
       }});
  add({"zscore",
       "zscore <key> <member>",
       "Print a member's score.",
       false,
       2,
       2,
       {},
       false,
       [](Invocation& inv) {
         return Wrap(inv.db->ZScore(inv.args[0], inv.args[1]));
       }});
  add({"zrank",
       "zrank <key> <member>",
       "Print a member's position from the lowest score, from 0.",
       false,
       2,
       2,
       {},
       false,
       [](Invocation& inv) {
         return Wrap(inv.db->ZRank(inv.args[0], inv.args[1]));
       }});
  add({"zcard",
       "zcard <key>",
       "Print the number of members.",
       false,
       1,
       1,
       {},
       false,
       [](Invocation& inv) { return Wrap(inv.db->ZCard(inv.args[0])); }});
  auto scored = [](const std::vector<ScoredMember>& members, bool with_scores) {
    Result out = Result::Array();
    for (const ScoredMember& m : members) {
      out.items.push_back(with_scores ? Pair(m.member, Result::Double(m.score))
                                      : Result::String(m.member));
    }
    return out;
  };
  add({"zrange",
       "zrange <key> <start> <stop> [--withscores]",
       "Print members at positions start..stop, lowest score first.",
       false,
       3,
       3,
       {{"--withscores", false}},
       false,
       [scored](Invocation& inv) {
         std::optional<int64_t> start = ParseInt(inv.args[1]);
         std::optional<int64_t> stop = ParseInt(inv.args[2]);
         if (!start) return BadValue("start", inv.args[1], "an integer");
         if (!stop) return BadValue("stop", inv.args[2], "an integer");
         StatusOr<std::vector<ScoredMember>> r =
             inv.db->ZRange(inv.args[0], *start, *stop);
         if (!r.ok()) return Outcome::FromStatus(r.status());
         return Done(scored(*r, inv.options.count("--withscores") > 0));
       }});
  add({"zrangebyscore",
       "zrangebyscore <key> <min> <max> [--withscores]",
       "Print members with min <= score <= max, lowest first. Use inf and -inf "
       "for no bound.",
       false,
       3,
       3,
       {{"--withscores", false}},
       false,
       [scored](Invocation& inv) {
         std::optional<double> min = ParseDouble(inv.args[1]);
         std::optional<double> max = ParseDouble(inv.args[2]);
         if (!min) return BadValue("min", inv.args[1], "a number");
         if (!max) return BadValue("max", inv.args[2], "a number");
         StatusOr<std::vector<ScoredMember>> r =
             inv.db->ZRangeByScore(inv.args[0], *min, *max);
         if (!r.ok()) return Outcome::FromStatus(r.status());
         return Done(scored(*r, inv.options.count("--withscores") > 0));
       }});
  add({"zincr",
       "zincr <key> <member> [delta]",
       "Add delta (default 1) to a score. Prints the new score.",
       true,
       2,
       3,
       {},
       false,
       [](Invocation& inv) {
         std::optional<double> delta = 1;
         if (inv.args.size() == 3) delta = ParseDouble(inv.args[2]);
         if (!delta) return BadValue("delta", inv.args[2], "a number");
         return Wrap(inv.db->ZIncr(inv.args[0], inv.args[1], *delta));
       }});

  // ---- The database as a whole
  add({"batch",
       "batch <command> [args...] [-- <command> [args...]]...",
       "Apply several changes as one atomic unit: all happen or none. "
       "Commands: set del expire "
       "persist append rename incr decr hset hdel hincr lpush rpush lpop rpop "
       "lset ltrim sadd "
       "srem zadd zrem zincr.",
       true,
       1,
       SIZE_MAX,
       {},
       true,
       RunBatch});
  add({"compact",
       "compact",
       "Rewrite the file so it holds only live data.",
       true,
       0,
       0,
       {},
       false,
       [](Invocation& inv) { return Wrap(inv.db->Compact()); }});
  add({"clear",
       "clear",
       "Delete every key.",
       true,
       0,
       0,
       {},
       false,
       [](Invocation& inv) { return Wrap(inv.db->Clear()); }});
  add({"stats",
       "stats",
       "Print the number of keys by type, the file size, and what opening "
       "found.",
       false,
       0,
       0,
       {},
       false,
       [](Invocation& inv) {
         std::map<std::string, int64_t> by_type = {
             {"string", 0}, {"list", 0}, {"hash", 0}, {"set", 0}, {"zset", 0}};
         int64_t keys = 0;
         for (const std::string& key : inv.db->Keys()) {
           ++by_type[std::string(TypeName(inv.db->Type(key)))];
           ++keys;
         }
         std::error_code ec;
         const auto size = std::filesystem::file_size(inv.db_path, ec);
         Result types = Result::Object();
         for (const char* name : {"string", "list", "hash", "set", "zset"}) {
           types.fields.emplace_back(name, Result::Int(by_type[name]));
         }
         const RecoveryInfo recovery = inv.db->recovery_info();
         Result out = Result::Object();
         out.fields.emplace_back("keys", Result::Int(keys));
         out.fields.emplace_back("types", std::move(types));
         out.fields.emplace_back(
             "file_bytes", Result::Int(ec ? 0 : static_cast<int64_t>(size)));
         out.fields.emplace_back(
             "records_replayed",
             Result::Int(static_cast<int64_t>(recovery.records_replayed)));
         out.fields.emplace_back(
             "bytes_dropped_at_open",
             Result::Int(static_cast<int64_t>(recovery.dropped_bytes)));
         return Done(std::move(out));
       }});
  return c;
}

}  // namespace

Outcome Outcome::FromStatus(const Status& status) {
  Outcome o;
  o.failure = Failure{Failure::Kind::kDatabase, StatusCodeName(status.code()),
                      status.message()};
  return o;
}

const char* StatusCodeName(StatusCode code) {
  switch (code) {
    case StatusCode::kOk:
      return "OK";
    case StatusCode::kNotInteger:
      return "NOT_INTEGER";
    case StatusCode::kOutOfRange:
      return "OUT_OF_RANGE";
    case StatusCode::kInvalidArgument:
      return "INVALID_ARGUMENT";
    case StatusCode::kIoError:
      return "IO_ERROR";
    case StatusCode::kCorruption:
      return "CORRUPTION";
    case StatusCode::kUnavailable:
      return "UNAVAILABLE";
    case StatusCode::kWrongType:
      return "WRONG_TYPE";
  }
  return "UNKNOWN";
}

const std::vector<CommandSpec>& Commands() {
  static const std::vector<CommandSpec> kCommands = BuildCommands();
  return kCommands;
}

const CommandSpec* FindCommand(std::string_view name) {
  for (const CommandSpec& spec : Commands()) {
    if (spec.name == name) return &spec;
  }
  return nullptr;
}

}  // namespace qey::cli
