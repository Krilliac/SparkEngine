#!/usr/bin/env bash
# validate-prompts.sh — Validates AI prompt system integrity
#
# Addresses:
#   - Task-to-prompt mapping drift (#95): checks that prompt files reference
#     source files/directories that actually exist in the codebase.
#   - Prompt overload prevention (#94): warns if too many domain prompts would
#     be loaded simultaneously (enforces single-domain-at-a-time rule).
#   - Stale prompt/context detection (#93): flags prompt files that reference
#     source paths, classes, or files that no longer exist.
#
# Usage:
#   ./tools/validate-prompts.sh          # Run all checks
#   ./tools/validate-prompts.sh --ci     # Exit with non-zero on errors (for CI)
#
# Can be integrated into CI via a GitHub Actions workflow step.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROMPTS_DIR="$REPO_ROOT/.github/prompts"
EXIT_CODE=0
CI_MODE=false
WARNINGS=0
ERRORS=0

if [[ "${1:-}" == "--ci" ]]; then
    CI_MODE=true
fi

log_info()    { echo "  [INFO]  $*"; }
log_warn()    { echo "  [WARN]  $*"; WARNINGS=$((WARNINGS + 1)); }
log_error()   { echo "  [ERROR] $*"; ERRORS=$((ERRORS + 1)); EXIT_CODE=1; }
log_ok()      { echo "  [OK]    $*"; }

echo "============================================"
echo " SparkEngine Prompt Validation"
echo "============================================"
echo ""

# -------------------------------------------------------------------------
# Check 1: Prompt files exist and are non-empty
# -------------------------------------------------------------------------
echo "--- Check 1: Prompt file integrity ---"

EXPECTED_PROMPTS=(
    "copilot-instructions.md"
    "engine-core.prompt.md"
    "graphics-rendering.prompt.md"
    "audio-physics.prompt.md"
    "gameplay-systems.prompt.md"
    "editor-tools.prompt.md"
    "build-test.prompt.md"
    "console-scripting.prompt.md"
    "token-optimization.prompt.md"
)

for prompt in "${EXPECTED_PROMPTS[@]}"; do
    if [[ ! -f "$PROMPTS_DIR/$prompt" ]]; then
        log_error "Missing prompt file: $prompt"
    elif [[ ! -s "$PROMPTS_DIR/$prompt" ]]; then
        log_error "Empty prompt file: $prompt"
    else
        log_ok "$prompt exists and is non-empty"
    fi
done

echo ""

# -------------------------------------------------------------------------
# Check 2: Source path references in prompts still exist (anti-drift)
# -------------------------------------------------------------------------
echo "--- Check 2: Source path references (anti-drift) ---"

