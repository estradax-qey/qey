# qey

[![CI](https://github.com/estradax/estradax-qey/actions/workflows/ci.yml/badge.svg)](https://github.com/estradax/estradax-qey/actions/workflows/ci.yml)

An embedded, in-memory key-value store for C++, inspired by SQLite and Redis/Valkey.

Status: early. The core works, but the API may still change.

```cpp
#include "qey/db.h"

// In memory only.
qey::Db cache;

// Durable: a single log file, replayed on open.
auto db = qey::Db::Open("app.qey");
if (!db.ok()) { /* db.status().message() */ }

(*db)->Set("greeting", "hello", std::chrono::seconds(60));  // optional TTL
std::optional<std::string> v = (*db)->Get("greeting");
(*db)->Incr("visits");
(*db)->Append("log", "more text");
(*db)->Rename("old-name", "new-name");

// Hashes, lists, sets and sorted sets, with Redis semantics.
(*db)->HSet("user:1", {{"name", "Ann"}, {"city", "Oslo"}});
(*db)->RPush("queue", {"job-1", "job-2"});
auto next = (*db)->LPop("queue");                       // {"job-1"}
(*db)->SAdd("tags:1", {"red", "blue"});
(*db)->ZAdd("scores", {{"ann", 91.5}, {"bo", 78}});
auto top = (*db)->ZRange("scores", -3, -1);             // highest three

// Several changes as one unit: all of them or none, even across a crash.
qey::WriteBatch batch;
batch.Decr("stock:widget", 2).Incr("sold:widget", 2).Set("order:42", "paid");
(*db)->Write(batch);  // fails whole if, say, "stock:widget" is not a number
auto values = (*db)->MGet({"stock:widget", "sold:widget"});  // read together

// Keys are ordered, so prefix queries are cheap. Page through them with Scan.
std::vector<std::string> all = (*db)->Keys("user:");
std::optional<std::string> cursor;
do {
  qey::ScanPage page = (*db)->Scan("user:", 100, cursor);
  // ... use page.keys ...
  cursor = page.next;
} while (cursor.has_value());
```

The log compacts itself as it grows, and the file is locked while open, so a
second `Open` of the same path fails with `kUnavailable`.

A key holds one type. Using a command for another type returns `kWrongType`
(`Get` returns nothing for a key that is not a string; `Type(key)` tells you
which it is), `Set` replaces whatever was there, and `Del`, `Expire`, `Rename`
and `Keys` work on every type. A collection that becomes empty disappears.
Everything above, including all the collection commands, can be part of a
`WriteBatch`.

By default every write is synced to disk before it returns. For higher write
throughput, `SyncPolicy::kInterval` syncs once per second instead, trading up
to about a second of recent writes on a crash. Time-to-live values longer than
`Db::kMaxTtl` (100 years) are rejected. See
[docs/design/persistence.md](docs/design/persistence.md) for the file format
and crash behavior.

## Build

```
bazel build //...
bazel test //...
```

## Command line

`qey` runs one command against a database file and exits. It never prompts, so
a script or an AI agent can drive it with arguments alone.

```
bazel build -c opt //cli:qey        # then use bazel-bin/cli/qey

qey --db app.qey set greeting hello --ttl 1h
qey --db app.qey get greeting                    # hello
qey --db app.qey hset user:1 name Ann city Oslo
qey --db app.qey zadd scores 91.5 ann 78 bo
qey --db app.qey zrange scores 0 -1 --withscores
qey --db app.qey batch decr stock 2 -- incr sold 2 -- set order:1 paid
echo -n "$BIG" | qey --db app.qey set blob --stdin
```

For programs, add `--json`. Every command then prints one line on standard
output, success or failure:

```
$ qey --db app.qey --json get greeting
{"ok":true,"result":"hello"}
$ qey --db app.qey --json lpush greeting x
{"ok":false,"error":{"code":"WRONG_TYPE","message":"the key holds a different type of value"}}
```

- **Exit codes:** `0` success (a missing key is a normal answer, `null`),
  `1` the command line is wrong, `2` the database refused (wrong type, I/O
  error, ...). The `code` in an error is stable: `USAGE`, `WRONG_TYPE`,
  `NOT_INTEGER`, `OUT_OF_RANGE`, `INVALID_ARGUMENT`, `NOT_FOUND`,
  `UNAVAILABLE`, `IO_ERROR`, `CORRUPTION`.
- **Discovery:** `qey commands --json` lists every command with its usage;
  `qey help <command>` explains one.
- **Binary data:** values are written as they are; in JSON, a string that is
  not valid UTF-8 is written as `{"base64": "..."}` so no byte is lost.
- **Safe by default:** a read command on a path that does not exist reports
  `NOT_FOUND` rather than creating an empty file, and a database that another
  process has open is reported as `UNAVAILABLE`, not waited for. A literal value
  that starts with `--` goes after a lone `--`.
- **Several changes in one process:** each invocation opens the file, so for
  many changes use `batch`, which applies its commands, separated by `--`,
  atomically.

## Benchmarks

`qey_bench` measures speed, memory per element and the size of the `.qey`
file. See [docs/benchmarks.md](docs/benchmarks.md) for how, and for a run.

```
bazel run -c opt //bench:qey_bench
```

## Contributing

Follow [AGENTS.md](AGENTS.md): Google C++ style (checked by CI) and
Conventional Commits.

## License

MIT. See [LICENSE](LICENSE).
