#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
RUNNER="$SCRIPT_DIR/run-sanitizer-tests.sh"
VERIFIER="$SCRIPT_DIR/verify-sanitizer-evidence.py"
EXTRACTOR="$SCRIPT_DIR/extract-errors.sh"
SUMMARIZER="$SCRIPT_DIR/summarize-test-results.py"
WORKFLOW="$REPO_ROOT/.github/workflows/build.yml"
TMP_ROOT="$(mktemp -d)"
if [[ "${KEEP_TMP:-0}" == "1" ]]; then
    echo "Sanitizer test scratch: $TMP_ROOT"
else
    trap 'rm -rf "$TMP_ROOT"' EXIT
fi

TEST_PYTHON="python3"
if ! "$TEST_PYTHON" -c 'import sys' >/dev/null 2>&1; then
    TEST_PYTHON="python"
fi
export TEST_PYTHON

SHA="0123456789abcdef0123456789abcdef01234567"
passed=0
failed=0
skipped=0
counter=1000

pass() { echo "PASS: $1"; passed=$((passed + 1)); }
fail() { echo "FAIL: $1" >&2; failed=$((failed + 1)); }
skip() { echo "SKIP: $1"; skipped=$((skipped + 1)); }
expect_status() {
    local expected="$1" actual="$2" label="$3"
    [[ "$actual" -eq "$expected" ]] && pass "$label" || fail "$label (expected $expected, got $actual)"
}
expect_nonzero() {
    local actual="$1" label="$2"
    [[ "$actual" -ne 0 ]] && pass "$label" || fail "$label (unexpected zero exit)"
}
expect_contains() {
    local file="$1" text="$2" label="$3"
    grep -Fq "$text" "$file" && pass "$label" || fail "$label"
}

FAKE_TEST="$TMP_ROOT/fake-spark-tests.sh"
cat > "$FAKE_TEST" <<'FAKE'
#!/usr/bin/env bash
set -euo pipefail

report=""
junit=""
while [[ "$#" -gt 0 ]]; do
    case "$1" in
        --output-file) report="$2"; shift 2 ;;
        --junit-xml) junit="$2"; shift 2 ;;
        *) shift ;;
    esac
done
[[ -n "$report" && -n "$junit" ]]

runtime_prefix() {
    local name value
    for name in ASAN_OPTIONS TSAN_OPTIONS MSAN_OPTIONS; do
        value="${!name-}"
        if [[ ":$value:" == *":log_path="* ]]; then
            value="${value##*log_path=}"
            printf '%s' "${value%%:*}"
            return 0
        fi
    done
    return 1
}

write_clean() {
    local extra="${1:-}"
    local output
    output="=== SparkEngine Test Suite ===
Shuffle seed: 123
Running 2 tests...
[   OK   ] Contract_One
[   OK   ] Contract_Two
${extra}
=== Results ===
Tests:      2 passed, 0 failed, 2 total
Assertions: 2 passed, 0 failed
Duration:   1ms"
    printf '%s\n' "$output"
    printf '%s\n' "$output" > "$report"
    cat > "$junit" <<'XML'
<?xml version="1.0" encoding="UTF-8"?>
<testsuites tests="2" failures="0" skipped="0" time="0.001">
  <testsuite name="SparkEngine" tests="2" failures="0" skipped="0" time="0.001">
    <testcase name="Contract_One" time="0.0005"/>
    <testcase name="Contract_Two" time="0.0005"/>
  </testsuite>
</testsuites>
XML
}

write_hostile_encoded_junit() {
    local kind="$1"
    "$TEST_PYTHON" - "$junit" "$kind" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
kind = sys.argv[2]
if kind == "markup":
    extra = "<!--hidden comment-->"
    xml = (
        '<?xml version="1.0" encoding="UTF-16"?>'
        '<testsuites tests="2" failures="0" errors="0" skipped="0">'
        '<testsuite name="SparkEngine" tests="2" failures="0" errors="0" skipped="0">'
        f'{extra}<testcase name="Contract_One"/><testcase name="Contract_Two"/>'
        '</testsuite></testsuites>'
    )
    path.write_bytes(xml.encode("utf-16"))
elif kind == "entity":
    xml = (
        '<?xml version="1.0" encoding="UTF-16LE"?>'
        '<!DOCTYPE testsuites [<!ENTITY contract "Contract_One">]>'
        '<testsuites tests="2" failures="0" errors="0" skipped="0">'
        '<testsuite name="SparkEngine" tests="2" failures="0" errors="0" skipped="0">'
        '<testcase name="&contract;"/><testcase name="Contract_Two"/>'
        '</testsuite></testsuites>'
    )
    # The explicit byte order satisfies the verifier's BOM-less encoding
    # contract, so this reaches the DTD/entity guard instead of failing early.
    # Its interleaved NULs also demonstrate why an ASCII byte scan is insufficient.
    path.write_bytes(xml.encode("utf-16-le"))
else:
    raise AssertionError(kind)
PY
}

case "${FAKE_MODE:-clean}" in
    clean) write_clean ;;
    empty) exit 0 ;;
    zero)
        output="=== SparkEngine Test Suite ===
Shuffle seed: 123
Running 0 tests...

=== Results ===
Tests:      0 passed, 0 failed, 0 total
Assertions: 0 passed, 0 failed
Duration:   0ms"
        printf '%s\n' "$output"
        printf '%s\n' "$output" > "$report"
        cat > "$junit" <<'XML'
<testsuites tests="0" failures="0" skipped="0"><testsuite name="SparkEngine" tests="0" failures="0" skipped="0"/></testsuites>
XML
        ;;
    warning)
        output="=== SparkEngine Test Suite ===
Shuffle seed: 123
Running 2 tests...
[   OK   ] Contract_One
[ WARN   ] Contract_Flaky
Known flaky: synthetic warning
=== Results ===
Tests:      1 passed, 0 failed, 1 warned, 2 total
Assertions: 1 passed, 0 failed
Duration:   1ms"
        printf '%s\n' "$output"
        printf '%s\n' "$output" > "$report"
        cat > "$junit" <<'XML'
<testsuites tests="2" failures="0" skipped="1"><testsuite name="SparkEngine" tests="2" failures="0" skipped="1"><testcase name="Contract_One"/><testcase name="Contract_Flaky"><skipped message="Known flaky: synthetic warning"/></testcase></testsuite></testsuites>
XML
        ;;
    failure)
        output="=== SparkEngine Test Suite ===
