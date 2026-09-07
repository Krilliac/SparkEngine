#!/usr/bin/env bash
# check-fuzz-policy.sh - SEC-120 blocking parser-inventory / fuzz-policy contract.
#
# Runs the same gate CI runs, so a local pre-commit run cannot report clean on a
# change CI will reject.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

PYTHON_BIN=""
for candidate in python3 python; do
    if command -v "$candidate" >/dev/null 2>&1 && "$candidate" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)' >/dev/null 2>&1; then
        PYTHON_BIN="$candidate"
        break
    fi
done

if [ -z "$PYTHON_BIN" ]; then
    echo "check-fuzz-policy: no Python 3.10+ interpreter found" >&2
    exit 1
fi

exec "$PYTHON_BIN" "$REPO_ROOT/tools/fuzz-policy/check_fuzz_policy.py" --source-root "$REPO_ROOT" --ci
