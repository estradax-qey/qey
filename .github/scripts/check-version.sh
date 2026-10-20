#!/bin/sh
# Checks that the version is the same everywhere: MODULE.bazel, the version the
# command-line tool prints, and a section of CHANGELOG.md. With a tag such as
# v0.1.0 as the argument, also checks that the tag matches.
set -eu
cd "$(dirname "$0")/../.."

module=$(sed -n 's/^    version = "\(.*\)",$/\1/p' MODULE.bazel | head -n 1)
cli=$(sed -n 's/^constexpr char kVersion\[\] = "qey \(.*\)";$/\1/p' cli/cli.cc)
[ -n "$module" ] || { echo "no version found in MODULE.bazel" >&2; exit 1; }

status=0
if [ "$cli" != "$module" ]; then
  echo "MODULE.bazel says $module but cli/cli.cc says \"$cli\"" >&2
  status=1
fi
if ! grep -q "^## \[$module\]" CHANGELOG.md; then
  echo "CHANGELOG.md has no section for $module" >&2
  status=1
fi
if [ $# -ge 1 ] && [ "${1#refs/tags/}" != "v$module" ]; then
  echo "the tag ${1#refs/tags/} does not match version $module" >&2
  status=1
fi
[ "$status" -eq 0 ] && echo "version $module is consistent"
exit "$status"