Shuffle seed: 123
Running 2 tests...
[   OK   ] Contract_One
[ FAILED ] Contract_Two
=== Results ===
Tests:      1 passed, 1 failed, 2 total
Assertions: 1 passed, 1 failed
Duration:   1ms"
        printf '%s\n' "$output"
        printf '%s\n' "$output" > "$report"
        cat > "$junit" <<'XML'
<testsuites tests="2" failures="1" skipped="0"><testsuite name="SparkEngine" tests="2" failures="1" skipped="0"><testcase name="Contract_One"/><testcase name="Contract_Two"><failure message="synthetic"/></testcase></testsuite></testsuites>
XML
        exit 23
        ;;
    nonzero-clean)
        write_clean
        exit 17
        ;;
    exit-124)
        write_clean
        exit 124
        ;;
    crash)
        printf 'Segmentation fault (core dumped)\n'
        exit 139
        ;;
    timeout)
        trap 'exit 143' TERM
        while :; do sleep 0.1; done
        ;;
    signature)
        write_clean "ERROR: AddressSanitizer: heap-buffer-overflow"
        ;;
    report-signature)
        write_clean
        printf 'ERROR: AddressSanitizer: report-only heap-buffer-overflow\n' >> "$report"
        ;;
    duplicate-seed)
        write_clean "Shuffle seed: 999"
        ;;
    retry-evidence)
        write_clean "Retries: 1"
        ;;
    terminal-count-spoof)
        output="=== SparkEngine Test Suite ===
Shuffle seed: 123
Running 2 tests...
[   OK   ] Contract_One
[   OK   ] Contract_Two
=== Results ===
Tests:      3 passed, 0 failed, 2 total
Assertions: 2 passed, 0 failed
Duration:   1ms"
        printf '%s\n' "$output"
        printf '%s\n' "$output" > "$report"
        cat > "$junit" <<'XML'
<testsuites tests="2" failures="0" errors="0" skipped="0"><testsuite name="SparkEngine" tests="2" failures="0" errors="0" skipped="0"><testcase name="Contract_One"/><testcase name="Contract_Two"/></testsuite></testsuites>
XML
        ;;
    capture-overflow)
        write_clean
        "$TEST_PYTHON" -c 'import sys; sys.stdout.buffer.write(b"x" * (17 * 1024 * 1024))'
        ;;
    report-overflow)
        write_clean
        "$TEST_PYTHON" -c 'import sys; open(sys.argv[1], "ab").write(b"x" * (17 * 1024 * 1024))' "$report"
        ;;
    arbitrary-entry)
        write_clean
        printf 'untrusted\n' > "$(dirname "$report")/sanitizer.999"
        ;;
    late-entry)
        write_clean
        evidence_parent="$(dirname "$report")"
        (sleep 0.02; printf 'late\n' > "$evidence_parent/late-evidence") >/dev/null 2>&1 &
        ;;
    runtime)
        write_clean
        prefix="$(runtime_prefix)"
        printf 'ERROR: AddressSanitizer: heap-buffer-overflow\n' > "${prefix}.$$"
        ;;
    runtime-tsan)
        write_clean
        prefix="$(runtime_prefix)"
        printf 'WARNING: ThreadSanitizer: data race\n' > "${prefix}.$$"
        ;;
    empty-runtime)
        write_clean
        prefix="$(runtime_prefix)"
        : > "${prefix}.$$"
        ;;
    bad-runtime)
        write_clean
        prefix="$(runtime_prefix)"
        printf 'not sanitizer evidence\n' > "${prefix}.$$"
        ;;
    arbitrary-runtime-name)
        write_clean
        prefix="$(runtime_prefix)"
        printf 'ERROR: AddressSanitizer: synthetic\n' > "$(dirname "$prefix")/untrusted.999"
        ;;
    symlink-runtime)
        write_clean
        prefix="$(runtime_prefix)"
        printf 'ERROR: AddressSanitizer: heap-buffer-overflow\n' > "${prefix}.target"
        ln -s "${prefix}.target" "${prefix}.$$"
        ;;
    stale-runtime)
        write_clean
        prefix="$(runtime_prefix)"
        printf 'ERROR: AddressSanitizer: heap-buffer-overflow\n' > "${prefix}.$$"
        touch -t 200001010000 "${prefix}.$$"
        ;;
    oversized-runtime)
        write_clean
        prefix="$(runtime_prefix)"
        "$TEST_PYTHON" -c 'import sys; open(sys.argv[1], "wb").write(b"ERROR: AddressSanitizer: x\n" + b"x" * (5 * 1024 * 1024))' "${prefix}.$$"
        ;;
    many-runtime)
        write_clean
        prefix="$(runtime_prefix)"
        for index in $(seq 1 33); do
            printf 'ERROR: AddressSanitizer: synthetic\n' > "${prefix}.$((10000 + index))"
        done
        ;;
    stale-junit)
        write_clean
        touch -t 200001010000 "$junit"
        ;;
    malformed-junit)
        write_clean
        printf '<not-valid-xml' > "$junit"
        ;;
    utf16-markup-junit)
        write_clean
        write_hostile_encoded_junit markup
        ;;
    utf16-entity-junit)
        write_clean
        write_hostile_encoded_junit entity
        ;;
    junit-errors)
        write_clean
        cat > "$junit" <<'XML'
<testsuites tests="2" failures="0" errors="1" skipped="0"><testsuite name="SparkEngine" tests="2" failures="0" errors="1" skipped="0"><testcase name="Contract_One"/><testcase name="Contract_Two"/></testsuite></testsuites>
XML
        ;;
    duplicate-junit-identity)
        write_clean
        cat > "$junit" <<'XML'
<testsuites tests="2" failures="0" errors="0" skipped="0"><testsuite name="SparkEngine" tests="2" failures="0" errors="0" skipped="0"><testcase name="Contract_One"/><testcase name="Contract_One"/></testsuite></testsuites>
XML
        ;;
    empty-junit-identity)
        write_clean
        cat > "$junit" <<'XML'
<testsuites tests="2" failures="0" errors="0" skipped="0"><testsuite name="SparkEngine" tests="2" failures="0" errors="0" skipped="0"><testcase name=""/><testcase name="Contract_Two"/></testsuite></testsuites>
XML
        ;;
    ambiguous-junit-outcome)
        write_clean
        cat > "$junit" <<'XML'
