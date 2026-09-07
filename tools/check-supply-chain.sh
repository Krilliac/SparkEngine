#!/bin/bash
# Wrapper for validate-all.sh integration.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Git Bash on Windows can expose a Windows Store ``python3`` alias that exists
# on PATH but cannot launch, while the installed interpreter is named ``python``.
# Prefer python3 where it works (Linux CI), then fall back only to a runnable
# python executable rather than treating a command-name match as evidence.
PYTHON=""
for candidate in python3 python; do
    if command -v "$candidate" >/dev/null 2>&1 && "$candidate" -c "import sys" >/dev/null 2>&1; then
        PYTHON="$candidate"
        break
    fi
done

if [ -z "$PYTHON" ]; then
    echo "check-supply-chain.sh requires a runnable Python interpreter (python3 or python)." >&2
    exit 127
fi

exec "$PYTHON" "$SCRIPT_DIR/check-supply-chain.py" "$@"
