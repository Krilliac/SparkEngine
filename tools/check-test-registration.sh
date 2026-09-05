#!/bin/sh
#
# check-test-registration.sh — guard against test files silently dropping out of the build.
#
# Recursively inventories Tests/Test*.cpp, Tests/Test*.mm, Tests/*Probe.cpp and
# nested equivalents, then verifies each relative path is referenced in
# Tests/CMakeLists.txt. Relative paths matter: duplicate basenames in different
# directories must not allow an unregistered source to hide behind a registered
# one. Non-Test* harnesses (the RemoteDebug boundary probe) are inventoried too;
# they were invisible to the guard while never being built by anything.
#
# Registration is classified, not just detected. A source listed inside an
# if()/endif() block is only compiled when that condition holds, so counting it
# as "registered" reported OK for tests no configuration built. Such sources are
# reported as CONDITIONAL REGISTRATION and counted in the summary; set
# SPARK_TEST_REGISTRATION_STRICT=1 to make them a failure.
#
# Opt-out: a test file that contains the comment "// test-registration: ignore"
# is intentionally excluded and not reported.
#
# Exit 0 when every on-disk test is registered (or ignored); exit 1 otherwise.
#
# POSIX sh only (find / grep / sed / awk / basename) — no bash-isms, no ripgrep.

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=${SPARK_TEST_REGISTRATION_ROOT:-$(cd -- "$SCRIPT_DIR/.." && pwd)}
STRICT=${SPARK_TEST_REGISTRATION_STRICT:-0}

TESTS_DIR="$REPO_ROOT/Tests"
CMAKE_LISTS="$TESTS_DIR/CMakeLists.txt"

if [ ! -f "$CMAKE_LISTS" ]; then
    echo "check-test-registration: $CMAKE_LISTS not found" >&2
    exit 1
fi

# Emit "<conditional-depth> <relative-path>" for every test source named in an
# active (non-comment) CMake line. Depth is the if()/endif() nesting the mention
# sits in, so a source that only ever appears at depth > 0 can be told apart
# from one the build always compiles.
extract_registrations() {
    awk '
        {
            line = $0
            sub(/[ \t]*#.*$/, "", line)

            opens = line
            closes = line
            n_open = gsub(/(^|[^A-Za-z0-9_])if[ \t]*\(/, "", opens)
            n_close = gsub(/(^|[^A-Za-z0-9_])endif[ \t]*\(/, "", closes)

            rest = line
            while (match(rest, /([A-Za-z0-9_.-]+\/)*(Test[A-Za-z0-9_.-]+|[A-Za-z0-9_.-]*Probe)\.(cpp|mm)/)) {
                token = substr(rest, RSTART, RLENGTH)
                sub(/^Tests\//, "", token)
                print depth " " token
                rest = substr(rest, RSTART + RLENGTH)
            }

            depth += n_open - n_close
            if (depth < 0)
                depth = 0
        }
    ' "$CMAKE_LISTS"
}

work_dir=$(mktemp -d "${TMPDIR:-/tmp}/spark-test-registration.XXXXXX") || exit 1
trap 'rm -rf "$work_dir"' EXIT HUP INT TERM

extract_registrations > "$work_dir/registrations"
awk '$1 == 0 { print $2 }' "$work_dir/registrations" | sort -u > "$work_dir/always"
awk '$1 > 0  { print $2 }' "$work_dir/registrations" | sort -u > "$work_dir/conditional"

# Set operations run through grep -f over whole lists rather than one grep per
# file: the inventory is ~700 sources and process spawns dominate on Windows.
find "$TESTS_DIR" -type f \
    \( -name 'Test*.cpp' -o -name 'Test*.mm' -o -name '*Probe.cpp' \) -print \
    | sed "s|^$TESTS_DIR/||" \
    | sort > "$work_dir/inventory"

grep -Fxv -f "$work_dir/always" "$work_dir/inventory" > "$work_dir/unregistered" || true

# Opt-out check only needs to look at sources that are not already registered.
: > "$work_dir/candidates"
while IFS= read -r relative; do
    [ -n "$relative" ] || continue
    if grep -q '// test-registration: ignore' "$TESTS_DIR/$relative" 2>/dev/null; then
        continue
    fi
    printf '%s\n' "$relative" >> "$work_dir/candidates"
done < "$work_dir/unregistered"

grep -Fx -f "$work_dir/conditional" "$work_dir/candidates" > "$work_dir/conditional-hits" || true
grep -Fxv -f "$work_dir/conditional" "$work_dir/candidates" > "$work_dir/missing-hits" || true

conditional=0
while IFS= read -r relative; do
    [ -n "$relative" ] || continue
    echo "CONDITIONAL REGISTRATION: Tests/$relative (compiled only when its if() block is taken)" >&2
    conditional=$((conditional + 1))
done < "$work_dir/conditional-hits"

missing=0
while IFS= read -r relative; do
    [ -n "$relative" ] || continue
    echo "MISSING REGISTRATION: Tests/$relative" >&2
    missing=$((missing + 1))
done < "$work_dir/missing-hits"

if [ "$missing" -gt 0 ]; then
    echo "check-test-registration: $missing unregistered test file(s) found —" >&2
    echo "  add them to Tests/CMakeLists.txt or mark with '// test-registration: ignore'" >&2
    exit 1
fi

if [ "$conditional" -gt 0 ] && [ "$STRICT" = "1" ]; then
    echo "check-test-registration: $conditional conditionally registered test file(s) —" >&2
    echo "  SPARK_TEST_REGISTRATION_STRICT=1 requires every test source to build in every configuration" >&2
    exit 1
fi

echo "check-test-registration: OK — every Tests/Test*.cpp, Test*.mm and *Probe.cpp source is registered" \
     "($conditional of them only inside a conditional block)"
exit 0