<testsuites tests="2" failures="1" errors="1" skipped="0"><testsuite name="SparkEngine" tests="2" failures="1" errors="1" skipped="0"><testcase name="Contract_One"><failure/><error/></testcase><testcase name="Contract_Two"/></testsuite></testsuites>
XML
        ;;
    deep-junit)
        write_clean
        cat > "$junit" <<'XML'
<testsuites tests="2" failures="0" errors="0" skipped="0"><testsuite name="SparkEngine" tests="2" failures="0" errors="0" skipped="0"><testcase name="Contract_One"><skipped><nested/></skipped></testcase><testcase name="Contract_Two"/></testsuite></testsuites>
XML
        ;;
    too-many-junit)
        write_clean
        "$TEST_PYTHON" - "$junit" <<'PY'
import sys
with open(sys.argv[1], "w", encoding="utf-8") as stream:
    stream.write('<testsuites tests="10001" failures="0" errors="0" skipped="0"><testsuite name="SparkEngine" tests="10001" failures="0" errors="0" skipped="0">')
    for index in range(10001):
        stream.write(f'<testcase name="Case_{index}"/>')
    stream.write('</testsuite></testsuites>')
PY
        ;;
    self-term)
        printf 'signal interruption\n'
        kill -TERM "$$"
        ;;
    *) echo "unknown FAKE_MODE" >&2; exit 99 ;;
esac
FAKE
chmod +x "$FAKE_TEST"

SYMLINK_SUPPORTED=0
printf 'probe\n' > "$TMP_ROOT/symlink-probe-target"
if ln -s "$TMP_ROOT/symlink-probe-target" "$TMP_ROOT/symlink-probe-link" 2>/dev/null \
    && [[ -L "$TMP_ROOT/symlink-probe-link" ]]; then
    SYMLINK_SUPPORTED=1
fi
rm -f "$TMP_ROOT/symlink-probe-link" "$TMP_ROOT/symlink-probe-target"

# Exercise the shared file primitive directly so every evidence role inherits
# the same hard-link and same-size/restored-mtime race guarantees.
set +e
"$TEST_PYTHON" - "$VERIFIER" "$TMP_ROOT" <<'PY'
import importlib.util
import os
import pathlib
import sys

verifier_path = pathlib.Path(sys.argv[1])
root = pathlib.Path(sys.argv[2]) / "identity-contract"
root.mkdir()
spec = importlib.util.spec_from_file_location("sanitizer_evidence_verifier", verifier_path)
assert spec is not None and spec.loader is not None
verifier = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = verifier
spec.loader.exec_module(verifier)

roles = (
    "console.txt",
    "report.txt",
    "junit.xml",
    "metadata.json",
    "process-footer.txt",
    "test-stats-linux-asan.json",
    ".wrapper-timeout",
    ".capture-overflow",
    "sanitizer.123",
)

for index, name in enumerate(roles):
    path = root / f"{index}-{name}"
    original = (f"original-{name}-".encode("utf-8") * 16)[:256]
    mutated = bytes(byte ^ 1 for byte in original)
    assert len(mutated) == len(original)
    path.write_bytes(original)
    before = os.stat(path, follow_symlinks=False)
    sleep_calls = [0]

    def mutate_during_quiescence(_seconds):
        sleep_calls[0] += 1
        if sleep_calls[0] == 1:
            with path.open("r+b") as stream:
                stream.write(mutated)
                stream.flush()
                os.fsync(stream.fileno())
            os.utime(
                path,
                ns=(before.st_atime_ns, before.st_mtime_ns),
            )

    real_sleep = verifier.time.sleep
    verifier.time.sleep = mutate_during_quiescence
    try:
        errors = []
        observation = verifier.read_regular_file(
            path,
            evidence_dir=root.resolve(),
            maximum=4096,
            started_ns=0,
            errors=errors,
        )
    finally:
        verifier.time.sleep = real_sleep
    assert observation is None, (name, errors)
    assert any("changed" in error for error in errors), (name, errors)
    path.unlink()

# A clean read is only a snapshot.  Reconsume every role after semantic use
# and require both the original object identity and the original bytes.
for index, name in enumerate(roles):
    path = root / f"stable-{index}-{name}"
    original = (f"stable-original-{name}-".encode("utf-8") * 16)[:256]
    mutated = bytes(byte ^ 1 for byte in original)
    path.write_bytes(original)
    errors = []
    observation = verifier.read_regular_file(
        path,
        evidence_dir=root.resolve(),
        maximum=4096,
        started_ns=0,
        errors=errors,
    )
    assert observation is not None and not errors, (name, errors)
    before = os.stat(path, follow_symlinks=False)
    path.write_bytes(mutated)
    os.utime(path, ns=(before.st_atime_ns, before.st_mtime_ns))
    verifier.verify_stable_file(
        path,
        observation,
        evidence_dir=root.resolve(),
        maximum=4096,
        started_ns=0,
        errors=errors,
    )
    assert any("changed after it was consumed" in error for error in errors), (name, errors)
    path.unlink()

# Replacing the pathname with a same-size object and restored mtime must not
# inherit the identity of bytes that were already validated.
target = root / "replacement-target"
replacement = root / "replacement-candidate"
target.write_bytes(b"A" * 256)
errors = []
observation = verifier.read_regular_file(
    target,
    evidence_dir=root.resolve(),
    maximum=4096,
    started_ns=0,
    errors=errors,
)
assert observation is not None and not errors, errors
before = os.stat(target, follow_symlinks=False)
replacement.write_bytes(b"B" * 256)
os.utime(replacement, ns=(before.st_atime_ns, before.st_mtime_ns))
os.replace(replacement, target)
verifier.verify_stable_file(
    target,
    observation,
    evidence_dir=root.resolve(),
    maximum=4096,
    started_ns=0,
    errors=errors,
)
assert any("changed after it was consumed" in error for error in errors), errors
target.unlink()

target = root / "hardlink-target"
outside = root.parent / "hardlink-outside"
target.write_bytes(b"hard-link evidence")
try:
    os.link(target, outside)
except OSError as exc:
    print(f"hard-link probe unavailable: {exc}")
    raise SystemExit(77)
try:
    assert os.stat(target, follow_symlinks=False).st_nlink >= 2
    errors = []
    assert verifier.read_regular_file(
        target,
        evidence_dir=root.resolve(),
        maximum=4096,
        started_ns=0,
        errors=errors,
    ) is None
    assert any("exactly one hard link" in error for error in errors), errors
