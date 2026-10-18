#!/bin/sh
# Drives the real qey binary as a separate process. $1 is the path to it.
set -u

QEY="$1"
DIR="${TEST_TMPDIR:-$(mktemp -d)}"
DB="$DIR/smoke.qey"
failures=0

fail() {
  echo "FAIL: $1" >&2
  failures=$((failures + 1))
}

expect_eq() {  # description, expected, actual
  [ "$2" = "$3" ] || fail "$1: expected [$2], got [$3]"
}

# Run qey with the database and capture stdout, stderr and the exit code.
run() {
  out=$("$QEY" --db "$DB" "$@" 2>"$DIR/err" </dev/null)
  code=$?
  err=$(cat "$DIR/err")
}

run set greeting hello
expect_eq "set exit code" 0 "$code"
expect_eq "set output" "OK" "$out"

run get greeting
expect_eq "get output" "hello" "$out"
expect_eq "get stderr is empty" "" "$err"

# --json puts success on stdout as one line.
run --json get greeting
expect_eq "json get" '{"ok":true,"result":"hello"}' "$out"

# A missing key is a normal answer: exit code 0.
run --json get nope
expect_eq "json missing" '{"ok":true,"result":null}' "$out"
expect_eq "missing exit code" 0 "$code"

# A usage error is exit code 1, with the explanation on stderr only.
run incr
expect_eq "usage exit code" 1 "$code"
expect_eq "usage leaves stdout empty" "" "$out"
case "$err" in *"usage: qey incr"*) ;; *) fail "usage message: $err" ;; esac

# A database error is exit code 2; in --json mode it is on stdout, not stderr.
run lpush greeting x
expect_eq "db error exit code" 2 "$code"
run --json lpush greeting x
expect_eq "db error exit code (json)" 2 "$code"
expect_eq "json errors leave stderr empty" "" "$err"
case "$out" in '{"ok":false,"error":{"code":"WRONG_TYPE"'*) ;; *) fail "json error: $out" ;; esac

# Reading from a database that does not exist does not create it.
"$QEY" --db "$DIR/absent.qey" get k >/dev/null 2>&1 </dev/null
expect_eq "missing database exit code" 2 "$?"
[ ! -e "$DIR/absent.qey" ] || fail "a read created the database file"

# The value can be piped in, byte for byte.
printf 'line one\nline two\0end' | "$QEY" --db "$DB" set blob --stdin >/dev/null
"$QEY" --db "$DB" get blob </dev/null | head -c 21 >"$DIR/blob.out"
printf 'line one\nline two\0end' >"$DIR/blob.expected"
cmp -s "$DIR/blob.out" "$DIR/blob.expected" || fail "binary value was changed"

# It never reads standard input unless asked: it works with it closed.
"$QEY" --db "$DB" get greeting <&- >"$DIR/closed.out" 2>&1
expect_eq "works with stdin closed" "hello" "$(cat "$DIR/closed.out")"

# A literal value that looks like an option, after "--".
run set -- dashed --not-an-option
expect_eq "dashed value exit code" 0 "$code"
run get dashed
expect_eq "dashed value" "--not-an-option" "$out"

# The environment can name the database; the flag wins.
out=$(QEY_DB="$DB" "$QEY" get greeting </dev/null)
expect_eq "QEY_DB" "hello" "$out"

# A batch is atomic: the bad command leaves the first one undone.
run set text abc
run batch set fresh 1 -- incr text
expect_eq "failed batch exit code" 2 "$code"
run exists fresh
expect_eq "failed batch changed nothing" "false" "$out"

# Several changes at once.
run batch decr stock 2 -- incr sold 2 -- set order paid
expect_eq "batch exit code" 0 "$code"
run mget stock sold order
expect_eq "batch results" "-2
2
paid" "$out"

# An agent can discover the commands.
run --json commands
case "$out" in '{"ok":true,"result":[{"name":"get"'*) ;; *) fail "commands: $out" ;; esac

# Persistence between processes, including a TTL.
run set ephemeral v --ttl 1h
run ttl ephemeral
[ "$out" -gt 0 ] 2>/dev/null || fail "ttl not positive: $out"

[ "$failures" -eq 0 ] || { echo "$failures check(s) failed" >&2; exit 1; }
echo "all smoke checks passed"
