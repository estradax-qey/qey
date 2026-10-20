# Changelog

All notable changes are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/). Until 1.0, a minor version may
change the API.

## [0.1.0] - 2026-10-21

The first release.

### Added

- **The store.** An in-memory key-value store you link into your process, with
  optional durability in a single append-only log file that is replayed on open,
  compacts itself, and recovers from a torn tail after a crash.
- **Types.** Strings with TTLs, hashes, lists, sets, sorted sets and streams,
  with Redis-style semantics, ordered key scans and prefix queries.
- **Streams.** Append-only entries with time-based IDs that only grow, trimming
  by length or ID, and reads that resume after an ID, for session and event
  history.
- **Write batches.** Several changes applied as one unit, all or none, even
  across a crash.
- **Sync policies.** Sync on every write, once an interval, or manually.
- **`qey`, a command-line tool.** It never prompts, has a `--json` mode with
  stable error codes and exit codes, and can read values from standard input.
- **`qey_bench`.** Measures speed, memory per element and file size.