finally:
    outside.unlink(missing_ok=True)
    target.unlink(missing_ok=True)

target = root / "post-consume-hardlink-target"
outside = root.parent / "post-consume-hardlink-outside"
target.write_bytes(b"post-consume hard-link evidence")
errors = []
observation = verifier.read_regular_file(
    target,
    evidence_dir=root.resolve(),
    maximum=4096,
    started_ns=0,
    errors=errors,
)
assert observation is not None and not errors, errors
os.link(target, outside)
try:
    verifier.verify_stable_file(
        target,
        observation,
        evidence_dir=root.resolve(),
        maximum=4096,
        started_ns=0,
        errors=errors,
    )
    assert any("exactly one hard link" in error for error in errors), errors
finally:
    outside.unlink(missing_ok=True)
    target.unlink(missing_ok=True)

target = root / "snapshot-target"
outside = root.parent / "snapshot-outside"
target.write_bytes(b"directory snapshot evidence")
errors = []
before_snapshot = verifier.snapshot_directory(
    root, maximum_entries=32, label="identity test directory", errors=errors
)
assert before_snapshot is not None and not errors, errors
os.link(target, outside)
try:
    after_snapshot = verifier.snapshot_directory(
        root, maximum_entries=32, label="identity test directory", errors=errors
    )
    assert after_snapshot is not None
    assert after_snapshot != before_snapshot
    assert any("exactly one hard link" in error for error in errors), errors
finally:
    outside.unlink(missing_ok=True)
    target.unlink(missing_ok=True)
PY
identity_status=$?
set -e
if [[ "$identity_status" -eq 0 ]]; then
    pass "all evidence roles bind object identity and reject hard-link or restored-mtime mutation"
elif [[ "$identity_status" -eq 77 ]]; then
    pass "all evidence roles reject same-size restored-mtime mutation"
    skip "hard-link identity probe unavailable on this filesystem"
else
    fail "evidence file identity regression contract"
fi

# Decode the XML entity before applying forbidden-markup policy.  Cover both
# UTF-16 byte orders and prove a DTD/entity payload never reaches ElementTree.
set +e
"$TEST_PYTHON" - "$VERIFIER" <<'PY'
import importlib.util
import pathlib
import sys

verifier_path = pathlib.Path(sys.argv[1])
spec = importlib.util.spec_from_file_location("sanitizer_evidence_xml_verifier", verifier_path)
assert spec is not None and spec.loader is not None
verifier = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = verifier
spec.loader.exec_module(verifier)


def encode(text, encoding):
    if encoding == "utf-8":
        return text.encode("utf-8")
    if encoding == "utf-16le-bom":
        return b"\xff\xfe" + text.encode("utf-16-le")
    if encoding == "utf-16be-bom":
        return b"\xfe\xff" + text.encode("utf-16-be")
    return text.encode(encoding)


def document(declaration, extra=""):
    return (
        declaration
        + '<testsuites tests="2" failures="0" errors="0" skipped="0">'
        + '<testsuite name="SparkEngine" tests="2" failures="0" errors="0" skipped="0">'
        + extra
        + '<testcase name="Contract_One"/><testcase name="Contract_Two"/>'
        + '</testsuite></testsuites>'
    )


benign = (
    encode(document('<?xml version="1.0" encoding="UTF-8"?>'), "utf-8"),
    encode(document('<?xml version="1.0" encoding="UTF-16"?>'), "utf-16le-bom"),
    encode(document('<?xml version="1.0" encoding="UTF-16"?>'), "utf-16be-bom"),
    encode(document('<?xml version="1.0" encoding="UTF-16LE"?>'), "utf-16-le"),
    encode(document('<?xml version="1.0" encoding="UTF-16BE"?>'), "utf-16-be"),
)
for data in benign:
    errors = []
    parsed = verifier.parse_junit(data, minimum_tests=2, errors=errors)
    assert parsed.get("tests") == 2 and not errors, errors

hostile = (
    (
        encode(
            document('<?xml version="1.0" encoding="UTF-16"?>', '<!--hidden-->'),
            "utf-16le-bom",
        ),
        "comments are outside",
    ),
    (
        encode(
            document('<?xml version="1.0" encoding="UTF-16"?>', '<?hidden value?>'),
            "utf-16be-bom",
        ),
        "processing instructions are outside",
    ),
    (
        encode(
            document('<?xml version="1.0" encoding="UTF-16LE"?>', '<![CDATA[hidden]]>'),
            "utf-16-le",
        ),
        "CDATA is outside",
    ),
)
for data, expected in hostile:
    errors = []
    assert verifier.parse_junit(data, minimum_tests=2, errors=errors) == {}
    assert any(expected in error for error in errors), errors

entity_xml = (
    '<?xml version="1.0" encoding="UTF-16LE"?>'
    '<!DOCTYPE testsuites [<!ENTITY contract "Contract_One">]>'
    '<testsuites tests="2" failures="0" errors="0" skipped="0">'
    '<testsuite name="SparkEngine" tests="2" failures="0" errors="0" skipped="0">'
    '<testcase name="&contract;"/><testcase name="Contract_Two"/>'
    '</testsuite></testsuites>'
)
parser_called = [False]


def forbidden_parser(*_args, **_kwargs):
    parser_called[0] = True
    raise AssertionError("hostile entity input reached ElementTree")


real_iterparse = verifier.ET.iterparse
verifier.ET.iterparse = forbidden_parser
try:
    errors = []
    assert verifier.parse_junit(
        encode(entity_xml, "utf-16-le"), minimum_tests=2, errors=errors
    ) == {}
finally:
    verifier.ET.iterparse = real_iterparse
assert not parser_called[0]
assert any("forbidden DTD declaration" in error for error in errors), errors
PY
xml_status=$?
set -e
if [[ "$xml_status" -eq 0 ]]; then
    pass "UTF-16 decoding precedes comment, DTD/entity, PI, and CDATA policy checks"
else
    fail "decoded XML security regression contract"
fi

case_dir() {
    local sanitizer="$1" run="$2"
    printf '%s/spark-sanitizer-%s-%s-%s-1-build-linux-%s' "$TMP_ROOT" "$sanitizer" "$SHA" "$run" "$sanitizer"
}

