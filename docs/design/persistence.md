# Persistence design

Status: proposed

## Goals

- `Db::Open(path)` gives a durable store in a **single file**, SQLite-style.
- In-memory speed: reads never touch the disk. The file is only appended to
  and read at open.
- A crash or power loss never corrupts the store. At worst the most recent
  writes that were not yet synced are lost, according to the sync policy.
- No dependencies. POSIX (macOS, Linux) first.

Non-goals (for now): multi-process access, replication, encryption,
compression, background threads, group commit.

## Approach: a log that compacts into its own snapshot

Instead of a snapshot section plus a separate WAL, the file is **one
append-only log**. A "snapshot" is simply a freshly compacted log that holds
one `SET` per live key. This keeps a single record format and one replay
path.

- **Open:** replay every record in order to rebuild the in-memory map.
- **Write:** append a record, then apply the change in memory.
- **Compact:** rewrite the live state into a new file and atomically replace
  the old one (below).

## File format

All integers are little-endian.

```
file   := header record*
header := magic[8] "QEYDB\0\0\1"  version:u32 (=1)  reserved:u32 (=0)   ; 16 bytes
record := length:u32  crc32:u32  type:u8  payload[length - 1]
```

- `length` counts `type` plus `payload`. `crc32` (IEEE) covers `type` and
  `payload`.
- Strings are `len:u32 bytes`.
- Expiry is an **absolute Unix time in milliseconds** (`i64`), or `-1` for
  none. The in-memory store uses a monotonic clock, which is meaningless
  after a restart, so times are converted at the file boundary using the
  wall-clock offset at that moment.

| type | name    | payload                          |
|------|---------|----------------------------------|
| 1    | SET     | key, value, expires_at_ms:i64    |
| 2    | DEL     | key                              |
| 3    | EXPIRE  | key, expires_at_ms:i64           |
| 4    | PERSIST | key                              |

Other operations log as these. `Incr`/`Decr` log a `SET` with the resulting
value and the key's current expiry, so replay never re-does arithmetic.
Lazy expiry and `Sweep()` are **not** logged: expiry is derived from the
stored absolute times, and keys already expired at replay are skipped.

Unknown record types make `Open` fail with `kCorruption` rather than being
skipped, so a newer file is never silently misread by an older reader.

## Durability

`OpenOptions::sync`:

| policy    | behavior                                  | use                   |
|-----------|-------------------------------------------|-----------------------|
| `kAlways` | sync after every write (default)          | no acknowledged loss  |
| `kManual` | never sync; owner calls `Db::Sync()`      | caches, bulk loads    |

`kAlways` is the default because a durable store that quietly loses data is
the worse surprise. A timer-based policy is left to the owner (call `Sync()`
periodically, as with `Sweep()`), since the library starts no threads.

"Sync" means `fsync`, and `fcntl(F_FULLFSYNC)` on macOS, where plain `fsync`
does not flush the drive's cache.

A write is acknowledged (returns OK) only after the record is appended and,
under `kAlways`, synced. The change is applied to memory only if the append
succeeded, so memory never runs ahead of the file.

## Crash recovery

A crash can only damage the **tail** of the log (a torn or partial last
record). On open, replay stops at the first record that is truncated or has a
bad CRC.

- Default (`Recovery::kTruncate`): truncate the file to the end of the last
  good record, sync, and report the number of dropped bytes in
  `Db::recovery_info()`.
- `Recovery::kStrict`: fail with `kCorruption` instead.

A bad record that is followed by more valid-looking data is not the
signature of a torn tail. In that case `Open` fails with `kCorruption` even
under `kTruncate`, rather than discarding good data after the damage.

## Compaction

Triggered automatically after a write when the file size exceeds both a
minimum (4 MiB) and 2x the size of the file right after the last compaction.
It can also be called explicitly with `Db::Compact()`.

1. Write header + one `SET` per live key to `<path>.tmp`.
2. `fsync` the temp file, then `rename` it over `<path>`.
3. `fsync` the containing directory.

A crash before step 2 leaves the old file untouched and a stray `.tmp`, which
`Open` deletes. After step 2 the new file is complete. The store is
**logically one file**; `.tmp` exists only during compaction.

Compaction runs synchronously under the store's lock, blocking other callers
while it runs. Simple first; background compaction is a possible later step.

## Locking

`Open` takes an exclusive advisory `flock` on the file and fails with
`kUnavailable` if another process holds it. During compaction the new file is
locked before the rename, so the lock is held continuously.

## API changes

Writes can now fail, so they report a `Status`. This is a breaking change to
the in-memory API (`feat!`):

```cpp
struct OpenOptions {
  SyncPolicy sync = SyncPolicy::kAlways;
  Recovery recovery = Recovery::kTruncate;
};

static StatusOr<std::unique_ptr<Db>> Open(std::string_view path,
                                           OpenOptions options = {});
Db();  // unchanged: in-memory only, never fails

Status Set(key, value, ttl = nullopt);
StatusOr<bool> Del(key);
StatusOr<bool> Expire(key, ttl);
StatusOr<bool> Persist(key);
Status Sync();      // flush to disk (no-op in memory)
Status Compact();   // rewrite the log (no-op in memory)
```

`Get`, `Exists`, `GetTtl`, `Size` and `Sweep` cannot fail and keep their
signatures. New status codes: `kIoError`, `kCorruption`, `kUnavailable`,
`kInvalidArgument`.

## Testing

- A small `File` interface (append, sync, truncate, rename) lets tests inject
  write failures and torn writes.
- Replay equivalence: for random operation sequences, a reopened store equals
  the live one.
- Torn tail: truncate a valid log at **every byte offset** and check that
  `Open` recovers to a prefix of the operations.
- Corruption: flip bits in the middle and tail, and check the two outcomes.
- Compaction: a crash simulated before and after the rename.

## Implementation plan

1. `feat!(db)`: writes return `Status`; new status codes (no persistence yet).
2. `feat(log)`: record encoding, CRC, `File` interface, replay, with tests.
3. `feat(db)`: `Db::Open`, append-on-write, sync policy, recovery.
4. `feat(db)`: compaction, locking, `Sync()`/`Compact()`.

## Open questions

- Is POSIX-only acceptable for now? (Assumed yes.)
- Is the 4 MiB / 2x compaction rule a reasonable default?
