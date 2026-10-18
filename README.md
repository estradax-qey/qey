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

## Contributing

Follow [AGENTS.md](AGENTS.md): Google C++ style (checked by CI) and
Conventional Commits.

## License

MIT. See [LICENSE](LICENSE).
