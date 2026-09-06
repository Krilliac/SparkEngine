#!/bin/bash

# Enforce DI boundaries for migrated modules.
# Fails if forbidden singleton lookups appear in files that must use EngineContext services.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

TARGET_FILE="$PROJECT_ROOT/SparkEngine/Source/Core/GameplaySystemLifecycle.cpp"

if [ ! -f "$TARGET_FILE" ]; then
    echo "ERROR: Missing target file: $TARGET_FILE"
    exit 1
fi

# rg is absent on Windows Git Bash and on plain CI images. With `|| true` the
# command-not-found produced an empty result and this guard reported success,
# so resolve the searcher once and fail closed when neither tool is present.
if command -v rg >/dev/null 2>&1; then
    spark_search_lines() { rg -n "$1" "$2"; }
    spark_reject_lines()  { rg -v "$1"; }
elif command -v grep >/dev/null 2>&1; then
    spark_search_lines() { grep -nE -- "$1" "$2"; }
    spark_reject_lines()  { grep -vF -- "$1"; }
else
    echo "ERROR: neither rg nor grep is available; the DI singleton guard cannot run" >&2
    exit 2
fi

violations="$(spark_search_lines "Spark::NetworkManager::GetInstance[[:space:]]*\(" "$TARGET_FILE" \
    | spark_reject_lines "DI_SHIM_OK" || true)"
if [ -n "$violations" ]; then
    echo "ERROR: Forbidden singleton usage found in DI-required module:"
    echo "$violations"
    echo "Use EngineContext::GetNetworkService() instead."
    exit 1
fi

echo "DI singleton guard passed: networking in GameplaySystemLifecycle uses EngineContext services."
