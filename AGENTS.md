# AGENTS.md

## Project

`qey` is an embedded, in-memory key-value store written in C++, in the spirit
of SQLite: a library you link into your process, with no server required.
It is inspired by Redis/Valkey. Licensed under MIT.

## Build and test

- Build system: Bazel (via Bazelisk). Modules are managed with `MODULE.bazel`.
- Build everything: `bazel build //...`
- Run all tests: `bazel test //...`
- Language standard: C++20 (set in `.bazelrc`).
- CI (`.github/workflows/ci.yml`) runs `bazel test //...` on Linux and macOS
  for every push to `main` and every pull request.

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
style). A pre-commit hook in `.githooks/` rejects commits with unformatted
C++ files. Enable it once per clone:

```
git config core.hooksPath .githooks
```

If the hook reports files, run the `clang-format -i` command it prints and
re-stage them. Do not bypass the hook with `--no-verify`.

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

- `qey/` - library sources and headers, with a `BUILD.bazel` per package.
- Tests sit next to the code as `*_test.cc`.

## Conventions

- Keep changes small and tested; add or update tests with every behavior change.
- Do not add dependencies without discussing them first.
