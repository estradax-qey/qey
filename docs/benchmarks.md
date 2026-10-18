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
string set (new key)                    3,216,205     311 ns     209 ns    2.08 us    5.21 us   15.83 us
string set (overwrite)                  3,364,508     297 ns     250 ns    1.08 us    4.04 us   30.54 us
string get (hit)                        6,223,961     161 ns     166 ns     417 ns     542 ns    8.33 us
string get (miss)                      12,486,310      80 ns      42 ns     458 ns     583 ns    7.54 us
incr (1000 counters)                    8,546,308     117 ns     125 ns     167 ns     375 ns    6.75 us
keys index build (first scan)                   -   44.26 ms   44.26 ms   44.26 ms   44.26 ms   44.26 ms
    (one-time cost, then kept up to date)
scan a page of 100 keys                   126,625    7.90 us    7.50 us   12.71 us   15.29 us   19.83 us
write batch of 10 sets                    203,182    4.92 us    4.62 us   10.17 us   13.83 us   46.62 us
    (each operation is a batch of 10)
string del                              1,488,941     672 ns     625 ns    1.38 us    4.33 us   20.33 us
hash hset (new field)                   2,406,586     416 ns     375 ns    2.50 us    5.33 us    8.58 us
    (1000 hashes)
hash hget                              11,783,941      85 ns      83 ns     167 ns    1.08 us    1.42 us
list rpush                              4,229,738     236 ns     209 ns     375 ns    1.58 us    7.50 us
    (100 lists)
list lpop                               6,238,384     160 ns     167 ns    1.17 us    1.33 us    1.54 us
set sadd                                2,744,800     364 ns     375 ns     583 ns     708 ns    2.67 us
    (100 sets)
set sismember                          13,486,935      74 ns      83 ns     541 ns    4.46 us    6.75 us
zset zadd                               1,397,731     715 ns     708 ns    4.58 us    7.29 us   32.71 us
    (100 sorted sets)
zset zscore                            13,941,329      72 ns      83 ns     167 ns     625 ns    1.21 us
zset zrank                                 49,409   20.24 us   17.75 us   64.62 us   87.42 us  130.67 us
    (walks the order index: cost grows with the rank)
zset zrange (first 10)                  4,141,965     241 ns     209 ns     292 ns     458 ns    8.42 us
persistent set (sync: manual)             452,632    2.21 us    2.00 us    5.62 us    8.04 us   28.17 us
    (appends to the log; flushed only on Sync())
persistent set (sync: every 1s)           445,211    2.25 us    2.00 us    5.67 us    8.75 us   27.92 us
    (flushes at most once a second)
persistent set (sync: always)                 265    3.77 ms    3.98 ms    6.02 ms   10.31 ms   10.31 ms
    (flushes to disk on every write; 1000 writes)
open and replay the log                   752,464    1.33 us
    (ops are log records: 400000 for 200000 keys)
compact the log                           839,169    1.19 us
    (ops are live keys written; the log held 400000 records)

MEMORY (footprint of a fresh process, database in memory only)
scenario                                elements     payload      memory  per element  x payload
strings: one key each                    200,000    14.5 MiB    37.5 MiB      196.4 B      2.58x
hash: one hash of N fields               200,000    14.9 MiB    31.1 MiB      162.9 B      2.09x
hashes: many hashes of 10 fields         200,000    15.1 MiB    34.0 MiB      178.3 B      2.25x
list: one list of N elements             200,000    12.2 MiB    20.2 MiB      105.8 B      1.65x
set: one set of N members                200,000     2.9 MiB    12.4 MiB       65.2 B      4.35x
sorted set: one set of N members         200,000     4.4 MiB    24.8 MiB      130.2 B      5.66x
  payload = the keys and values themselves; "x payload" is the overhead of storing them.

FILE SIZE (the .qey log)
scenario                                  payload          after load       after rewrite       after compact
strings: one key each                    14.5 MiB    19.3 MiB (1.33x)    38.5 MiB (2.66x)    19.3 MiB (1.33x)
hash: N fields, one call each            14.9 MiB    20.0 MiB (1.35x)    40.1 MiB (2.69x)    16.4 MiB (1.10x)
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
