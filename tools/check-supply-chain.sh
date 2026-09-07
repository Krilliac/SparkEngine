#!/bin/bash

# Wrapper to run the Python supply-chain policy checker from validate-all.sh.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

python3 "$SCRIPT_DIR/check-supply-chain.py" "$@"