run_case() {
    local mode="$1" sanitizer="${2:-asan}" timeout_value="${3:-10}"
    counter=$((counter + 1))
    CASE_RUN="$counter"
    CASE_DIR="$(case_dir "$sanitizer" "$CASE_RUN")"
    local runtime_env
    case "$sanitizer" in
        asan) runtime_env=ASAN_OPTIONS ;;
        tsan) runtime_env=TSAN_OPTIONS ;;
        msan) runtime_env=MSAN_OPTIONS ;;
    esac
    local command=("$FAKE_TEST" --shuffle 123)
    [[ "$sanitizer" == "msan" ]] || command+=(--warn-is-error)
    set +e
    FAKE_MODE="$mode" bash "$RUNNER" \
        --evidence-root "$TMP_ROOT" \
        --sanitizer "$sanitizer" \
        --expected-sha "$SHA" \
        --run-id "$CASE_RUN" \
        --run-attempt 1 \
        --job "build-linux-$sanitizer" \
        --expected-selector all \
        --minimum-tests 1 \
        --timeout-seconds "$timeout_value" \
        --runtime-env "$runtime_env" \
        -- "${command[@]}" >/dev/null 2>&1
    CASE_STATUS=$?
    set -e
}

reject_command_contract() {
    local label="$1"
    shift
    counter=$((counter + 1))
    set +e
    FAKE_MODE=clean bash "$RUNNER" \
        --evidence-root "$TMP_ROOT" --sanitizer asan --expected-sha "$SHA" \
        --run-id "$counter" --run-attempt 1 --job build-linux-asan --expected-selector all \
        --minimum-tests 1 --timeout-seconds 10 --runtime-env ASAN_OPTIONS -- \
        "$FAKE_TEST" "$@" >/dev/null 2>&1
    local status=$?
    set -e
    expect_status 2 "$status" "$label"
}

# Clean, provenance-bound completion evidence.
run_case clean
expect_status 0 "$CASE_STATUS" "clean run exits zero"
expect_contains "$CASE_DIR/process-footer.txt" "evidence_classification=clean" "clean footer classification"
expect_contains "$CASE_DIR/process-footer.txt" "capture_exit_code=0" "PIPESTATUS capture recorded"
"$TEST_PYTHON" - "$CASE_DIR/metadata.json" "$SHA" <<'PY'
import json, sys
data = json.load(open(sys.argv[1], encoding="utf-8"))
assert data["classification"] == "clean"
assert data["completion"]["tests"] == 2
assert data["completion"]["valid"] is True
assert data["selector"] == {"expected": "all", "verified": True}
assert data["provenance"]["commitSha"] == sys.argv[2]
assert data["process"]["exitCode"] == 0
assert data["process"]["captureExitCode"] == 0
PY
pass "clean metadata binds completion, selector, and provenance"

# Exit zero is never enough without complete positive evidence.
run_case empty
expect_status 70 "$CASE_STATUS" "empty-output exit zero fails verification"
expect_contains "$CASE_DIR/process-footer.txt" "evidence_classification=verification-failure" "empty-output classification"
run_case zero
expect_status 70 "$CASE_STATUS" "zero-test exit zero fails verification"
run_case warning
expect_status 1 "$CASE_STATUS" "known-flaky warning cannot pass required lane"
expect_contains "$CASE_DIR/metadata.json" '"classification": "test-policy-failure"' "known warning classification"

# Deterministic failures, crashes, and sanitizer findings remain distinct.
run_case failure
expect_status 23 "$CASE_STATUS" "deterministic test failure preserves producer exit"
expect_contains "$CASE_DIR/metadata.json" '"classification": "test-failure"' "deterministic failure classification"
bash "$EXTRACTOR" "sanitizer-contract" "$TMP_ROOT/failure-summary.json" \
    "$CASE_DIR/console.txt" "$CASE_DIR/process-footer.txt" "$CASE_DIR/metadata.json"
expect_contains "$TMP_ROOT/failure-summary.json" "effective_exit_code=23" "error summary preserves authoritative process footer"
run_case nonzero-clean
expect_status 17 "$CASE_STATUS" "unsigned nonzero process exit is preserved"
expect_contains "$CASE_DIR/metadata.json" '"classification": "process-failure"' "unsigned nonzero is not mislabeled infrastructure"
expect_contains "$CASE_DIR/metadata.json" '"infrastructure": false' "unsigned nonzero has no invented infrastructure evidence"
run_case crash
expect_status 139 "$CASE_STATUS" "crash preserves producer exit"
expect_contains "$CASE_DIR/metadata.json" '"classification": "crash"' "crash classification remains distinct"
expect_contains "$CASE_DIR/metadata.json" '"crash": true' "crash signal recorded"
run_case signature
expect_status 1 "$CASE_STATUS" "sanitizer signature overrides exit zero"
expect_contains "$CASE_DIR/metadata.json" '"classification": "sanitizer-finding"' "console sanitizer classification"
run_case runtime
expect_status 1 "$CASE_STATUS" "parseable private ASan runtime log overrides exit zero"
expect_contains "$CASE_DIR/metadata.json" '"runtimeEvidence": true' "runtime evidence recorded"
expect_contains "$CASE_DIR/metadata.json" '"sha256":' "runtime evidence content hash is recorded"
expect_contains "$CASE_DIR/metadata.json" '"pid":' "runtime evidence filename provenance is recorded"
run_case runtime-tsan tsan
expect_status 1 "$CASE_STATUS" "parseable private TSan runtime log overrides exit zero"

# Runtime evidence is bounded, fresh, regular, and parseable.
for mode in empty-runtime bad-runtime arbitrary-runtime-name stale-runtime oversized-runtime many-runtime; do
    run_case "$mode"
    expect_status 70 "$CASE_STATUS" "$mode is a verification failure"
done
if [[ "$SYMLINK_SUPPORTED" -eq 1 ]]; then
    run_case symlink-runtime
    expect_status 70 "$CASE_STATUS" "symlink-runtime is a verification failure"
    expect_contains "$CASE_DIR/metadata.json" "non-symlink" "runtime symlink rejection is explicit"
else
    skip "runtime symlink probe unavailable on this filesystem"
fi
run_case stale-junit
expect_status 70 "$CASE_STATUS" "stale JUnit evidence is rejected"
run_case malformed-junit
expect_status 70 "$CASE_STATUS" "unparseable JUnit evidence is rejected"
for mode in utf16-markup-junit utf16-entity-junit; do
    run_case "$mode"
    expect_status 70 "$CASE_STATUS" "$mode cannot bypass the decoded XML policy"
    if [[ "$mode" == "utf16-markup-junit" ]]; then
        expect_contains "$CASE_DIR/metadata.json" "JUnit comments are outside" \
            "UTF-16 comment reaches the comment-specific rejection"
    else
        expect_contains "$CASE_DIR/metadata.json" "JUnit contains a forbidden DTD declaration" \
            "BOM-less UTF-16LE entity payload reaches the DTD/entity rejection"
    fi
