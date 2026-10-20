# Streams design

Status: proposed

## Goals

A stream is an append-only sequence of entries under one key. Each entry gets
an ID that qey assigns, and IDs only grow. It is the general form of a log:
chat or agent session history, event feeds, job journals, audit trails.

The motivating use is storing and caching LLM or agent session history, one
stream per session, with the application writing JSON or JSONL as the entry.
Nothing in qey knows about that: an entry is opaque bytes, so JSON, protobuf
and plain text all work.

- **Append** one entry atomically and durably, like every other write.
- **Resume**: read everything after an ID the reader already has. The ID stays
  valid after older entries are trimmed, which a list index does not.
- **Read the tail**: the last N entries, for a context window.
- **Bound the size**: drop old entries by count or by ID.
- Works with what already exists: TTL, `Del`, `Rename`, `Keys`/`Scan`,
  `WriteBatch`, compaction, and the CLI.

Non-goals (for now): consumer groups, blocking reads, per-entry TTL, field/value
entries, time-based IDs, any parsing or indexing of entry contents.

## Why not a list

A list already gives `RPush`, `LRange`, `LTrim` and a key TTL, and is enough for
a first version of the use case. It falls short in two ways. Positions shift
when the head is trimmed, so a reader cannot say "after the last message I
saw". And there is no stable identity for an entry, which callers end up
adding to the payload themselves. A stream makes both part of the type.

## Model

```
stream := last_id:u64  entries (id:u64, payload:string)*   ; ids strictly ascending
```

- **ID**: one `u64` per stream. The first entry is 1, and each `XAdd` assigns
  `last_id + 1`. IDs are never reused within a stream's life, including after
  trimming. The largest valid ID is `INT64_MAX`, since records store IDs as
  `i64`; a stream at that limit fails further adds with `kOutOfRange`.
- **Entry**: one opaque string, at most `Db::kMaxValueSize`.
- **Time**: not part of the ID. A caller that wants a timestamp puts it in the
  payload. A single counter is simpler than Redis's `ms-seq` pair, cannot go
  backwards when the clock does, and is cheap to compare and store.
- **Empty streams stay.** Trimming every entry leaves a stream with no entries
  but its `last_id`, because removing it would let IDs restart at 1 and make a
  reader's saved cursor point at the wrong entry. This differs from the other
  collections, which disappear when empty. `Del` removes the stream for real,
  and then IDs do restart; a reader that holds a cursor across a `Del` of the
  same key has to expect that. `XLen` is 0 and `XRange` returns nothing for an
  empty stream; `Type` still says stream.
- **Key type**: a new `KeyType::kStream`. Commands for another type fail with
  `kWrongType`, and `Set` replaces a stream, as for the other types.

In memory, entries sit in a `std::deque` ordered by ID, so appending and
trimming from the front are cheap and a lookup by ID is a binary search.

## API

```cpp
struct StreamEntry {
  uint64_t id;
  std::string payload;
};

// Appends `payload` and returns its ID. With `max_len`, then trims the stream
// to its newest `max_len` entries, atomically with the append.
StatusOr<uint64_t> XAdd(std::string_view key, std::string_view payload,
                        std::optional<size_t> max_len = std::nullopt);

// The number of entries; 0 for a missing key.
StatusOr<size_t> XLen(std::string_view key);

// The ID of the newest entry ever added, or nullopt for a missing key. Valid
// for an empty stream, which still remembers it.
StatusOr<std::optional<uint64_t>> XLastId(std::string_view key);

// Up to `count` entries with an ID greater than `after`, oldest first. With
// `after` of 0, from the start.
StatusOr<std::vector<StreamEntry>> XRange(std::string_view key, uint64_t after,
                                          size_t count);

// Up to `count` newest entries, newest first, optionally only those with an ID
// below `before`.
StatusOr<std::vector<StreamEntry>> XRevRange(
    std::string_view key, size_t count,
    std::optional<uint64_t> before = std::nullopt);

// Removes all but the newest `max_len` entries, or all entries with an ID below
// `min_id`. Return how many were removed. A missing key is not an error.
StatusOr<size_t> XTrimMaxLen(std::string_view key, size_t max_len);
StatusOr<size_t> XTrimMinId(std::string_view key, uint64_t min_id);
```

`XAdd`, `XTrimMaxLen` and `XTrimMinId` can be added to a `WriteBatch`, so that
a session's messages and its metadata change together. Inside a batch, the ID
`XAdd` assigns is not returned (the batch has one result), so a caller that
needs it reads `XLastId` after `Write`.

Errors follow the rest of the API: `kWrongType` for another type,
`kInvalidArgument` for an oversized key or payload, `kOutOfRange` at the ID
limit. A read of a missing key returns an empty result, not an error.

`XRange`'s `count` is required and a read holds the store's lock while it
copies, as `Keys` does. The cursor form (`after`) is what makes paging
possible; there is no unbounded read.

## Log records

Two new record types, using the collection layout table that already
validates key, `fresh`, strings and `i64` fields:

| type | name  | payload                                              |
|------|-------|------------------------------------------------------|
| 21   | XADD  | key, fresh, payload string, id:i64                   |
| 22   | XTRIM | key, min_id:i64                                      |

As with the other collections, **only effective changes are logged**, resolved
against the state at the time, so replay does no interpretation:

- `XAdd` logs the ID it assigned. Replay checks that it is greater than the
  stream's `last_id` and sets `last_id` to it. An ID that is not greater is a
  corrupt log and fails `Open` with `kCorruption`.
