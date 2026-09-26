#!/usr/bin/env bash
# Controlled-failure tests for check-format-changed.sh (CI-100).
#
# Each case builds a throwaway git repository with every format root, commits a
# baseline, applies one change, and runs the production check-format script
# exactly as the check-format job does (repository root as cwd, base SHA in
# FORMAT_BASE_SHA). Failure paths must exit nonzero; clean, Metal-only, and
# out-of-diff legacy-debt changes must exit 0.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
CHECKER="$SCRIPT_DIR/check-format-changed.sh"
STYLE_FILE="$REPO_ROOT/.clang-format"
TMP_ROOT="$(mktemp -d)"
if [[ "${KEEP_TMP:-0}" == "1" ]]; then
    echo "check-format test scratch: $TMP_ROOT"
else
    trap 'rm -rf "$TMP_ROOT"' EXIT
fi

# The checker must never see the live workflow's base SHA.
unset FORMAT_BASE_SHA

FORMAT_ROOTS=(SparkEngine/Source GameModules SparkEditor/Source SparkConsole/src SparkShaderCompiler/src SparkBuild/src SparkInstaller/src SparkDaemon/src SparkServer/src SparkGateway/src SparkCooker/src SparkWorker/src SparkAutomation/src SparkLauncher/src Tests FuzzerTests)

passed=0
failed=0

pass() { echo "PASS: $1"; passed=$((passed + 1)); }
fail() { echo "FAIL: $1" >&2; failed=$((failed + 1)); }
expect_status() {
    local expected="$1" actual="$2" label="$3"
    if [[ "$actual" -eq "$expected" ]]; then pass "$label"; else fail "$label (expected exit $expected, got $actual)"; fi
}
expect_nonzero() {
    local actual="$1" label="$2"
    if [[ "$actual" -ne 0 ]]; then pass "$label"; else fail "$label (unexpected zero exit)"; fi
}
expect_contains() {
    local file="$1" text="$2" label="$3"
    if grep -Fq -- "$text" "$file"; then pass "$label"; else fail "$label (missing '$text')"; fi
}

# The gate is only meaningful against the real formatter. The ubuntu-24.04
# runner ships clang-format 18 (as clang-format or clang-format-18); a host
# without either fails this suite rather than silently skipping it.
REAL_CLANG_FORMAT="$(command -v clang-format || command -v clang-format-18 || true)"
if [[ -z "$REAL_CLANG_FORMAT" ]]; then
    echo "FAIL: clang-format (or clang-format-18) is required to test the check-format gate" >&2
    exit 1
fi
REAL_BIN="$TMP_ROOT/real-bin"
mkdir -p "$REAL_BIN"
ln -s "$REAL_CLANG_FORMAT" "$REAL_BIN/clang-format"
echo "Using $("$REAL_CLANG_FORMAT" --version)"

# A clang-format that accepts the style file but fails every format request
# without printing a violation, as a crashing or mis-installed binary would.
BROKEN_BIN="$TMP_ROOT/broken-bin"
mkdir -p "$BROKEN_BIN"
cat > "$BROKEN_BIN/clang-format" <<'FAKE'
#!/usr/bin/env bash
if [[ "$*" == *--dump-config* ]]; then
    exit 0
fi
echo "clang-format: simulated internal failure" >&2
exit 3
FAKE
chmod +x "$BROKEN_BIN/clang-format"

# A PATH with git and the shell utilities the checker needs, but no clang-format.
NO_FORMAT_BIN="$TMP_ROOT/no-format-bin"
mkdir -p "$NO_FORMAT_BIN"
for tool in git bash cat grep mktemp rm; do
    ln -s "$(command -v "$tool")" "$NO_FORMAT_BIN/$tool"
done

CLEAN_SOURCE=$'int Add(int left, int right)\n{\n    return left + right;\n}\n'
MISFORMATTED_SOURCE=$'int  Add( int left,int right ){return left+right;}\n'

git_in() {
    local repo="$1"
    shift
    git -C "$repo" -c user.name="check-format test" -c user.email="check-format@test.invalid" \
        -c commit.gpgsign=false -c core.hooksPath=/dev/null "$@"
}

# new_repo NAME -> prints the repository path; baseline commit has every
# format root, the real .clang-format, and one clean source.
new_repo() {
    local repo="$TMP_ROOT/$1"
    mkdir -p "$repo"
    git -C "$repo" init -q
    for root in "${FORMAT_ROOTS[@]}"; do
        mkdir -p "$repo/$root"
        touch "$repo/$root/.gitkeep"
    done
    cp "$STYLE_FILE" "$repo/.clang-format"
    printf '%s' "$CLEAN_SOURCE" > "$repo/SparkEngine/Source/Baseline.cpp"
    git_in "$repo" add -A
    git_in "$repo" commit -q -m baseline
    echo "$repo"
}

commit_all() {
    git_in "$1" add -A
    git_in "$1" commit -q -m "$2"
}