done
for mode in junit-errors duplicate-junit-identity empty-junit-identity ambiguous-junit-outcome deep-junit too-many-junit; do
    run_case "$mode"
    expect_status 70 "$CASE_STATUS" "$mode strict JUnit evidence is rejected"
done
run_case duplicate-seed
expect_status 70 "$CASE_STATUS" "duplicate/conflicting runtime seed markers are rejected"
run_case retry-evidence
expect_status 70 "$CASE_STATUS" "runtime retry evidence is rejected"
run_case terminal-count-spoof
expect_status 70 "$CASE_STATUS" "terminal Results arithmetic cannot spoof JUnit evidence"
run_case report-signature
expect_status 1 "$CASE_STATUS" "report-only sanitizer signature fails the lane"
expect_contains "$CASE_DIR/metadata.json" '"classification": "sanitizer-finding"' "console and report are scanned as a union"
run_case arbitrary-entry
expect_status 70 "$CASE_STATUS" "arbitrary evidence-directory entries are rejected"
run_case late-entry
expect_status 70 "$CASE_STATUS" "late evidence-directory mutation cannot race verification"

# Fresh directory and prefix ownership are fail-closed.
counter=$((counter + 1))
stale_dir="$(case_dir asan "$counter")"
mkdir "$stale_dir"
set +e
FAKE_MODE=clean bash "$RUNNER" --evidence-root "$TMP_ROOT" --sanitizer asan \
    --expected-sha "$SHA" --run-id "$counter" --run-attempt 1 --job build-linux-asan \
    --expected-selector all --minimum-tests 1 --timeout-seconds 10 --runtime-env ASAN_OPTIONS \
    -- "$FAKE_TEST" --shuffle 123 --warn-is-error >/dev/null 2>&1
status=$?
set -e
expect_status 2 "$status" "preexisting evidence directory is rejected"

set +e
ASAN_OPTIONS=log_path=/tmp/untrusted bash "$RUNNER" --evidence-root "$TMP_ROOT" --sanitizer asan \
    --expected-sha "$SHA" --run-id 99991 --run-attempt 1 --job build-linux-asan \
    --expected-selector all --minimum-tests 1 --timeout-seconds 10 --runtime-env ASAN_OPTIONS \
    -- "$FAKE_TEST" --shuffle 123 --warn-is-error >/dev/null 2>&1
status=$?
set -e
expect_status 2 "$status" "caller-supplied arbitrary runtime prefix is rejected"

if [[ "$SYMLINK_SUPPORTED" -eq 1 ]] && ln -s "$TMP_ROOT" "$TMP_ROOT/root-link" 2>/dev/null; then
    set +e
    bash "$RUNNER" --evidence-root "$TMP_ROOT/root-link" --sanitizer asan \
        --expected-sha "$SHA" --run-id 99992 --run-attempt 1 --job build-linux-asan \
        --expected-selector all --minimum-tests 1 --timeout-seconds 10 --runtime-env ASAN_OPTIONS \
        -- "$FAKE_TEST" --shuffle 123 --warn-is-error >/dev/null 2>&1
    status=$?
    set -e
    expect_status 2 "$status" "symlink evidence root is rejected"
else
    skip "symlink evidence-root probe unavailable on this filesystem"
fi

# Scanner errors are not interpreted as 'no findings'.
mkdir "$TMP_ROOT/bad-bin"
cat > "$TMP_ROOT/bad-bin/grep" <<'BADGREP'
#!/usr/bin/env bash
exit 2
BADGREP
chmod +x "$TMP_ROOT/bad-bin/grep"
counter=$((counter + 1))
scanner_dir="$(case_dir asan "$counter")"
set +e
PATH="$TMP_ROOT/bad-bin:$PATH" FAKE_MODE=clean bash "$RUNNER" \
    --evidence-root "$TMP_ROOT" --sanitizer asan --expected-sha "$SHA" \
    --run-id "$counter" --run-attempt 1 --job build-linux-asan --expected-selector all \
    --minimum-tests 1 --timeout-seconds 10 --runtime-env ASAN_OPTIONS -- \
    "$FAKE_TEST" --shuffle 123 --warn-is-error >/dev/null 2>&1
status=$?
set -e
expect_status 70 "$status" "grep status greater than one is a verification failure"
expect_contains "$scanner_dir/metadata.json" "scanner failed with status 2" "scanner failure is recorded"

# The bounded capture drains the producer but never writes or emits more than
# 16 MiB, and its authenticated overflow is independently authoritative.
run_case capture-overflow
expect_status 74 "$CASE_STATUS" "capture overflow status is preserved independently"
expect_contains "$CASE_DIR/metadata.json" '"captureExitCode": 74' "capture overflow status is recorded"
expect_contains "$CASE_DIR/metadata.json" '"captureOverflow": true' "capture overflow marker is authenticated"
expect_contains "$CASE_DIR/metadata.json" '"classification": "capture-failure"' "capture overflow classification"
console_size="$(wc -c < "$CASE_DIR/console.txt")"
[[ "$console_size" -le 16777216 ]] && pass "console is capped while written" || fail "console write-time cap"

run_case report-overflow
expect_nonzero "$CASE_STATUS" "report overflow cannot pass"
report_size="$(wc -c < "$CASE_DIR/report.txt")"
if [[ "$(uname -s)" =~ ^(MINGW|MSYS) ]]; then
    skip "POSIX RLIMIT_FSIZE enforcement unavailable in Git Bash"
elif [[ "$report_size" -le 16777216 ]]; then
    pass "report is capped while written"
else
    fail "report write-time cap"
fi

# Provenance, required policy, selector, and timeout controls are enforced.
set +e
GITHUB_SHA=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa bash "$RUNNER" \
    --evidence-root "$TMP_ROOT" --sanitizer asan --expected-sha "$SHA" \
    --run-id 99993 --run-attempt 1 --job build-linux-asan --expected-selector all \
    --minimum-tests 1 --timeout-seconds 10 --runtime-env ASAN_OPTIONS -- \
    "$FAKE_TEST" --shuffle 123 --warn-is-error >/dev/null 2>&1
