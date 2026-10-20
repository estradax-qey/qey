# AGENTS.md

## Project

`qey` is an embedded, in-memory key-value store written in C++, in the spirit
of SQLite: a library you link into your process, with no server required.
It is inspired by Redis/Valkey. Licensed under MIT.

## Build and test

- Build system: Bazel (via Bazelisk). Modules are managed with `MODULE.bazel`.
- Build everything: `bazel build //...`
- Run all tests: `bazel test //...`
- Run under sanitizers: `bazel test --config=asan //...` (AddressSanitizer and
  UBSan) and `bazel test --config=tsan //...` (ThreadSanitizer). Run `tsan`
  after changing anything that involves threads or locking.
- Language standard: C++20 (set in `.bazelrc`).
- Locally, run only the tests for the code you changed (for example
  `bazel test //qey:log_test`) and leave the full suite and the sanitizer
  configs to CI. Push, then check the run with `gh run list`.
- CI (`.github/workflows/ci.yml`) checks formatting, runs `bazel test //...`
  on Linux and macOS, and runs the `asan` and `tsan` configs on Linux, for
  every push to `main` and every pull request.

## Code style

Follow the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html).
Key points:

- Files: `lower_snake_case.cc` / `.h`; headers use `#define` guards of the form
  `QEY_<PATH>_<FILE>_H_`.
- Types and enums: `PascalCase`. Functions: `PascalCase`; trivial accessors may
  be `snake_case`. Variables: `snake_case`; class data members end in `_`.
  Constants: `kPascalCase`.
- Namespace: everything lives in `qey`. No `using namespace`.
- 80-column limit, 2-space indent, no tabs.
- No exceptions; report errors via return values (e.g. a status/result type).
- No RTTI. Prefer `std::unique_ptr` over raw owning pointers.
- Include order: related header, C system, C++ standard library, other
  libraries, project headers; each group sorted.
- Comment public APIs; don't restate what the code says.

Format with `clang-format` (the repo's `.clang-format` selects the Google
style). CI rejects unformatted C++ files using `clang-format` 17.0.6, installed
with `pipx run clang-format==17.0.6`; any 17.x gives the same result. To
format locally:

```
clang-format -i $(git ls-files '*.cc' '*.h')
```

## Commit messages

Follow [Conventional Commits 1.0.0](https://www.conventionalcommits.org/en/v1.0.0/#specification):

```
<type>[optional scope][!]: <description>

[optional body]

[optional footer(s)]
```

- Types: `feat`, `fix`, `docs`, `style`, `refactor`, `perf`, `test`, `build`,
  `ci`, `chore`, `revert`.
- Scope is optional and names the area, e.g. `feat(ttl): add lazy expiry`.
- Description is imperative, lowercase, and has no trailing period.
- Breaking changes use `!` after the type/scope or a `BREAKING CHANGE:` footer.
- One logical change per commit.

## Layout

- `qey/` - the library: the store (`db`), the log format (`log`), storage
  (`storage`, `posix_storage`), value types (`value`), `write_batch`.
- `cli/` - `qey`, a non-interactive command-line tool (`qey --db FILE <command>`;
  `--json` for machine-readable output). It must never prompt or read standard
  input unless a flag asks for it.
- `bench/` - `qey_bench`, which measures speed, memory and file size. Build it
  with `-c opt` (`bazel run -c opt //bench:qey_bench`).
- `docs/` - design notes and benchmark results.
- Tests sit next to the code as `*_test.cc`.

When you add a command to the library, add it to the CLI's command table in
`cli/commands.cc` too, with a test in `cli/cli_test.cc`.

## Conventions

- Keep changes small and tested; add or update tests with every behavior change.
- Do not add dependencies without discussing them first.
