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

is_dependabot_pull_request() {
    [ "${GITHUB_EVENT_NAME:-}" = "pull_request" ] || return 1
    [ "${GITHUB_ACTOR:-}" = "dependabot[bot]" ] || return 1
    [ -n "${GITHUB_EVENT_PATH:-}" ] || return 1
    [ -f "$GITHUB_EVENT_PATH" ] || return 1

    node - "$GITHUB_EVENT_PATH" "${GITHUB_REPOSITORY:-}" <<'JS'
const fs = require("fs");

const event = JSON.parse(fs.readFileSync(process.argv[2], "utf8"));
const pullRequest = event.pull_request || {};
const repository = event.repository || {};
const headRepository = (pullRequest.head || {}).repo || {};
const expectedRepository = process.argv[3];

const valid =
    (pullRequest.user || {}).login === "dependabot[bot]" &&
    repository.full_name &&
    headRepository.full_name === repository.full_name &&
    (!expectedRepository || repository.full_name === expectedRepository);
process.exit(valid ? 0 : 1);
JS
}

is_pointer_only_submodule_diff() {
    local base_ref="$1"
    local raw_diff=""
    local line=""
    local path=""
    local pointer_count=0
    local pointer_pattern=$'^:160000 160000 [0-9a-f]{40} [0-9a-f]{40} M\t(.+)$'

    raw_diff="$(git diff --raw --no-abbrev --no-renames "$base_ref"...HEAD --)"
    [ -n "$raw_diff" ] || return 1

    while IFS= read -r line; do
        # Only an existing mode-160000 entry moving to another mode-160000
        # entry is accepted. Additions, removals, renames, ordinary files, and
        # mixed diffs all fall back to the normal manifest-update requirement.
        if [[ ! "$line" =~ $pointer_pattern ]]; then
            return 1
        fi
        path="${BASH_REMATCH[1]}"
        [[ "$path" == ThirdParty/* ]] || return 1
        git config --file .gitmodules --get-regexp '^submodule\..*\.path$' \
            | awk '{print $2}' \
            | grep -Fxq "$path" || return 1
        grep -Fq "|$path|" "$MANIFEST" || return 1
        pointer_count=$((pointer_count + 1))
    done <<< "$raw_diff"

    [ "$pointer_count" -gt 0 ]
}

if [ ! -f "$MANIFEST" ]; then
    log_error "Missing manifest: $MANIFEST"
    exit 1
fi

# Parse the manifest with CMake itself before examining git drift. Quoted
# semicolons are CMake list separators unless escaped, so a record can look
# like nine pipe-delimited fields to a text-only check while becoming two
# invalid entries at configure time.
cmake \
    -DSPARK_THIRDPARTY_AUDIT_VALIDATE_ONLY=ON \
    -DSPARK_THIRDPARTY_MANIFEST="$PROJECT_ROOT/$MANIFEST" \
    -P "$PROJECT_ROOT/cmake/SparkThirdPartyAudit.cmake"

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
    if [ -n "${BASE_REF:-}" ] \
        && is_dependabot_pull_request \
        && is_pointer_only_submodule_diff "$BASE_REF"; then
        log_success "Verified Dependabot pointer-only submodule update against repository gitlinks"
        exit 0
    fi

    log_error "Dependency paths/URLs/versions changed but $MANIFEST was not updated."
    log_warn "Changed files:"
    echo "$CHANGED_FILES" | sed 's/^/  - /'
    exit 1
fi

log_success "Third-party manifest sync check passed"
exit 0
