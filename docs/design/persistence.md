# Persistence design

Status: implemented

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
| 5    | APPEND  | key, suffix                      |
| 6    | RENAME  | key (source), new key            |
| 7    | BATCH   | count:u32, then count x (length:u32, body) |
| 8    | REPLACE | key, value (the TTL is kept)     |
| 9    | HSET    | key, fresh, field/value strings  |
| 10   | HDEL    | key, field strings               |
| 11   | LPUSH   | key, fresh, value strings        |
| 12   | RPUSH   | key, fresh, value strings        |
| 13   | LPOP    | key, count:i64                   |
| 14   | RPOP    | key, count:i64                   |
| 15   | LSET    | key, value string, index:i64     |
| 16   | LTRIM   | key, first:i64, last:i64         |
| 17   | SADD    | key, fresh, member strings       |
| 18   | SREM    | key, member strings              |
| 19   | ZADD    | key, fresh, member strings, one score:f64 each |
| 20   | ZREM    | key, member strings              |
| 21   | XADD    | key, fresh, payload string, ms:i64, seq:i64 |
| 22   | XTRIM   | key, ms:i64, seq:i64             |

A BATCH holds other records (any type except BATCH) as members. The members
share the BATCH's checksum, so the log has all of them or none: a crash that
tears the record drops it whole, and a bad checksum is never half-applied.

Other operations log as these. `Incr`/`Decr` log a `SET` with the resulting
value and the key's current expiry, so replay never re-does arithmetic.
`Append` logs only the suffix as an `APPEND`, so appending to a large value
does not rewrite it; but appending to a missing or expired key logs a `SET`,
so replay never depends on a stale earlier value. `Rename` is a single record,
so a crash leaves either the old name or the new one, never both or neither.
`Db::Write(WriteBatch)` logs the batch's *effective* changes, those that did
something when applied in order, as one BATCH, or as a plain record if there is
only one. Changes that name a missing key are left out, and an `Append` to a
missing or expired key becomes a `SET`, so replaying never depends on state the
log does not hold. A batch is applied to memory first, through the same function
replay uses, keeping what it overwrote; if logging then fails it is rolled
back, so memory never gets ahead of the log. Other threads cannot see the
intermediate states because the whole thing runs under the store's lock.
`Clear` has no record: it replaces the log with an empty one through the same
atomic mechanism as compaction, so the file shrinks immediately.
Lazy expiry and `Sweep()` are **not** logged: expiry is derived from the
stored absolute times, and keys already expired at replay are skipped.

### Collections

Types 9 to 22 change lists, hashes, sets and sorted sets. They share one
layout, described by a single table in the code: after the key come an
optional *fresh* byte, an optional list of strings (`count:u32` and then that
many length-prefixed strings), an optional list of scores (8-byte IEEE-754 bit
patterns, one for each string), and up to two `i64`. The same rules validate a
record when it is written and when it is read, including that a field the
layout does not use must be zero or empty.

**Only effective changes are logged**, resolved against the state at the time:
a `LTRIM` carries absolute positions, never negative ones; a `LPOP` carries how
many elements it really removed; `SADD` lists only the members that were new,
`HDEL` only the fields that were there; a `HINCR`, `ZINCR` or `Incr` logs the
resulting value, never the delta. So replay does no interpretation and cannot
disagree with what happened.

**The fresh flag.** A key can expire in memory without anything being logged,
so the log may still hold the old entry and its deadline. When a change
*creates* a collection under such a name, its record has `fresh = 1`, which
tells replay to start from an empty collection with no TTL and discard
whatever is there. Without it, replay would merge into, or be refused by, an
entry the live store had already forgotten. (For strings the same job is done
by logging `SET`, which replaces everything.)

A collection that becomes empty is removed, in memory and when replayed.

Unknown record types make `Open` fail with `kCorruption` rather than being
skipped, so a newer file is never silently misread by an older reader.

## Durability

`OpenOptions::sync`:

| policy      | behavior                                          | use                  |
|-------------|---------------------------------------------------|----------------------|
| `kAlways`   | sync after every write (default)                  | no acknowledged loss |
| `kInterval` | sync at most every `sync_interval` (default 1 s)  | most applications    |
| `kManual`   | never sync; owner calls `Db::Sync()`              | caches, bulk loads   |

`kAlways` is the default because a durable store that quietly loses data is
the worse surprise.

