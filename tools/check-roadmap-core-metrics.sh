#!/usr/bin/env bash
# check-roadmap-core-metrics.sh
# Validates core metrics snapshots in roadmap docs against .claude/index.md.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
INDEX_FILE="$REPO_ROOT/.claude/index.md"
ROADMAP_GLOB="$REPO_ROOT/.claude/knowledge/*recommendations-*.md"

if [[ ! -f "$INDEX_FILE" ]]; then
    echo "[ERROR] Missing index file: $INDEX_FILE"
    exit 1
fi

index_tests_line="$(grep -E '^- \*\*Tests\*\*:' "$INDEX_FILE" | head -1 || true)"
index_codebase_line="$(grep -E '^- \*\*Codebase\*\*:' "$INDEX_FILE" | head -1 || true)"

if [[ -z "$index_tests_line" || -z "$index_codebase_line" ]]; then
    echo "[ERROR] Could not find core metric lines in $INDEX_FILE"
    exit 1
fi

index_tests_files="$(echo "$index_tests_line" | sed -nE 's/.*: ([0-9]+)\+? test files, ~?([0-9]+) tests.*/\1/p')"
index_tests_total="$(echo "$index_tests_line" | sed -nE 's/.*: ([0-9]+)\+? test files, ~?([0-9]+) tests.*/\2/p')"
index_loc_k="$(echo "$index_codebase_line" | sed -nE 's/.*: ~([0-9]+)K lines of C\+\+ across ([0-9]+) source files.*/\1/p')"
index_source_files="$(echo "$index_codebase_line" | sed -nE 's/.*: ~([0-9]+)K lines of C\+\+ across ([0-9]+) source files.*/\2/p')"

if [[ -z "$index_tests_files" || -z "$index_tests_total" || -z "$index_loc_k" || -z "$index_source_files" ]]; then
    echo "[ERROR] Failed to parse index metrics from $INDEX_FILE"
    exit 1
fi

echo "[INFO] Index metrics: tests_files=$index_tests_files tests_total=$index_tests_total loc_k=$index_loc_k source_files=$index_source_files"

errors=0
checked=0
skipped=0

for doc in $ROADMAP_GLOB; do
    [[ -f "$doc" ]] || continue

    core_line="$(grep -E '^<!-- CORE_METRICS:' "$doc" | head -1 || true)"
    source_line="$(grep -E '^<!-- SNAPSHOT_SOURCE:' "$doc" | head -1 || true)"
    date_line="$(grep -E '^<!-- SNAPSHOT_DATE:' "$doc" | head -1 || true)"

    if [[ -z "$core_line" && -z "$source_line" && -z "$date_line" ]]; then
        echo "[INFO] Skipping $(basename "$doc") (no snapshot metadata)."
        skipped=$((skipped + 1))
        continue
    fi

    checked=$((checked + 1))

    if [[ "$source_line" != "<!-- SNAPSHOT_SOURCE: .claude/index.md -->" ]]; then
        echo "[ERROR] $(basename "$doc"): SNAPSHOT_SOURCE must be '.claude/index.md'"
        errors=$((errors + 1))
    fi

    if [[ -z "$date_line" ]]; then
        echo "[ERROR] $(basename "$doc"): missing SNAPSHOT_DATE metadata."
        errors=$((errors + 1))
    fi

    doc_tests_files="$(echo "$core_line" | sed -nE 's/.*tests_files=([0-9]+).*/\1/p')"
    doc_tests_total="$(echo "$core_line" | sed -nE 's/.*tests_total=([0-9]+).*/\1/p')"
    doc_loc_k="$(echo "$core_line" | sed -nE 's/.*loc_k=([0-9]+).*/\1/p')"
    doc_source_files="$(echo "$core_line" | sed -nE 's/.*source_files=([0-9]+).*/\1/p')"

    if [[ -z "$doc_tests_files" || -z "$doc_tests_total" || -z "$doc_loc_k" || -z "$doc_source_files" ]]; then
        echo "[ERROR] $(basename "$doc"): malformed CORE_METRICS metadata."
        errors=$((errors + 1))
        continue
    fi

    if [[ "$doc_tests_files" != "$index_tests_files" || "$doc_tests_total" != "$index_tests_total" || "$doc_loc_k" != "$index_loc_k" || "$doc_source_files" != "$index_source_files" ]]; then
        echo "[ERROR] $(basename "$doc"): core metrics mismatch."
        echo "        doc   : tests_files=$doc_tests_files tests_total=$doc_tests_total loc_k=$doc_loc_k source_files=$doc_source_files"
        echo "        index : tests_files=$index_tests_files tests_total=$index_tests_total loc_k=$index_loc_k source_files=$index_source_files"
        errors=$((errors + 1))
    else
        echo "[OK] $(basename "$doc"): core metrics match index."
    fi
done

echo "[INFO] Checked: $checked, Skipped: $skipped, Errors: $errors"

if [[ $errors -ne 0 ]]; then
    exit 1
fi

