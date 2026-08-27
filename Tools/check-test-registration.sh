#!/bin/sh
#
# check-test-registration.sh — guard against test files silently dropping out of the build.
#
# Recursively inventories Tests/Test*.cpp, Tests/Test*.mm, and nested Test*
# sources, then verifies each relative path is referenced in Tests/CMakeLists.txt.
# Relative paths matter: duplicate basenames in different directories must not
# allow an unregistered source to hide behind a registered one.
#
# Opt-out: a test file that contains the comment "// test-registration: ignore"
# is intentionally excluded and not reported.
#
# Exit 0 when every on-disk test is registered (or ignored); exit 1 otherwise.
#
# POSIX sh only (find / grep / sed / basename) — no bash-isms, no ripgrep.

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=${SPARK_TEST_REGISTRATION_ROOT:-$(cd -- "$SCRIPT_DIR/.." && pwd)}

TESTS_DIR="$REPO_ROOT/Tests"
CMAKE_LISTS="$TESTS_DIR/CMakeLists.txt"

if [ ! -f "$CMAKE_LISTS" ]; then
    echo "check-test-registration: $CMAKE_LISTS not found" >&2
    exit 1
fi

# Extract normalized relative Test*.cpp/Test*.mm paths from active CMake lines.
# Strip comments first so a commented-out source cannot masquerade as registered.
registered=$(
    sed 's/[[:space:]]*#.*$//' "$CMAKE_LISTS" \
        | grep -oE '([A-Za-z0-9_.-]+/)*Test[A-Za-z0-9_.-]+\.(cpp|mm)' \
        | sed 's|^Tests/||' \
        | sort -u \
        || true
)

inventory=$(mktemp "${TMPDIR:-/tmp}/spark-test-registration.XXXXXX") || exit 1
trap 'rm -f "$inventory"' EXIT HUP INT TERM
find "$TESTS_DIR" -type f \( -name 'Test*.cpp' -o -name 'Test*.mm' \) -print \
    | sort > "$inventory"

missing=0
while IFS= read -r test_file; do
    [ -n "$test_file" ] || continue
    relative=${test_file#"$TESTS_DIR"/}

    # Opt-out: file explicitly excluded from the build.
    if grep -q '// test-registration: ignore' "$test_file" 2>/dev/null; then
        continue
    fi

    if printf '%s\n' "$registered" | grep -Fqx "$relative"; then
        continue
    fi

    echo "MISSING REGISTRATION: Tests/$relative" >&2
    missing=$((missing + 1))
done < "$inventory"

if [ "$missing" -eq 0 ]; then
    echo "check-test-registration: OK — all recursive Tests/Test*.cpp and Test*.mm sources are registered"
    exit 0
fi

echo "check-test-registration: $missing unregistered test file(s) found —" >&2
echo "  add them to Tests/CMakeLists.txt or mark with '// test-registration: ignore'" >&2
exit 1
