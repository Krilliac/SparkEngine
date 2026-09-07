#!/bin/bash
# Wrapper for validate-all.sh integration.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec python3 "$SCRIPT_DIR/check-supply-chain.py" "$@"