status=$?
set -e
expect_status 2 "$status" "SHA mismatch is rejected before execution"

set +e
bash "$RUNNER" --evidence-root "$TMP_ROOT" --sanitizer asan --expected-sha "$SHA" \
    --run-id 99994 --run-attempt 1 --job build-linux-asan --expected-selector all \
    --minimum-tests 1 --timeout-seconds 10 --runtime-env ASAN_OPTIONS -- \
    "$FAKE_TEST" --shuffle 123 >/dev/null 2>&1
status=$?
set -e
expect_status 2 "$status" "required ASan lane rejects missing warn-is-error"

reject_command_contract "duplicate shuffle flags are rejected" \
    --warn-is-error --shuffle 123 --shuffle 123
reject_command_contract "conflicting shuffle flags are rejected" \
    --warn-is-error --shuffle 123 --shuffle 999
reject_command_contract "retry flags are rejected" \
    --warn-is-error --shuffle 123 --retries 1
reject_command_contract "duplicate warn-is-error flags are rejected" \
    --warn-is-error --warn-is-error --shuffle 123

set +e
SPARK_TEST_NAME=hidden bash "$RUNNER" --evidence-root "$TMP_ROOT" --sanitizer asan \
    --expected-sha "$SHA" --run-id 99995 --run-attempt 1 --job build-linux-asan \
    --expected-selector all --minimum-tests 1 --timeout-seconds 10 --runtime-env ASAN_OPTIONS -- \
    "$FAKE_TEST" --shuffle 123 --warn-is-error >/dev/null 2>&1
status=$?
set -e
expect_status 2 "$status" "hidden selector environment is rejected"

run_case exit-124
expect_status 124 "$CASE_STATUS" "arbitrary child exit 124 is preserved"
expect_contains "$CASE_DIR/metadata.json" '"timedOut": false' "arbitrary exit 124 is not authenticated as timeout"
expect_contains "$CASE_DIR/metadata.json" '"classification": "process-failure"' "arbitrary exit 124 remains process failure"

run_case timeout asan 1
expect_status 124 "$CASE_STATUS" "test timeout is enforced"
expect_contains "$CASE_DIR/metadata.json" '"timedOut": true' "timeout evidence is explicit"
expect_contains "$CASE_DIR/metadata.json" '"classification": "timeout"' "timeout classification remains distinct"

run_case self-term
expect_status 143 "$CASE_STATUS" "child signal exit is preserved"
footer_count="$(grep -Fc '=== SparkEngine CI sanitizer process result ===' "$CASE_DIR/process-footer.txt")"
[[ "$footer_count" -eq 1 ]] && pass "signal footer is idempotent" || fail "signal footer is idempotent"

# Interrupt the wrapper itself while its child is active. EXIT and TERM both
# attempt finalization, so the single footer proves the handler is idempotent.
counter=$((counter + 1))
interrupt_dir="$(case_dir asan "$counter")"
FAKE_MODE=timeout bash "$RUNNER" \
    --evidence-root "$TMP_ROOT" --sanitizer asan --expected-sha "$SHA" \
    --run-id "$counter" --run-attempt 1 --job build-linux-asan --expected-selector all \
    --minimum-tests 1 --timeout-seconds 2 --runtime-env ASAN_OPTIONS -- \
    "$FAKE_TEST" --shuffle 123 --warn-is-error >/dev/null 2>&1 &
wrapper_pid=$!
interrupt_ready=0
for _ in $(seq 1 100); do
    if [[ -f "$interrupt_dir/console.txt" ]]; then
        interrupt_ready=1
        break
    fi
    sleep 0.1
done
[[ "$interrupt_ready" -eq 1 ]] || fail "wrapper reached interruptible execution state"
kill -TERM "$wrapper_pid"
set +e
wait "$wrapper_pid"
status=$?
set -e
expect_status 143 "$status" "wrapper TERM interruption is preserved"
expect_contains "$interrupt_dir/process-footer.txt" "signal_received=TERM" "wrapper interruption signal is recorded"
footer_count="$(grep -Fc '=== SparkEngine CI sanitizer process result ===' "$interrupt_dir/process-footer.txt")"
[[ "$footer_count" -eq 1 ]] && pass "TERM and EXIT handlers write one footer" || fail "TERM and EXIT footer idempotence"

# Published artifact verification consumes, rather than trusts, metadata,
# statistics, all three content hashes, and external workflow identity.
run_case clean
published_dir="$CASE_DIR"
published_run="$CASE_RUN"
"$TEST_PYTHON" "$SUMMARIZER" "$published_dir/junit.xml" \
    --min-tests 1 --title "Synthetic ASan" \
    --json "$published_dir/test-stats-linux-asan.json" >/dev/null

verify_published() {
    local expected_sha="${1:-$SHA}" expected_run="${2:-$published_run}"
    set +e
    "$TEST_PYTHON" "$VERIFIER" verify-published \
        --evidence-dir "$published_dir" \
        --stats "$published_dir/test-stats-linux-asan.json" \
        --sanitizer asan --lane linux-asan \
        --expected-sha "$expected_sha" --run-id "$expected_run" --run-attempt 1 \
        --job build-linux-asan --minimum-tests 1 --timeout-seconds 10 >/dev/null 2>&1
    PUBLISHED_STATUS=$?
    set -e
}

verify_published
expect_status 0 "$PUBLISHED_STATUS" "published clean artifact is independently verified"
rmdir "$published_dir/runtime"
verify_published
expect_status 0 "$PUBLISHED_STATUS" "published evidence tolerates dropped empty runtime directory"
for artifact_file in \
    console.txt report.txt junit.xml metadata.json process-footer.txt test-stats-linux-asan.json; do
    hardlink_path="$TMP_ROOT/published-hardlink-${artifact_file}"
    "$TEST_PYTHON" -c 'import os, sys; os.link(sys.argv[1], sys.argv[2])' \
        "$published_dir/$artifact_file" "$hardlink_path"
    verify_published
    expect_status 70 "$PUBLISHED_STATUS" "published $artifact_file rejects an external hard link"
    rm "$hardlink_path"
done
for artifact_file in console.txt report.txt junit.xml; do
    cp "$published_dir/$artifact_file" "$TMP_ROOT/published-$artifact_file"
    printf '\n' >> "$published_dir/$artifact_file"
    verify_published
    expect_status 70 "$PUBLISHED_STATUS" "published $artifact_file hash mismatch is rejected"
    cp "$TMP_ROOT/published-$artifact_file" "$published_dir/$artifact_file"