# run_checker REPO BIN_DIR [BASE_SHA] -> sets STATUS and OUTPUT (log file path)
run_checker() {
    local repo="$1" bin_dir="$2" base="${3-}"
    OUTPUT="$repo.out"
    set +e
    (
        cd "$repo"
        if [[ "$bin_dir" == "$NO_FORMAT_BIN" ]]; then
            export PATH="$NO_FORMAT_BIN"
        else
            export PATH="$bin_dir:$PATH"
        fi
        FORMAT_BASE_SHA="$base" bash "$CHECKER"
    ) > "$OUTPUT" 2>&1
    STATUS=$?
    set -e
}

head_sha() { git -C "$1" rev-parse HEAD; }

# --- Clean change passes -----------------------------------------------------
repo=$(new_repo clean)
base=$(head_sha "$repo")
printf '%s' "$CLEAN_SOURCE" > "$repo/Tests/CleanChange.cpp"
commit_all "$repo" "clean change"
run_checker "$repo" "$REAL_BIN" "$base"
expect_status 0 "$STATUS" "clean C++ change exits 0"
expect_contains "$OUTPUT" "Checked formatting of 1 changed C++ source(s)." "clean change checks exactly the changed source"

# --- Misformatted source fails -----------------------------------------------
repo=$(new_repo misformatted)
base=$(head_sha "$repo")
printf '%s' "$MISFORMATTED_SOURCE" > "$repo/GameModules/Misformatted.cpp"
commit_all "$repo" "misformatted change"
run_checker "$repo" "$REAL_BIN" "$base"
expect_nonzero "$STATUS" "misformatted .cpp exits nonzero"
expect_contains "$OUTPUT" "-Wclang-format-violations" "misformatted .cpp reports clang-format violations"
expect_contains "$OUTPUT" "::error::Code formatting violations found." "misformatted .cpp emits the violation annotation"
expect_contains "$repo/check-format-output.log" "GameModules/Misformatted.cpp" "violation log names the file for the error-summary step"

# Every routed suffix is checked, not only .cpp.
for suffix in h hpp inl; do
    repo=$(new_repo "misformatted-$suffix")
    base=$(head_sha "$repo")
    printf '%s' "$MISFORMATTED_SOURCE" > "$repo/SparkEditor/Source/Misformatted.$suffix"
    commit_all "$repo" "misformatted .$suffix"
    run_checker "$repo" "$REAL_BIN" "$base"
    expect_nonzero "$STATUS" "misformatted .$suffix exits nonzero"
done

# --- Metal-only change is out of scope ---------------------------------------
repo=$(new_repo metal)
base=$(head_sha "$repo")
mkdir -p "$repo/SparkEngine/Source/Graphics/RHI/Metal"
printf '%s' "$MISFORMATTED_SOURCE" > "$repo/SparkEngine/Source/Graphics/RHI/Metal/MetalDevice.cpp"
commit_all "$repo" "metal-only change"
run_checker "$repo" "$REAL_BIN" "$base"
expect_status 0 "$STATUS" "Metal-only change exits 0"
expect_contains "$OUTPUT" "No changed C++ sources require formatting." "Metal-only change selects no sources"

# --- Unparsable .clang-format fails ------------------------------------------
repo=$(new_repo bad-style)
base=$(head_sha "$repo")
printf 'BasedOnStyle: [unterminated\n' > "$repo/.clang-format"
commit_all "$repo" "break style file"
run_checker "$repo" "$REAL_BIN" "$base"
expect_nonzero "$STATUS" "unparsable .clang-format exits nonzero"
expect_contains "$OUTPUT" "::error::.clang-format could not be parsed." "unparsable .clang-format emits the parse annotation"

# An unknown key is also a parse failure, even with no source change.
repo=$(new_repo unknown-style-key)
base=$(head_sha "$repo")
printf 'BasedOnStyle: LLVM\nNotARealClangFormatKey: true\n' > "$repo/.clang-format"
commit_all "$repo" "unknown style key"
run_checker "$repo" "$REAL_BIN" "$base"
expect_nonzero "$STATUS" "unknown .clang-format key exits nonzero"

# A missing style file fails.
repo=$(new_repo no-style)
base=$(head_sha "$repo")
git_in "$repo" rm -q .clang-format
commit_all "$repo" "delete style file"
run_checker "$repo" "$REAL_BIN" "$base"
expect_nonzero "$STATUS" "missing .clang-format exits nonzero"
expect_contains "$OUTPUT" "::error::.clang-format is missing." "missing .clang-format emits the annotation"

# --- Missing format root fails -----------------------------------------------
repo=$(new_repo missing-root)
base=$(head_sha "$repo")
git_in "$repo" rm -q -r SparkLauncher/src
commit_all "$repo" "remove a format root"
run_checker "$repo" "$REAL_BIN" "$base"
expect_nonzero "$STATUS" "missing format root exits nonzero"
expect_contains "$OUTPUT" "::error::Formatting source root is missing: SparkLauncher/src" "missing root is named"

