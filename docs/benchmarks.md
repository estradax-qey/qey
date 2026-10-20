# Benchmarks

`qey_bench` measures three things: **speed** (throughput and latency),
**memory** (the footprint of the data in RAM) and **file size** (the `.qey`
log on disk).

```
bazel run -c opt //bench:qey_bench                       # the tables below
bazel run -c opt //bench:qey_bench -- --json > run.json  # for tracking over time
bazel run -c opt //bench:qey_bench -- --section memory --keys 1000000
bazel run -c opt //bench:qey_bench -- --help
```

Always build with `-c opt`. Without it the tool says so and the timings mean
little. `--quick` runs a small workload, only to check that everything works;
a test does this on every push.

## How it measures

**Speed.** Each scenario runs N operations. The whole run is timed once, which
gives the throughput without any per-call overhead. Every 16th operation is
also timed on its own to get the latency percentiles (every operation, for slow
ones), so the clock does not distort the fast ones. Keys are visited in a
shuffled order, so a lookup is a cache miss as in real use, not a walk through
neighbouring memory. The collection scenarios spread their operations over
100 to 1000 keys. In-memory scenarios use a `Db` with no file; the
`persistent` ones use a file in a temporary directory and show what the log,
and flushing it, cost.

**Memory.** Each scenario fills a database in a **forked child process** and
reads the process's memory use before and after, while the data is still alive.
A fresh process has a small, known baseline, and an allocator that keeps freed
memory cannot carry it from one scenario to the next. On macOS the number is
the physical footprint (what Activity Monitor shows), which includes pages the
system has compressed; the resident size would leave those out and make a big
heap look much smaller than it is. On Linux it is the resident set size.
"Payload" is the keys and values themselves, so "x payload" is what storing
them costs on top.

**File size.** Each scenario writes every element once with one call each (the
usual way a log grows), writes each one again, and compacts, reading the size of
the file after each step. Auto-compaction is off so the growth can be seen.

The numbers vary between runs: file sizes not at all, memory per element by
under 1%, speed by a few percent, and by up to about 20% for the scenarios that
touch new memory (a new key grows the table). Compare runs on the same machine,
when it is not busy.

## A run

