#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "bench/bench.h"
#include "qey/db.h"
#include "qey/write_batch.h"

namespace qey::bench {
namespace {

// A benchmark that cannot do its work must not report numbers for it.
void Must(const Status& s, const char* what) {
  if (!s.ok()) {
    std::fprintf(stderr, "benchmark failed at %s: %s\n", what,
                 s.message().c_str());
    std::abort();
  }
}
template <typename T>
void Must(const StatusOr<T>& r, const char* what) {
  Must(r.status(), what);
}

std::string Numbered(const char* prefix, size_t i, int width) {
  char buffer[48];
  std::snprintf(buffer, sizeof buffer, "%s%0*zu", prefix, width, i);
  return buffer;
}
std::string KeyName(size_t i) { return Numbered("key:", i, 8); }
std::string FieldName(size_t i) { return Numbered("field:", i, 8); }
std::string MemberName(size_t i) { return Numbered("member:", i, 8); }
std::string GroupName(const char* prefix, size_t i) {
  return Numbered(prefix, i, 5);
}

std::vector<std::string> Names(size_t n, std::string (*name)(size_t)) {
  std::vector<std::string> names;
  names.reserve(n);
  for (size_t i = 0; i < n; ++i) names.push_back(name(i));
  return names;
}

std::vector<size_t> Shuffled(size_t n, uint64_t seed) {
  std::vector<size_t> order(n);
  for (size_t i = 0; i < n; ++i) order[i] = i;
  std::mt19937_64 rng(seed);
  std::shuffle(order.begin(), order.end(), rng);
  return order;
}

uint64_t FileBytes(const std::string& path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  return ec ? 0 : static_cast<uint64_t>(size);
}

// How many operations to time one by one: all of them when each is slow
// enough that reading the clock does not matter.
size_t SampleEvery(size_t ops) { return ops > 20000 ? 16 : 1; }

// ---- Speed ----------------------------------------------------------------

void AddSpeed(Report* report, std::string name, Timing timing,
              std::string note = "") {
  report->speed.push_back({std::move(name), std::move(note), timing});
}

void InMemorySpeed(const Config& config, Report* report) {
  const size_t n = config.keys;
  const size_t sample = SampleEvery(n);
  const std::vector<std::string> keys = Names(n, KeyName);
  const std::vector<size_t> order = Shuffled(n, config.seed);
  const std::string value(config.value_size, 'v');

  Db db;
  AddSpeed(report, "string set (new key)", Measure(n, sample, [&](size_t i) {
             Must(db.Set(keys[order[i]], value), "set");
           }));
  AddSpeed(report, "string set (overwrite)", Measure(n, sample, [&](size_t i) {
             Must(db.Set(keys[order[i]], value), "set");
           }));
  AddSpeed(report, "string get (hit)", Measure(n, sample, [&](size_t i) {
             auto v = db.Get(keys[order[i]]);
             Consume(v);
           }));
  std::vector<std::string> missing;
  missing.reserve(n);
  for (size_t i = 0; i < n; ++i) missing.push_back(Numbered("miss:", i, 8));
  AddSpeed(report, "string get (miss)", Measure(n, sample, [&](size_t i) {
             auto v = db.Get(missing[i]);
             Consume(v);
           }));

  const std::vector<std::string> counters =
      Names(1000, [](size_t i) { return GroupName("ctr:", i); });
  AddSpeed(report, "incr (1000 counters)", Measure(n, sample, [&](size_t i) {
             Must(db.Incr(counters[i % counters.size()]), "incr");
           }));

  // The first scan builds the ordered index of keys; later ones only use it.
  AddSpeed(report, "keys index build (first scan)",
           Measure(1, 1,
                   [&](size_t) {
                     ScanPage page = db.Scan("key:", 1);
                     Consume(page);
                   }),
           "one-time cost, then kept up to date");
  std::optional<std::string> cursor;
  AddSpeed(report, "scan a page of 100 keys",
           Measure(std::min<size_t>(n / 100 + 1, 2000), 1, [&](size_t) {
             ScanPage page = db.Scan("key:", 100, cursor);
             cursor = page.next;
             Consume(page);
           }));

  const size_t batches = std::max<size_t>(n / 10, 1);
  AddSpeed(report, "write batch of 10 sets",
           Measure(batches, SampleEvery(batches),
                   [&](size_t i) {
                     WriteBatch batch;
                     for (size_t k = 0; k < 10; ++k) {
                       batch.Set(keys[order[(i * 10 + k) % n]], value);
                     }
                     Must(db.Write(batch), "batch");
                   }),
           "each operation is a batch of 10");
  AddSpeed(report, "string del", Measure(n, sample, [&](size_t i) {
             Must(db.Del(keys[order[i]]), "del");
           }));

  // Collections: the same N operations spread over a few hundred keys.
  {
    Db h;
    const size_t groups = 1000;
    const std::vector<std::string> hashes =
        Names(groups, [](size_t i) { return GroupName("hash:", i); });
    const std::vector<std::string> fields = Names(n, FieldName);
    AddSpeed(report, "hash hset (new field)",
             Measure(n, sample,
                     [&](size_t i) {
                       Must(h.HSet(hashes[i % groups], fields[i], value),
                            "hset");
                     }),
             "1000 hashes");
    AddSpeed(report, "hash hget", Measure(n, sample, [&](size_t i) {
               auto v = h.HGet(hashes[i % groups], fields[i]);
               Consume(v);
             }));
  }
  {
    Db l;
    const size_t groups = 100;
    const std::vector<std::string> lists =
        Names(groups, [](size_t i) { return GroupName("list:", i); });
    AddSpeed(report, "list rpush",
             Measure(n, sample,
                     [&](size_t i) {
                       Must(l.RPush(lists[i % groups], {value}), "rpush");
                     }),
             "100 lists");
    AddSpeed(report, "list lpop", Measure(n, sample, [&](size_t i) {
               auto v = l.LPop(lists[i % groups]);
               Consume(v);
             }));
  }
  {
    Db s;
    const size_t groups = 100;
    const std::vector<std::string> sets =
        Names(groups, [](size_t i) { return GroupName("set:", i); });
    const std::vector<std::string> members = Names(n, MemberName);
    AddSpeed(report, "set sadd",
             Measure(n, sample,
                     [&](size_t i) {
                       Must(s.SAdd(sets[i % groups], {members[i]}), "sadd");
                     }),
             "100 sets");
    AddSpeed(report, "set sismember", Measure(n, sample, [&](size_t i) {
               auto v = s.SIsMember(sets[i % groups], members[i]);
               Consume(v);
             }));
  }
  {
    Db z;
    const size_t groups = 100;
    const std::vector<std::string> zsets =
        Names(groups, [](size_t i) { return GroupName("zset:", i); });
    const std::vector<std::string> members = Names(n, MemberName);
    AddSpeed(
        report, "zset zadd",
        Measure(n, sample,
                [&](size_t i) {
                  Must(z.ZAdd(zsets[i % groups],
                              {{members[i], static_cast<double>(order[i])}}),
                       "zadd");
                }),
        "100 sorted sets");
    AddSpeed(report, "zset zscore", Measure(n, sample, [&](size_t i) {
               auto v = z.ZScore(zsets[i % groups], members[i]);
               Consume(v);
             }));
    const size_t slow = std::min<size_t>(n, 20000);
    AddSpeed(report, "zset zrank",
             Measure(slow, 1,
                     [&](size_t i) {
                       auto v = z.ZRank(zsets[i % groups], members[i]);
                       Consume(v);
                     }),
             "walks the order index: cost grows with the rank");
    AddSpeed(report, "zset zrange (first 10)",
             Measure(slow, SampleEvery(slow), [&](size_t i) {
               auto v = z.ZRange(zsets[i % groups], 0, 9);
               Consume(v);
             }));
  }
  // Streams, as for chat sessions: many streams, each appended to over time.
  {
    Db st;
    const size_t groups = 100;
    const std::vector<std::string> streams =
        Names(groups, [](size_t i) { return GroupName("stream:", i); });
    AddSpeed(report, "stream xadd",
             Measure(n, sample,
                     [&](size_t i) {
                       Must(st.XAdd(streams[i % groups], value), "xadd");
                     }),
             "100 streams");
    const size_t slow = std::min<size_t>(n, 20000);
    AddSpeed(report, "stream xrevrange (newest 20)",
             Measure(slow, SampleEvery(slow),
                     [&](size_t i) {
                       auto v = st.XRevRange(streams[i % groups], 20);
                       Consume(v);
                     }),
             "the last messages of a session");
    // Resume from an entry somewhere in the stream, as a client would.
    std::vector<std::vector<StreamId>> ids(groups);
    for (size_t g = 0; g < groups; ++g) {
      StatusOr<std::vector<StreamEntry>> all =
          st.XRange(streams[g], StreamId(), SIZE_MAX);
      Must(all, "xrange");
      for (const StreamEntry& e : *all) ids[g].push_back(e.id);
    }
    AddSpeed(report, "stream xrange (page of 100)",
             Measure(slow, SampleEvery(slow),
                     [&](size_t i) {
                       const std::vector<StreamId>& list = ids[i % groups];
                       if (list.empty()) return;
                       auto v = st.XRange(streams[i % groups],
                                          list[order[i] % list.size()], 100);
                       Consume(v);
                     }),
             "from a random entry onwards");
  }
  {
    Db st;
    const size_t groups = 100;
    const std::vector<std::string> streams =
        Names(groups, [](size_t i) { return GroupName("stream:", i); });
    // A batch keeps a copy of what it changes, to undo it if the log write
    // fails, so touching a stream in a batch costs time in proportion to the
    // stream's length.
    const size_t batches = std::max<size_t>(n / 10, 1);
    Db in_batch;
    AddSpeed(report, "stream xadd in a write batch",
             Measure(batches, SampleEvery(batches),
                     [&](size_t i) {
                       WriteBatch batch;
                       batch.XAdd(streams[i % groups], value)
                           .Set(keys[i % keys.size()], value);
                       Must(in_batch.Write(batch), "batch");
                     }),
             "copies the stream for rollback: slower as it grows (here up to "
             "200 entries)");
    AddSpeed(report, "stream xadd (keep newest 1000)",
             Measure(n, sample,
                     [&](size_t i) {
                       Must(st.XAdd(streams[i % groups], value, 1000), "xadd");
                     }),
             "100 streams, each trimmed to 1000 entries");
  }
}

void PersistentSpeed(const Config& config, const std::string& dir,
                     Report* report) {
  const size_t n = config.keys;
  const std::vector<std::string> keys = Names(n, KeyName);
  const std::string value(config.value_size, 'v');
  const std::string path = dir + "/speed.qey";

  auto open = [&](SyncPolicy sync) {
    std::filesystem::remove(path);
    OpenOptions options;
    options.sync = sync;
    options.sync_thread = false;
    options.auto_compact = false;  // measure the writes, not the housekeeping
    StatusOr<std::unique_ptr<Db>> db = Db::Open(path, options);
    Must(db, "open");
    return std::move(*db);
  };

  {
    std::unique_ptr<Db> db = open(SyncPolicy::kManual);
    AddSpeed(report, "persistent set (sync: manual)",
             Measure(n, SampleEvery(n),
                     [&](size_t i) { Must(db->Set(keys[i], value), "set"); }),
             "appends to the log; flushed only on Sync()");
  }
  {
    std::unique_ptr<Db> db = open(SyncPolicy::kManual);
    const std::vector<std::string> streams =
        Names(100, [](size_t i) { return GroupName("stream:", i); });
    AddSpeed(report, "persistent xadd (sync: manual)",
             Measure(n, SampleEvery(n),
                     [&](size_t i) {
                       Must(db->XAdd(streams[i % streams.size()], value),
                            "xadd");
                     }),
             "100 streams; appends to the log, flushed only on Sync()");
  }
  {
    std::unique_ptr<Db> db = open(SyncPolicy::kInterval);
    AddSpeed(report, "persistent set (sync: every 1s)",
             Measure(n, SampleEvery(n),
                     [&](size_t i) { Must(db->Set(keys[i], value), "set"); }),
             "flushes at most once a second");
  }
  {
    const size_t ops = std::min(config.always_ops, n);
    std::unique_ptr<Db> db = open(SyncPolicy::kAlways);
    AddSpeed(
        report, "persistent set (sync: always)",
        Measure(ops, 1,
                [&](size_t i) { Must(db->Set(keys[i], value), "set"); }),
        "flushes to disk on every write; " + std::to_string(ops) + " writes");
  }

  // Reopening replays the log; compacting rewrites it.
  {
    {
      std::unique_ptr<Db> db = open(SyncPolicy::kManual);
      for (size_t i = 0; i < n; ++i) Must(db->Set(keys[i], value), "set");
      for (size_t i = 0; i < n; ++i) Must(db->Set(keys[i], value), "set");
    }
    OpenOptions options;
    options.auto_compact = false;
    options.sync_thread = false;
    std::unique_ptr<Db> db;
    Timing open_time = Measure(1, 1, [&](size_t) {
      StatusOr<std::unique_ptr<Db>> r = Db::Open(path, options);
      Must(r, "reopen");
      db = std::move(*r);
    });
    open_time.sampled = 0;  // one timing for the whole run: no distribution
    open_time.ops = 2 * n;  // records replayed
    open_time.ops_per_second =
        static_cast<double>(open_time.ops) / open_time.seconds;
    open_time.mean_ns =
        open_time.seconds * 1e9 / static_cast<double>(open_time.ops);
    AddSpeed(report, "open and replay the log", open_time,
             "ops are log records: " + std::to_string(2 * n) + " for " +
                 std::to_string(n) + " keys");

    Timing compact =
        Measure(1, 1, [&](size_t) { Must(db->Compact(), "compact"); });
    compact.sampled = 0;
    compact.ops = n;
    compact.ops_per_second = static_cast<double>(n) / compact.seconds;
    compact.mean_ns = compact.seconds * 1e9 / static_cast<double>(n);
    AddSpeed(report, "compact the log", compact,
             "ops are live keys written; the log held " +
                 std::to_string(2 * n) + " records");
  }
  std::filesystem::remove(path);
}

// ---- Memory ---------------------------------------------------------------

void AddMemory(Report* report, std::string name, size_t elements,
               uint64_t payload, const std::function<void(Db&)>& fill) {
  MemoryResult r;
  r.name = std::move(name);
  r.elements = elements;
  r.payload_bytes = payload;
  r.use = MeasureInChild([&](const auto& begin, const auto& end) {
    Db db;
    begin();
    fill(db);
    end();  // while the database still holds everything
  });
  report->memory.push_back(std::move(r));
}

void MemorySection(const Config& config, Report* report) {
  const size_t n = config.keys;
  const size_t v = config.value_size;
  const std::string value(v, 'v');
  // The sizes of the names below, for the payload.
  const size_t key_len = KeyName(0).size();
  const size_t field_len = FieldName(0).size();
  const size_t member_len = MemberName(0).size();

  AddMemory(report, "strings: one key each", n, n * (key_len + v), [&](Db& db) {
    for (size_t i = 0; i < n; ++i) {
      Must(db.Set(KeyName(i), value), "set");
    }
  });
  AddMemory(report, "hash: one hash of N fields", n,
            key_len + n * (field_len + v), [&](Db& db) {
              for (size_t i = 0; i < n; ++i) {
                Must(db.HSet("h", FieldName(i), value), "hset");
              }
            });
  const size_t small_hashes = std::max<size_t>(n / 10, 1);
  AddMemory(report, "hashes: many hashes of 10 fields", small_hashes * 10,
            small_hashes * (key_len + 10 * (field_len + v)), [&](Db& db) {
              for (size_t i = 0; i < small_hashes * 10; ++i) {
                Must(db.HSet(KeyName(i / 10), FieldName(i % 10), value),
                     "hset");
              }
            });
  AddMemory(report, "stream: one stream of N entries", n, key_len + n * v,
            [&](Db& db) {
              for (size_t i = 0; i < n; ++i) {
                Must(db.XAdd("st", value), "xadd");
              }
            });
  // Chat sessions: 100 entries each.
  const size_t sessions = std::max<size_t>(n / 100, 1);
  AddMemory(report, "streams: many of 100 entries", sessions * 100,
            sessions * (key_len + 100 * v), [&](Db& db) {
              for (size_t i = 0; i < sessions * 100; ++i) {
                Must(db.XAdd(KeyName(i / 100), value), "xadd");
              }
            });
  AddMemory(report, "list: one list of N elements", n, key_len + n * v,
            [&](Db& db) {
              for (size_t i = 0; i < n; ++i) {
                Must(db.RPush("l", {value}), "rpush");
              }
            });
  AddMemory(report, "set: one set of N members", n, key_len + n * member_len,
            [&](Db& db) {
              for (size_t i = 0; i < n; ++i) {
                Must(db.SAdd("s", {MemberName(i)}), "sadd");
              }
            });
  const uint64_t zset_payload = key_len + n * (member_len + sizeof(double));
  AddMemory(report, "sorted set: one set of N members", n, zset_payload,
            [&](Db& db) {
              for (size_t i = 0; i < n; ++i) {
                const double score = static_cast<double>(i);
                Must(db.ZAdd("z", {{MemberName(i), score}}), "zadd");
              }
            });
}

// ---- File size --------------------------------------------------------------

struct SizeScenario {
  std::string name;
  size_t elements;
  uint64_t payload;
  std::function<void(Db&)> load;     // write every element once
  std::function<void(Db&)> rewrite;  // write each element again
};

void SizeSection(const Config& config, const std::string& dir, Report* report) {
  const size_t n = config.keys;
  const size_t v = config.value_size;
  const std::string value(v, 'v');
  const std::string other(v, 'w');
  const size_t key_len = KeyName(0).size();
  const size_t field_len = FieldName(0).size();
  const size_t member_len = MemberName(0).size();

  std::vector<SizeScenario> scenarios;
  scenarios.push_back({"strings: one key each", n, n * (key_len + v),
                       [&](Db& db) {
                         for (size_t i = 0; i < n; ++i)
                           Must(db.Set(KeyName(i), value), "set");
                       },
                       [&](Db& db) {
                         for (size_t i = 0; i < n; ++i)
                           Must(db.Set(KeyName(i), other), "set");
                       }});
  scenarios.push_back({"hash: N fields, one call each", n,
                       key_len + n * (field_len + v),
                       [&](Db& db) {
                         for (size_t i = 0; i < n; ++i) {
                           Must(db.HSet("h", FieldName(i), value), "hset");
                         }
                       },
                       [&](Db& db) {
                         for (size_t i = 0; i < n; ++i) {
                           Must(db.HSet("h", FieldName(i), other), "hset");
                         }
                       }});
  scenarios.push_back({"stream: N entries, one call each", n, key_len + n * v,
                       [&](Db& db) {
                         for (size_t i = 0; i < n; ++i)
                           Must(db.XAdd("st", value), "xadd");
                       },
                       [&](Db& db) {  // N more, then keep the newest N
                         for (size_t i = 0; i < n; ++i)
                           Must(db.XAdd("st", other), "xadd");
                         Must(db.XTrimMaxLen("st", n), "xtrim");
                       }});
  scenarios.push_back({"list: N elements, one call each", n, key_len + n * v,
                       [&](Db& db) {
                         for (size_t i = 0; i < n; ++i)
                           Must(db.RPush("l", {value}), "rpush");
                       },
                       [&](Db& db) {
                         for (size_t i = 0; i < n; ++i) {
                           Must(db.LSet("l", static_cast<int64_t>(i), other),
                                "lset");
                         }
                       }});
  scenarios.push_back(
      {"set: N members, one call each", n, key_len + n * member_len,
       [&](Db& db) {
         for (size_t i = 0; i < n; ++i) {
           Must(db.SAdd("s", {MemberName(i)}), "sadd");
         }
       },
       [&](Db& db) {  // a member that is already there changes nothing
         for (size_t i = 0; i < n; ++i) {
           Must(db.SRem("s", {MemberName(i)}), "srem");
           Must(db.SAdd("s", {MemberName(i)}), "sadd");
         }
       }});
  scenarios.push_back(
      {"sorted set: N members, one call each", n,
       key_len + n * (member_len + sizeof(double)),
       [&](Db& db) {
         for (size_t i = 0; i < n; ++i) {
           Must(db.ZAdd("z", {{MemberName(i), static_cast<double>(i)}}),
                "zadd");
         }
       },
       [&](Db& db) {
         for (size_t i = 0; i < n; ++i) {
           Must(db.ZAdd("z", {{MemberName(i), static_cast<double>(i) + 0.5}}),
                "zadd");
         }
       }});

  const std::string path = dir + "/size.qey";
  for (const SizeScenario& s : scenarios) {
    std::filesystem::remove(path);
    OpenOptions options;
    options.sync = SyncPolicy::kManual;
    options.sync_thread = false;
    options.auto_compact = false;  // let the file grow so that it can be seen
    StatusOr<std::unique_ptr<Db>> db = Db::Open(path, options);
    Must(db, "open");

    SizeResult r;
    r.name = s.name;
    r.elements = s.elements;
    r.payload_bytes = s.payload;
    s.load(**db);
    Must((*db)->Sync(), "sync");
    r.after_load = FileBytes(path);
    s.rewrite(**db);
    Must((*db)->Sync(), "sync");
    r.after_rewrite = FileBytes(path);
    Must((*db)->Compact(), "compact");
    r.after_compact = FileBytes(path);
    report->sizes.push_back(std::move(r));
  }
  std::filesystem::remove(path);
}

}  // namespace

Report RunSuite(const Config& config) {
  Report report;
  report.config = config;
  report.environment = DescribeEnvironment();

  std::string dir = config.dir;
  bool remove_dir = false;
  if (dir.empty()) {
    dir = (std::filesystem::temp_directory_path() /
           ("qey-bench-" + std::to_string(getpid())))
              .string();
    remove_dir = true;
  }
  std::filesystem::create_directories(dir);

  const bool all = config.section == "all";
  if (all || config.section == "speed") {
    InMemorySpeed(config, &report);
    PersistentSpeed(config, dir, &report);
  }
  if (all || config.section == "memory") MemorySection(config, &report);
  if (all || config.section == "size") SizeSection(config, dir, &report);

  if (remove_dir) std::filesystem::remove_all(dir);
  return report;
}

}  // namespace qey::bench