# --- clang-format that errors fails ------------------------------------------
repo=$(new_repo broken-binary)
base=$(head_sha "$repo")
printf '%s' "$CLEAN_SOURCE" > "$repo/SparkServer/src/Server.cpp"
commit_all "$repo" "clean change"
run_checker "$repo" "$BROKEN_BIN" "$base"
expect_status 3 "$STATUS" "erroring clang-format propagates its exit status"
expect_contains "$OUTPUT" "::error::clang-format failed to parse or format the changed sources (exit 3)." "erroring clang-format emits the failure annotation"

# A missing clang-format fails.
run_checker "$repo" "$NO_FORMAT_BIN" "$base"
expect_nonzero "$STATUS" "missing clang-format exits nonzero"
expect_contains "$OUTPUT" "::error::clang-format is not installed." "missing clang-format emits the annotation"

# --- Base-SHA selection ------------------------------------------------------
# Legacy debt outside the diff does not fail an unrelated clean change.
repo=$(new_repo legacy-debt)
printf '%s' "$MISFORMATTED_SOURCE" > "$repo/Tests/LegacyDebt.cpp"
commit_all "$repo" "legacy debt"
base=$(head_sha "$repo")
printf '%s' "$CLEAN_SOURCE" > "$repo/Tests/NewWork.cpp"
commit_all "$repo" "clean change"
run_checker "$repo" "$REAL_BIN" "$base"
expect_status 0 "$STATUS" "legacy debt outside the diff does not fail a clean change"

# Deleting a source is a change, but there is nothing left to format.
repo=$(new_repo deletion)
base=$(head_sha "$repo")
git_in "$repo" rm -q SparkEngine/Source/Baseline.cpp
commit_all "$repo" "delete source"
run_checker "$repo" "$REAL_BIN" "$base"
expect_status 0 "$STATUS" "deleted source exits 0"

# Missing, malformed, all-zero (new branch push), and unknown base SHAs fall
# back to HEAD^, so a misformatted file in HEAD still fails.
repo=$(new_repo fallback)
printf '%s' "$CLEAN_SOURCE" > "$repo/Tests/Earlier.cpp"
commit_all "$repo" "earlier clean work"
printf '%s' "$MISFORMATTED_SOURCE" > "$repo/Tests/HeadDebt.cpp"
commit_all "$repo" "misformatted head"
for bad_base in "" "not-a-sha" "0000000000000000000000000000000000000000" \
    "0123456789abcdef0123456789abcdef01234567"; do
    run_checker "$repo" "$REAL_BIN" "$bad_base"
    expect_nonzero "$STATUS" "base '${bad_base:-<unset>}' falls back to HEAD^ and fails HEAD's violation"
    expect_contains "$repo/check-format-output.log" "Tests/HeadDebt.cpp" "base '${bad_base:-<unset>}' fallback checks HEAD's file"
done
# The fallback diff is HEAD^..HEAD only: debt fixed in HEAD passes.
printf '%s' "$CLEAN_SOURCE" > "$repo/Tests/HeadDebt.cpp"
commit_all "$repo" "fix head debt"
run_checker "$repo" "$REAL_BIN" "0000000000000000000000000000000000000000"
expect_status 0 "$STATUS" "all-zero base falls back to HEAD^ only"

# A root commit has no HEAD^, so every tracked in-scope file is checked
# (plain `git rev-parse HEAD^` would echo "HEAD^" and select nothing).
repo="$TMP_ROOT/root-commit"
mkdir -p "$repo"
git -C "$repo" init -q
for root in "${FORMAT_ROOTS[@]}"; do
    mkdir -p "$repo/$root"
    touch "$repo/$root/.gitkeep"
done
cp "$STYLE_FILE" "$repo/.clang-format"
printf '%s' "$MISFORMATTED_SOURCE" > "$repo/SparkWorker/src/Worker.cpp"
commit_all "$repo" "root commit"
run_checker "$repo" "$REAL_BIN" ""
expect_nonzero "$STATUS" "root commit checks every tracked source"
expect_contains "$repo/check-format-output.log" "SparkWorker/src/Worker.cpp" "root commit reports the tracked violation"

# A diff that git cannot compute (here: the base tree's objects are gone) fails
# the gate instead of reading as "no changed sources".
repo=$(new_repo unreadable-diff)
base=$(head_sha "$repo")
printf '%s' "$CLEAN_SOURCE" > "$repo/Tests/Change.cpp"
commit_all "$repo" "clean change"
base_tree_object="$repo/.git/objects/$(git -C "$repo" rev-parse "$base^{tree}" | cut -c1-2)"
rm -rf "$base_tree_object"
run_checker "$repo" "$REAL_BIN" "$base"
expect_nonzero "$STATUS" "unreadable base diff exits nonzero"
expect_contains "$OUTPUT" "::error::Could not list sources changed since $base." "unreadable base diff emits the annotation"

echo
echo "check-format-changed tests: $passed passed, $failed failed"
if (( failed > 0 )); then
    exit 1
fi