```
qey benchmark
  cpu:      Apple M1
  os:       Darwin 24.6.0 arm64
  compiler: Apple LLVM 17.0.0 (clang-1700.3.19.1)
  workload: 200,000 keys, 64-byte values, seed 1

SPEED
scenario                                  ops/sec       mean        p50        p99      p99.9        max
string set (new key)                    3,061,796     327 ns     250 ns    2.25 us    5.12 us   99.79 us
string set (overwrite)                  3,201,679     312 ns     292 ns    1.04 us    3.29 us   10.12 us
string get (hit)                        6,031,188     166 ns     167 ns     458 ns     916 ns   34.71 us
string get (miss)                      11,116,566      90 ns      42 ns     459 ns     625 ns    1.92 us
incr (1000 counters)                    8,431,792     119 ns     125 ns     167 ns     375 ns    4.79 us
keys index build (first scan)                   -   47.79 ms   47.79 ms   47.79 ms   47.79 ms   47.79 ms
    (one-time cost, then kept up to date)
scan a page of 100 keys                   122,641    8.15 us    7.96 us   11.21 us   21.83 us   25.71 us
write batch of 10 sets                    193,557    5.17 us    4.88 us   10.46 us   18.08 us  110.58 us
    (each operation is a batch of 10)
string del                              1,423,844     702 ns     667 ns    1.42 us    3.92 us   14.75 us
hash hset (new field)                   2,409,966     415 ns     375 ns    2.25 us    5.12 us   11.67 us
    (1000 hashes)
hash hget                              11,843,108      84 ns      83 ns     167 ns     667 ns    3.29 us
list rpush                              4,115,706     243 ns     209 ns    1.00 us    1.71 us   23.42 us
    (100 lists)
list lpop                               6,269,559     160 ns     167 ns     291 ns    1.50 us   14.46 us
set sadd                                2,784,665     359 ns     375 ns    3.92 us    6.58 us   17.50 us
    (100 sets)
set sismember                          13,663,535      73 ns      83 ns     208 ns    2.12 us    6.62 us
zset zadd                               1,356,717     737 ns     708 ns    1.29 us    2.17 us    5.67 us
    (100 sorted sets)
zset zscore                            13,470,621      74 ns      83 ns     167 ns     667 ns    1.08 us
zset zrank                                 42,606   23.47 us   19.88 us   80.83 us  121.62 us  528.25 us
    (walks the order index: cost grows with the rank)
zset zrange (first 10)                  4,225,910     237 ns     208 ns     250 ns     458 ns   15.17 us
stream xadd                             4,936,438     203 ns     208 ns     334 ns    3.50 us    9.67 us
    (100 streams)
stream xrevrange (newest 20)            1,481,166     675 ns     625 ns     792 ns     958 ns   15.12 us
    (the last messages of a session)
stream xrange (page of 100)               184,212    5.43 us    5.67 us    7.38 us   16.50 us   24.21 us
    (from a random entry onwards)
stream xadd in a write batch              307,996    3.25 us    2.96 us    7.00 us   11.12 us  213.33 us
    (copies the stream for rollback: slower as it grows (here up to 200 entries))
stream xadd (keep newest 1000)          3,133,554     319 ns     333 ns    1.38 us    3.33 us    6.50 us
    (100 streams, each trimmed to 1000 entries)
persistent set (sync: manual)             435,949    2.29 us    2.04 us    5.42 us    8.54 us  731.83 us
    (appends to the log; flushed only on Sync())
persistent xadd (sync: manual)            439,322    2.28 us    2.04 us    5.08 us   10.17 us   16.85 ms
    (100 streams; appends to the log, flushed only on Sync())
persistent set (sync: every 1s)           446,438    2.24 us    2.04 us    5.38 us    9.17 us   16.96 us
    (flushes at most once a second)
persistent set (sync: always)                 264    3.78 ms    3.98 ms    5.66 ms   15.09 ms   15.09 ms
    (flushes to disk on every write; 1000 writes)
open and replay the log                   741,455    1.35 us
    (ops are log records: 400000 for 200000 keys)
compact the log                           738,125    1.35 us
    (ops are live keys written; the log held 400000 records)

MEMORY (footprint of a fresh process, database in memory only)
scenario                                elements     payload      memory  per element  x payload
strings: one key each                    200,000    14.5 MiB    37.5 MiB      196.4 B      2.58x
hash: one hash of N fields               200,000    14.9 MiB    31.1 MiB      163.1 B      2.09x
hashes: many hashes of 10 fields         200,000    15.1 MiB    34.0 MiB      178.4 B      2.25x
stream: one stream of N entries          200,000    12.2 MiB    23.2 MiB      121.7 B      1.90x
streams: many of 100 entries             200,000    12.2 MiB    23.8 MiB      124.5 B      1.94x
list: one list of N elements             200,000    12.2 MiB    20.2 MiB      105.7 B      1.65x
set: one set of N members                200,000     2.9 MiB    12.5 MiB       65.5 B      4.36x
sorted set: one set of N members         200,000     4.4 MiB    24.8 MiB      130.1 B      5.66x
  payload = the keys and values themselves; "x payload" is the overhead of storing them.

FILE SIZE (the .qey log)
scenario                                  payload          after load       after rewrite       after compact
strings: one key each                    14.5 MiB    19.3 MiB (1.33x)    38.5 MiB (2.66x)    19.3 MiB (1.33x)
hash: N fields, one call each            14.9 MiB    20.0 MiB (1.35x)    40.1 MiB (2.69x)    16.4 MiB (1.10x)
stream: N entries, one call each         12.2 MiB    19.8 MiB (1.62x)    39.7 MiB (3.25x)    19.8 MiB (1.62x)
list: N elements, one call each          12.2 MiB    16.6 MiB (1.36x)    34.5 MiB (2.83x)    13.0 MiB (1.06x)
set: N members, one call each             2.9 MiB     7.2 MiB (2.53x)    21.6 MiB (7.53x)     3.6 MiB (1.27x)
sorted set: N members, one call each      4.4 MiB     8.8 MiB (2.00x)    17.5 MiB (4.00x)     5.1 MiB (1.17x)
  "load" writes every element once, one call each; "rewrite" writes each one again;
  compaction keeps one copy of each. Multiples are of the payload.
```