STALE_REFS=0
for prompt_file in "$PROMPTS_DIR"/*.md; do
    basename_prompt="$(basename "$prompt_file")"
    # Skip README
    [[ "$basename_prompt" == "README.md" ]] && continue

    # Extract file paths that look like SparkEngine/Source/... or SparkEngine/Source/.../*.h
    refs="$(grep -oE 'SparkEngine/Source/[A-Za-z0-9_/]+\.[a-z]+' "$prompt_file" 2>/dev/null || true)"
    while IFS= read -r ref_path; do
        [[ -z "$ref_path" ]] && continue
        # Clean up the path (remove backticks, trailing punctuation)
        clean_path="$(echo "$ref_path" | sed 's/[`"'\''(),]//g' | sed 's/[[:space:]]*$//')"
        # Skip if it contains wildcards or is too short
        [[ "$clean_path" == *"*"* ]] && continue
        [[ ${#clean_path} -lt 10 ]] && continue
        # Check if it exists relative to repo root
        if [[ ! -e "$REPO_ROOT/$clean_path" ]]; then
            # Check if it's a directory reference (might have trailing /)
            dir_path="${clean_path%/}"
            if [[ ! -e "$REPO_ROOT/$dir_path" ]]; then
                log_warn "Stale reference in $basename_prompt: $clean_path"
                STALE_REFS=$((STALE_REFS + 1))
            fi
        fi
    done <<< "$refs"
done

if [[ $STALE_REFS -eq 0 ]]; then
    log_ok "No stale source path references found"
fi

echo ""

# -------------------------------------------------------------------------
# Check 3: Prompt overload detection
# -------------------------------------------------------------------------
echo "--- Check 3: Prompt overload prevention ---"

DOMAIN_COUNT=${#EXPECTED_PROMPTS[@]}
# Subtract copilot-instructions (auto-loaded) and token-optimization (meta)
LOADABLE_DOMAINS=$((DOMAIN_COUNT - 2))
MAX_RECOMMENDED=2

log_info "Total domain prompts available: $LOADABLE_DOMAINS"
log_info "Recommended max per task: $MAX_RECOMMENDED"

# Check if the token-optimization prompt has the warning
if grep -q "Never load all domain prompts" "$PROMPTS_DIR/token-optimization.prompt.md" 2>/dev/null; then
    log_ok "Token optimization prompt contains overload warning"
else
    log_warn "Token optimization prompt missing 'never load all' guidance"
fi

# Check that each domain prompt references the shared context
for prompt_file in "$PROMPTS_DIR"/*.prompt.md; do
    basename_prompt="$(basename "$prompt_file")"
    [[ "$basename_prompt" == "token-optimization.prompt.md" ]] && continue
    if ! grep -q "copilot-instructions" "$prompt_file" 2>/dev/null; then
        log_warn "$basename_prompt does not reference copilot-instructions.md (possible duplication)"
    fi
done

echo ""

# -------------------------------------------------------------------------
# Check 4: .promptignore validation
# -------------------------------------------------------------------------
echo "--- Check 4: .promptignore file ---"

PROMPTIGNORE="$REPO_ROOT/.promptignore"
if [[ -f "$PROMPTIGNORE" ]]; then
    log_ok ".promptignore exists"
    # Verify key exclusions are present
    for dir in "ThirdParty/" "build/" "Shaders/Compiled/"; do
        if grep -q "^${dir}" "$PROMPTIGNORE" 2>/dev/null; then
            log_ok ".promptignore excludes $dir"
        else
            log_warn ".promptignore missing exclusion for $dir"
        fi
    done
else
    log_error ".promptignore file not found at repo root"
fi

echo ""

# -------------------------------------------------------------------------
# Check 5: Task-to-prompt mapping table consistency
# -------------------------------------------------------------------------
echo "--- Check 5: Task-to-prompt mapping table ---"

# Verify the mapping table in token-optimization.prompt.md references valid prompts
# Extract only from the "Which Prompt to Load" table: lines matching "| description | `name` |"
if [[ -f "$PROMPTS_DIR/token-optimization.prompt.md" ]]; then
    mapping_refs="$(sed -n '/Which Prompt to Load/,/^## /p' "$PROMPTS_DIR/token-optimization.prompt.md" | \
                    sed -n 's/.*| `\([a-z][a-z-]*\)` |.*/\1/p')"
    while IFS= read -r prompt_ref; do
        [[ -z "$prompt_ref" ]] && continue
        if [[ -f "$PROMPTS_DIR/${prompt_ref}.prompt.md" ]]; then
            log_ok "Mapping reference '$prompt_ref' -> ${prompt_ref}.prompt.md exists"
        else
            log_error "Mapping reference '$prompt_ref' has no corresponding prompt file"
        fi
    done <<< "$mapping_refs"
fi

echo ""

# -------------------------------------------------------------------------
# Summary
# -------------------------------------------------------------------------
echo "============================================"
echo " Summary: $ERRORS error(s), $WARNINGS warning(s)"
echo "============================================"

if [[ $ERRORS -gt 0 ]]; then
    echo " FAILED — fix errors above before merging."
elif [[ $WARNINGS -gt 0 ]]; then
    echo " PASSED with warnings — review items above."
else
    echo " ALL CHECKS PASSED"
fi

if $CI_MODE; then
    exit $EXIT_CODE
fi
