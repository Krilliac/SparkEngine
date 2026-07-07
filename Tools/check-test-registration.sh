#!/bin/sh
#
# check-test-registration.sh — guard against test files silently dropping out of the build.
#
# Globs Tests/Test*.cpp and verifies every file's basename is referenced in
# Tests/CMakeLists.txt. This catches the failure mode where a new Test*.cpp is
# committed but never added to the CMake source list, so it never compiles or
# runs (the exact reason a batch of orphan tests went unnoticed).
#
# Opt-out: a test file that contains the comment "// test-registration: ignore"
# is intentionally excluded and not reported.
#
# Exit 0 when every on-disk test is registered (or ignored); exit 1 otherwise.
#
# POSIX sh only (find / grep / sed / basename) — no bash-isms, no ripgrep.

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)

TESTS_DIR="$REPO_ROOT/Tests"
CMAKE_LISTS="$TESTS_DIR/CMakeLists.txt"

if [ ! -f "$CMAKE_LISTS" ]; then
    echo "check-test-registration: $CMAKE_LISTS not found" >&2
    exit 1
fi

# Extract every Test*.cpp token referenced in the CMake source lists.
# grep exits non-zero when there are no matches; guard so 'set -u' loops still run.
registered=$(grep -oE 'Test[A-Za-z0-9_]+\.cpp' "$CMAKE_LISTS" 2>/dev/null | sort -u)

missing=0
for test_file in "$TESTS_DIR"/Test*.cpp; do
    # Guard against the literal glob pattern when no files match.
    [ -e "$test_file" ] || continue

    base=$(basename -- "$test_file")

    # Opt-out: file explicitly excluded from the build.
    if grep -q '// test-registration: ignore' "$test_file" 2>/dev/null; then
        continue
    fi

    if printf '%s\n' "$registered" | grep -qx "$base"; then
        continue
    fi

    echo "MISSING REGISTRATION: Tests/$base" >&2
    missing=$((missing + 1))
done

if [ "$missing" -eq 0 ]; then
    echo "check-test-registration: OK — all Tests/Test*.cpp are registered in CMakeLists.txt"
    exit 0
fi

echo "check-test-registration: $missing unregistered test file(s) found —" >&2
echo "  add them to Tests/CMakeLists.txt or mark with '// test-registration: ignore'" >&2
exit 1