(Apple M1, an optimized build, 200,000 keys of 64-byte values. Your numbers
will differ; the shape should not.)

## What the numbers say

- **Reads are cheap, writes cost a lookup plus a log record.** A hit on a
  string takes about 160 ns on average; hash, set and sorted-set lookups about
  70 to 85 ns on small collections. Writing to a file adds the cost of encoding
  and appending a record (about 2 us), which is why a persistent write is about
  7 times slower than the in-memory one.
- **Flushing on every write is the expensive part.** With `SyncPolicy::kAlways`
  a write waits for the disk: here about 4 ms, or roughly 260 writes a second,
  against 440,000 a second when flushing at most once a second
  (`kInterval`). That is a choice between losing nothing acknowledged and
  losing up to a second.
- **Memory is about 2.6 times the data for small strings.** A string key
  costs about 196 bytes for 76 bytes of key and value: the hash table node, the
  key, the entry, and the value, each allocated separately. Collections are
  cheaper per element (a hash field about 163 bytes for 78, a list element
  about 106 for 64) and a set member 65 for 15, which is a small member
  carrying a fixed overhead. This is the area with the most room to improve.
- **The log is bigger than the data, and compaction shrinks it.** Every
  record carries a length and a checksum (8 bytes) and a type, so writing a small
  element on its own costs several times the element: a 15-byte set member
  takes about 38 bytes in the log. Writing a value again appends a second
  record rather than overwriting, so a rewrite roughly doubles the file until it
  is compacted. After compaction a collection is written in large pieces and is
  within 6 to 27% of its payload; a string, which compaction writes as the same
  record it started as, stays at 1.33 times.
- **Streams are the cheapest write there is, and trimming is nearly free.**
  Appending an entry takes about 200 ns (5 million a second), because it is a
  push onto a deque. Keeping only the newest 1000 adds about 120 ns
  (320 ns in all). Reading the newest 20 entries of a session takes 0.7 us, and
  a page of 100 from the middle 5.4 us (a binary search by ID, then a copy). An
  entry costs about 122 bytes of memory for a 64-byte payload (1.9 times it),
  and 104 bytes in the log (1.62 times), which does not shrink on compaction
  because each entry is one record. Adding an entry with `max_len` plans the
  add and the trim together and logs them as one record, so it does not depend
  on the length of the stream.
- **A `WriteBatch` that touches a stream copies it.** To undo the batch if the
  log write fails, a batch keeps a copy of every key it changes, so a batch
  with a stream in it costs time in proportion to the stream's length. Here the
  streams reach 200 entries and an add in a batch takes 3.3 us, against 0.2 us
  alone. For a very long stream, use `XAdd` with `max_len` rather than a batch.
- **`ZRANK` is the slow one.** It walks the ordered index, so it costs time
  proportional to the rank (21 us here at an average rank of 1,000). Everything
  else in the sorted set is fast.
- **Writes have a latency tail; reads mostly do not.** For reads the 99th
  percentile is 2 to 3 times the median. For writes it is 5 to 10 times (a new
  key: 209 ns median, 2.1 us at the 99th), and the 99.9th up to about 25 times,
  from the hash table growing and from memory allocation. The worst single
  operation is tens of microseconds.
- **Do not read too much into the smallest latencies.** The clock on this
  machine ticks about every 42 ns, so the percentiles of the fastest operations
  fall on multiples of that (42, 83, 125 ns ...). The throughput and mean are
  not affected, as they come from timing the whole run.
