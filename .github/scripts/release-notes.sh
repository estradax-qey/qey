#!/bin/sh
# Prints the CHANGELOG.md section for a version, such as 0.1.0, without its
# heading.
set -eu
cd "$(dirname "$0")/../.."
awk -v v="$1" '
  /^## \[/ { if (found) exit; found = ($0 ~ "^## \\[" v "\\]"); next }
  found { print }
' CHANGELOG.md
