#!/bin/bash

# Ensures ThirdParty/dependencies.lock is updated when dependency wiring changes.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
MANIFEST="ThirdParty/dependencies.lock"

cd "$PROJECT_ROOT"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info()    { echo -e "${BLUE}[THIRDPARTY-MANIFEST]${NC} $1"; }
log_success() { echo -e "${GREEN}[THIRDPARTY-MANIFEST]${NC} $1"; }
log_warn()    { echo -e "${YELLOW}[THIRDPARTY-MANIFEST]${NC} $1"; }
log_error()   { echo -e "${RED}[THIRDPARTY-MANIFEST]${NC} $1"; }

if [ ! -f "$MANIFEST" ]; then
    log_error "Missing manifest: $MANIFEST"
    exit 1
fi

if [ "${CI:-}" = "true" ] || [ "${GITHUB_ACTIONS:-}" = "true" ]; then
    if git rev-parse --verify origin/Working >/dev/null 2>&1; then
        BASE_REF="$(git merge-base HEAD origin/Working)"
    else
        BASE_REF="$(git rev-parse HEAD~1 2>/dev/null || git rev-parse HEAD)"
    fi
    CHANGED_FILES="$(git diff --name-only "$BASE_REF"...HEAD)"
    WIRING_DIFF_CMD=(git diff "$BASE_REF"...HEAD -- .gitmodules CMakeLists.txt cmake/SparkThirdPartyAudit.cmake)
else
    CHANGED_FILES="$(git diff --name-only HEAD --)"
    WIRING_DIFF_CMD=(git diff HEAD -- .gitmodules CMakeLists.txt cmake/SparkThirdPartyAudit.cmake)
fi
UNTRACKED_FILES="$(git ls-files --others --exclude-standard)"
if [ -n "$UNTRACKED_FILES" ]; then
    if [ -n "$CHANGED_FILES" ]; then
        CHANGED_FILES="$(printf "%s\n%s" "$CHANGED_FILES" "$UNTRACKED_FILES")"
    else
        CHANGED_FILES="$UNTRACKED_FILES"
    fi
fi

if [ -z "$CHANGED_FILES" ]; then
    log_success "No file changes detected"
    exit 0
fi

manifest_changed=false
if echo "$CHANGED_FILES" | grep -Fxq "$MANIFEST"; then
    manifest_changed=true
fi

# Trigger 1: third-party directories changed (vendor update / submodule pointer update / file edits)
thirdparty_changed=false
if echo "$CHANGED_FILES" | grep -qE '^ThirdParty/'; then
    thirdparty_changed=true
fi

# Trigger 2: dependency wiring changed in build config.
# Tokens are intentionally specific to third-party references; broader
# tokens like _DIR/_ROOT/version/commit used to live here but triggered
# false positives on every routine CMAKE_SOURCE_DIR line. If you add a
# new third-party dep without matching one of these patterns, bump the
# manifest manually.
wiring_changed=false
if "${WIRING_DIFF_CMD[@]}" \
    | grep -Eq '^[+-].*(ThirdParty/|https://|\.git|SPARK_HAS_|SPARK_RECAST_AVAILABLE|SPARK_JOLT_PHYSICS_AVAILABLE)'; then
    wiring_changed=true
fi

if { [ "$thirdparty_changed" = true ] || [ "$wiring_changed" = true ]; } && [ "$manifest_changed" = false ]; then
    log_error "Dependency paths/URLs/versions changed but $MANIFEST was not updated."
    log_warn "Changed files:"
    echo "$CHANGED_FILES" | sed 's/^/  - /'
    exit 1
fi

log_success "Third-party manifest sync check passed"
exit 0