`kInterval` bounds the loss window instead of paying an fsync per write. Dirty
data is synced by whichever comes first: a write that arrives after the
interval has passed, a background thread that wakes every interval
(`sync_thread`, on by default), or the owner calling `Db::SyncIfDue()`. The
thread is what covers the idle case, where the last writes would otherwise sit
unsynced until the next write. Owners who want no threads can set
`sync_thread = false` and drive `SyncIfDue()` from their own timer. A crash can
lose writes from up to about one interval before it, but never corrupts the
log, because records are appended in order and recovery drops only a torn tail.

Every policy syncs unsynced writes when the `Db` is destroyed.

A failed sync, whether inline or in the background thread, puts the `Db` in the
failed state: writes and `Sync()` return `kIoError` naming the first cause,
until the store is reopened.

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

Damage counts as a torn tail only where a crash during an append could leave
it: a header or record cut short by the end of the file, a final record that
fails its checksum, or a zero-filled tail (left by some file systems). Any
other damage, such as a checksum failure with more data after the record,
makes `Open` fail with `kCorruption` even under `kTruncate`, rather than
discarding good data after the damage.

Known limitation: a corrupted length field that points past the end of the
file looks exactly like a record cut short, so everything from that record on
is treated as a torn tail and dropped. `recovery_info()` reports the dropped
byte count so callers can notice an unexpectedly large loss.

## Compaction

Runs automatically before a write once the log is larger than
`compact_min_bytes` (default 4 MiB) and `compact_ratio` (default 2) times its
size right after the last compaction. At open, the baseline is the encoded
size of the live data, so a bloated log is compacted immediately. It can also
be called explicitly with `Db::Compact()`, which additionally drops expired
keys from memory. `OpenOptions::auto_compact = false` turns the automatic
trigger off.

Compaction is done by `Storage::Replace()`; `PosixStorage` implements it as:

1. Write header + the records that recreate each live key to `<path>.tmp`
   (records are batched into ~1 MiB appends), skipping keys that have
   expired. A string is one `SET` with its deadline; a collection is written
   in pieces of about 1 MiB (the first with `fresh = 1`) followed by an
   `EXPIRE` if it has a TTL, so a collection of any size fits the record limit.
2. `fsync` the temp file, then `rename` it over `<path>`.
3. `fsync` the containing directory.

A crash before step 2 leaves the old file untouched and a stray `.tmp`, which
`Open` deletes. After step 2 the new file is complete. The store is
**logically one file**; `.tmp` exists only during compaction.

If compaction fails before the rename, the old log stays in use and the
database keeps working. A failed *automatic* compaction does not fail the
write that triggered it, and is not retried until the log has grown by
another `compact_min_bytes`. If the directory sync after the rename fails,
whether the rename is durable is unknown, and writes acknowledged to the new
file might be lost if the old one reappears after a power loss. The log then
fails every operation, so the database stops accepting writes until it is
reopened.

Compaction runs synchronously under the store's lock, blocking other callers
while it runs. It is triggered at the start of a write, before any lookup, so
memory and log agree and no iterator is outstanding. Background compaction is
a possible later step.

## Locking

`PosixStorage::Open` takes an exclusive, non-blocking `flock` on the file and
fails with `kUnavailable` if another handle holds it. This also catches a
second `Open` of the same path within one process. During compaction the new
file is locked before the rename, so the lock is held continuously. Only the
lock holder removes a stale `.tmp`.

## Storage abstraction

`Db` talks to a `Storage`, which exposes the current log as a `File` and a
`Replace(fill)` operation that atomically swaps in a rewritten log. This keeps
the file-system specifics (temp file, rename, directory sync, lock) out of
`Db` and lets tests inject failures at each step with `MemStorage`.

## API changes

Writes can now fail, so they report a `Status`. This is a breaking change to
the in-memory API (`feat!`):

```cpp
struct OpenOptions {
  SyncPolicy sync = SyncPolicy::kAlways;
  std::chrono::milliseconds sync_interval{1000};  // kInterval
  bool sync_thread = true;                        // kInterval
  Recovery recovery = Recovery::kTruncate;
  bool auto_compact = true;
  uint64_t compact_min_bytes = 4 << 20;
  uint32_t compact_ratio = 2;
};

static StatusOr<std::unique_ptr<Db>> Open(std::string_view path,
                                           OpenOptions options = {});
static StatusOr<std::unique_ptr<Db>> Open(std::unique_ptr<Storage> storage,
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

## Possible later work

- Background compaction, so writers are not blocked.
- Windows support (a `Storage` implementation).