done
verify_published aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
expect_status 70 "$PUBLISHED_STATUS" "published evidence rejects a different external SHA"
verify_published "$SHA" 999999
expect_status 70 "$PUBLISHED_STATUS" "published evidence rejects a different external run ID"

cp "$published_dir/metadata.json" "$TMP_ROOT/published-metadata.json"
cp "$published_dir/process-footer.txt" "$TMP_ROOT/published-footer.txt"
tamper_metadata_and_rebind_footer() {
    local field="$1" value="$2"
    local value_mode="${3:-string}"
    "$TEST_PYTHON" - "$published_dir/metadata.json" "$published_dir/process-footer.txt" \
        "$field" "$value" "$value_mode" <<'PY'
import hashlib, json, sys
metadata_path, footer_path, field, replacement = sys.argv[1:5]
if len(sys.argv) > 5 and sys.argv[5] == "json":
    replacement = json.loads(replacement)
with open(metadata_path, encoding="utf-8") as stream:
    value = json.load(stream)
target = value
parts = field.split(".")
for part in parts[:-1]:
    target = target[part]
target[parts[-1]] = replacement
payload = (json.dumps(value, indent=2, sort_keys=True) + "\n").encode()
with open(metadata_path, "wb") as stream:
    stream.write(payload)
digest = hashlib.sha256(payload).hexdigest()
with open(footer_path, encoding="utf-8") as stream:
    lines = stream.read().splitlines()
lines = [f"metadata_sha256={digest}" if line.startswith("metadata_sha256=") else line for line in lines]
with open(footer_path, "w", encoding="utf-8", newline="\n") as stream:
    stream.write("\n".join(lines) + "\n")
PY
}

tamper_metadata_and_rebind_footer provenance.runAttempt true json
verify_published
expect_status 70 "$PUBLISHED_STATUS" "published metadata rejects bool/int identity ambiguity"
cp "$TMP_ROOT/published-metadata.json" "$published_dir/metadata.json"
cp "$TMP_ROOT/published-footer.txt" "$published_dir/process-footer.txt"

"$TEST_PYTHON" - "$published_dir/metadata.json" "$published_dir/process-footer.txt" <<'PY'
import hashlib
import sys

metadata_path, footer_path = sys.argv[1:]
with open(metadata_path, "rb") as stream:
    payload = stream.read()
payload = payload.replace(b"{\n", b'{\n  "schemaVersion": 2,\n', 1)
with open(metadata_path, "wb") as stream:
    stream.write(payload)
digest = hashlib.sha256(payload).hexdigest()
with open(footer_path, encoding="utf-8") as stream:
    lines = stream.read().splitlines()
lines = [f"metadata_sha256={digest}" if line.startswith("metadata_sha256=") else line for line in lines]
with open(footer_path, "w", encoding="utf-8", newline="\n") as stream:
    stream.write("\n".join(lines) + "\n")
PY
verify_published
expect_status 70 "$PUBLISHED_STATUS" "published metadata rejects duplicate JSON keys"
cp "$TMP_ROOT/published-metadata.json" "$published_dir/metadata.json"
cp "$TMP_ROOT/published-footer.txt" "$published_dir/process-footer.txt"

tamper_metadata_and_rebind_footer classification test-failure
verify_published
expect_status 70 "$PUBLISHED_STATUS" "published evidence rejects caller-stamped classification"
cp "$TMP_ROOT/published-metadata.json" "$published_dir/metadata.json"
cp "$TMP_ROOT/published-footer.txt" "$published_dir/process-footer.txt"

tamper_metadata_and_rebind_footer provenance.sanitizer tsan
verify_published
expect_status 70 "$PUBLISHED_STATUS" "published evidence rejects a forged sanitizer kind"
cp "$TMP_ROOT/published-metadata.json" "$published_dir/metadata.json"
cp "$TMP_ROOT/published-footer.txt" "$published_dir/process-footer.txt"

cp "$published_dir/test-stats-linux-asan.json" "$TMP_ROOT/published-stats.json"
"$TEST_PYTHON" - "$published_dir/test-stats-linux-asan.json" <<'PY'
import json, sys
path = sys.argv[1]
with open(path, encoding="utf-8") as stream:
    value = json.load(stream)
value["tests"] += 1
with open(path, "w", encoding="utf-8", newline="\n") as stream:
    json.dump(value, stream)
    stream.write("\n")
PY
verify_published
expect_status 70 "$PUBLISHED_STATUS" "published statistics cannot spoof strict JUnit counts"
cp "$TMP_ROOT/published-stats.json" "$published_dir/test-stats-linux-asan.json"

printf 'unexpected\n' > "$published_dir/extra.json"
verify_published
expect_status 70 "$PUBLISHED_STATUS" "published artifact rejects ambiguous extra evidence"
rm "$published_dir/extra.json"

# Static workflow contracts: required/optional policy, timeouts, aggregation,
# and private exact-provenance paths remain reviewable without running C++.
[[ "$(grep -Fc 'bash .github/scripts/run-sanitizer-tests.sh' "$WORKFLOW")" -eq 3 ]] && \
    pass "exactly three sanitizer runner invocations" || fail "sanitizer runner invocation count"
grep -Fq -- '--warn-is-error --shuffle 123' "$WORKFLOW" && pass "workflow hardens flaky warnings and shuffle seed" || fail "workflow warn/shuffle contract"
grep -Fq 'timeout-minutes: 90' "$WORKFLOW" && pass "sanitizer jobs have bounded job time" || fail "sanitizer job timeout"
grep -Fq -- '--timeout-seconds 900' "$WORKFLOW" && pass "sanitizer test process timeout present" || fail "sanitizer process timeout"
grep -Fq -- '--expected-sha "${{ github.sha }}"' "$WORKFLOW" && pass "workflow binds exact SHA" || fail "workflow exact SHA"
grep -Fq 'test-results-linux-asan' "$WORKFLOW" && pass "ASan exact-commit artifact present" || fail "ASan aggregate artifact"
grep -Fq 'test-results-linux-tsan' "$WORKFLOW" && pass "TSan exact-commit artifact present" || fail "TSan aggregate artifact"

echo "Sanitizer pipeline tests: $passed passed, $failed failed, $skipped skipped"
if [[ "$failed" -ne 0 ]]; then
    exit 1
fi
echo "Sanitizer evidence checks passed."
