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

violations="$(rg -n "Spark::NetworkManager::GetInstance\s*\(" "$TARGET_FILE" | rg -v "DI_SHIM_OK" || true)"
if [ -n "$violations" ]; then
    echo "ERROR: Forbidden singleton usage found in DI-required module:"
    echo "$violations"
    echo "Use EngineContext::GetNetworkService() instead."
    exit 1
fi

echo "DI singleton guard passed: networking in GameplaySystemLifecycle uses EngineContext services."
