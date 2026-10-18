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
```

The log compacts itself as it grows, and the file is locked while open, so a
second `Open` of the same path fails with `kUnavailable`.

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