- A trim, whether by length or by ID, logs one `XTRIM` with an absolute
  `min_id`, never a count. It removes entries with an ID below `min_id` and
  raises `last_id` to at least `min_id - 1`. The second part is what lets one
  record express "the stream is now empty but remembers where it was".
  `XAdd` with `max_len` is a `BATCH` of an `XADD` and an `XTRIM`.
- A trim that removes nothing logs nothing. `XTrimMaxLen` on a stream that is
  within the limit is a no-op.
- `fresh` means the same as for other collections: when an add creates a stream
  under a name whose earlier stream expired in memory without being logged,
  replay starts from an empty stream with `last_id` 0 and no TTL.

Unknown record types still make an older reader fail with `kCorruption`, so a
file with streams is never silently misread by a version that lacks them. That
existing rule already gives the safe behavior, so the header version stays 1.

## Compaction

A stream is written as `XADD` records in pieces of about
`compact_chunk_bytes`, the first with `fresh = 1`, so no record outgrows the
record limit. Each carries the ID the entry already has, so IDs survive
compaction. If `last_id` is greater than the newest entry's ID (the tail was
trimmed or the stream is empty), an `XTRIM` with `min_id = last_id + 1`
follows, which restores `last_id`. An `EXPIRE` follows if the key has a TTL,
as for other collections.

## Expiry and `Rename`

A stream's TTL is the key's TTL, handled by the existing code, not per entry.
`Rename` moves the stream with its entries and `last_id`, in one record as
today. An LLM-session cache sets a TTL on the key and refreshes it (`Expire`)
when the session is used; that is the intended idle-timeout pattern.

## CLI

New commands in `cli/commands.cc`, each with a test in `cli/cli_test.cc`:

```
qey --db app.qey xadd session:42 '{"role":"user","content":"hi"}' [--maxlen N]
qey --db app.qey xlen session:42
qey --db app.qey xrange session:42 [--after ID] --count N
qey --db app.qey xrevrange session:42 --count N [--before ID]
qey --db app.qey xtrim session:42 --maxlen N        # or --minid ID
qey --db app.qey xlastid session:42
```

`--count` has no default on `xrange`, to avoid accidentally printing an
unbounded stream; `xrevrange` is meant for "the last N". In `--json` output an
entry is `{"id":7,"value":"..."}`, with the same `{"base64":...}` form for a
payload that is not valid UTF-8, so a JSONL line round-trips as a string. An
agent can parse the `value` itself. The IDs are JSON numbers; the limit of
`INT64_MAX` is above 2^53, so a JavaScript consumer that cares should read
them as strings. `--json` therefore also accepts `--ids-as-strings`.

## Memory and cost

Per entry: the payload, the ID, and the `std::deque` slot, about 40 bytes of
overhead beyond the payload on a 64-bit platform (a `std::string` is 32 bytes
and the ID 8, with small payloads stored inline). `XAdd` and front trims are
O(1) amortized and O(k) for k removed. `XRange` is O(log n + count).
`bench/` gets a stream suite measuring add throughput, tail-read latency,
memory per entry and file size, like the existing suites.

## Testing

- Unit tests: IDs ascend and are never reused after a trim; trimming to empty
  keeps `last_id`; `max_len` is atomic with the add; `XRange` pages cover
  every entry exactly once; wrong-type errors; limits.
- Replay equivalence: random operation sequences, reopen, compare the full
  state including `last_id`.
- Torn tail at every byte offset for a log with streams.
- Compaction: a stream larger than a chunk (using `compact_chunk_bytes`), an
  empty stream with a high `last_id`, a stream with a TTL.
- `fresh`: a stream that expired in memory and is recreated under the same
  key, then replayed.
- A corrupt log with a non-increasing ID fails `Open` with `kCorruption`.
- `asan` and `tsan` for concurrent `XAdd` and `XRange`.

**Where tests run.** Locally, run only the targets for the code being changed,
for example `bazel test //qey:log_test`, so each step stays quick. The full
suite, the Linux and macOS jobs, and the `asan` and `tsan` configs run in
GitHub Actions on every push and pull request. A slow test, such as one that
builds a large collection, belongs in CI and should be kept small with
`compact_chunk_bytes` rather than by building huge data.

## Implementation plan

1. `feat(log)`: `XADD` and `XTRIM` records, with encode, decode and validation.
2. `feat(db)`: the stream type, `XAdd`, `XLen`, `XLastId`, `XRange`,
   `XRevRange`, `XTrimMaxLen`, `XTrimMinId`, replay and compaction.
3. `feat(db)`: stream operations in `WriteBatch`.
4. `feat(cli)`: the commands above.
5. `feat(bench)`: a stream suite.
6. `docs`: README, this note's status, and `persistence.md`'s record table.

## Open questions

- **Field/value entries.** Redis streams hold field/value pairs. Opaque entries
  are simpler and cover JSONL. Pairs could be added later as a second entry
  encoding without changing IDs or the log layout. This note assumes opaque.
- **Empty streams that stay.** Keeping `last_id` costs one entry's worth of
  memory per emptied stream and breaks the "empty collections disappear" rule.
  The alternative is to let a trimmed-empty stream disappear and accept that
  cursors may restart. This note keeps the stream, because a wrong cursor
  silently skips or repeats history.
- **IDs as a single counter versus `ms-seq`.** A single counter is assumed. If
  time-range queries matter, a payload timestamp plus a client-side filter
  covers most cases; a time-based ID can be a later, separate type.
