#!/bin/bash

# SparkEngine Cross-Utilization Guardrails
#
# Enforces architectural boundaries discussed in docs/architecture/dependency-matrix.md.
# Usage:
#   ./tools/check-cross-utilization.sh
#   ./tools/check-cross-utilization.sh check

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info()    { echo -e "${BLUE}[XUTIL]${NC} $1"; }
log_success() { echo -e "${GREEN}[XUTIL]${NC} $1"; }
log_warning() { echo -e "${YELLOW}[XUTIL]${NC} $1"; }
log_error()   { echo -e "${RED}[XUTIL]${NC} $1"; }

VIOLATIONS=0

record_violation() {
    local message="$1"
    log_error "$message"
    VIOLATIONS=$((VIOLATIONS + 1))
}

# ripgrep is not installed on Windows Git Bash or on plain CI images, and every
# search below swallowed failures with `|| true`. A missing rg therefore made
# each boundary check log success on an empty result, while the negated
# `if ! rg -q` module checks turned exit 127 into a violation for every module.
# Resolve the searcher once and fail closed when neither tool exists.
if command -v rg >/dev/null 2>&1; then
    SPARK_SEARCHER=rg
elif command -v grep >/dev/null 2>&1; then
    SPARK_SEARCHER=grep
else
    log_error "Neither rg nor grep is available; cross-utilization checks cannot run"
    exit 2
fi

# spark_search <pattern> <sources|cpp> <path>...
# Patterns are POSIX ERE so both searchers accept them verbatim.
spark_search() {
    local pattern="$1"
    local include_spec="$2"
    shift 2
    if [ "$SPARK_SEARCHER" = "rg" ]; then
        if [ "$include_spec" = "cpp" ]; then
            rg -n --no-heading --color never "$pattern" "$@" --glob '*.cpp' || true
        else
            rg -n --no-heading --color never "$pattern" "$@" --glob '*.[ch]pp' --glob '*.h' || true
        fi
    else
        if [ "$include_spec" = "cpp" ]; then
            grep -rnE --include='*.cpp' -- "$pattern" "$@" || true
        else
            grep -rnE --include='*.cpp' --include='*.hpp' --include='*.h' -- "$pattern" "$@" || true
        fi
    fi
}

# spark_file_matches <pattern> <file>
spark_file_matches() {
    if [ "$SPARK_SEARCHER" = "rg" ]; then
        rg -q "$1" "$2"
    else
        grep -qE -- "$1" "$2"
    fi
}

check_game_modules_boundary() {
    log_info "Checking GameModules include boundaries..."

    local result
    result=$(spark_search '#include[[:space:]]*[<"]SparkEngine/Source/' sources \
        "$PROJECT_ROOT/GameModules")

    if [ -n "$result" ]; then
        while IFS= read -r line; do
            [ -n "$line" ] && record_violation "Game module includes engine private header: $line"
        done <<< "$result"
    else
        log_success "GameModules do not include SparkEngine/Source private headers"
    fi
}

check_deprecated_globals() {
    log_info "Checking deprecated global service usage..."

    local allowlist
    allowlist='SparkEngine/Source/Core/SparkEngine.cpp|SparkEngine/Source/Core/SparkEngineLinux.cpp|SparkEngine/Source/Core/SparkEngineWindows.cpp|SparkEngine/Source/Core/GameplaySystemLifecycle.cpp'

    local result
    result=$(spark_search '\bg_(graphics|input|audio|physics|network|scripting|resource|timer|eventBus)\b' sources \
        "$PROJECT_ROOT/SparkEngine" "$PROJECT_ROOT/SparkEditor" "$PROJECT_ROOT/GameModules")

    if [ -n "$result" ]; then
        while IFS= read -r line; do
            [ -z "$line" ] && continue
            local rel
            rel="${line#$PROJECT_ROOT/}"
            if [[ ! "$rel" =~ ^($allowlist): ]]; then
                record_violation "Deprecated global usage outside lifecycle allowlist: $rel"
            fi
        done <<< "$result"
    fi

    if [ "$VIOLATIONS" -eq 0 ]; then
        log_success "Deprecated globals are only used in lifecycle allowlist"
    fi
}

check_subsystem_lateral_includes() {
    log_info "Checking lateral subsystem include coupling..."

    local engine_root="$PROJECT_ROOT/SparkEngine/Source/Engine"
    local domains=(AI Animation Audio Dialogue Gameplay Loading Localization Modding Persistence Physics Replay SaveSystem Streaming Tween UI VR World)

    for source_domain in "${domains[@]}"; do
        local source_path="$engine_root/$source_domain"
        [ -d "$source_path" ] || continue

        local regex=''
        local first=true
        for target_domain in "${domains[@]}"; do
            [ "$target_domain" = "$source_domain" ] && continue
            if [ "$first" = true ]; then
                regex="(#include[[:space:]]*[<\"]Engine/${target_domain}/)"
                first=false
            else
                regex+="|(#include[[:space:]]*[<\"]Engine/${target_domain}/)"
            fi
        done

        local result
        result=$(spark_search "$regex" sources "$source_path")
        if [ -n "$result" ]; then
            while IFS= read -r line; do
                [ -n "$line" ] && record_violation "Lateral domain include in Engine/$source_domain: ${line#$PROJECT_ROOT/}"
            done <<< "$result"
        fi
    done

    if [ "$VIOLATIONS" -eq 0 ]; then
        log_success "No lateral Engine/<Domain> include coupling detected"
    fi
}

check_ecs_system_direct_calls() {
    log_info "Checking ECS systems for direct sibling calls..."

    local ecs_systems="$PROJECT_ROOT/SparkEngine/Source/Engine/ECS/Systems"
    if [ ! -d "$ecs_systems" ]; then
        log_warning "ECS systems folder not found; skipping direct sibling call check"
        return
    fi

    local result
    result=$(spark_search '(GetSystem\(|->GetSystem\()' cpp "$ecs_systems")

    if [ -n "$result" ]; then
        while IFS= read -r line; do
            [ -n "$line" ] && record_violation "ECS system-to-system lookup detected: ${line#$PROJECT_ROOT/}"
        done <<< "$result"
    else
        log_success "No ECS .cpp direct GetSystem() sibling lookups detected"
    fi
}

check_module_abi_contract() {
    log_info "Checking module ABI exports and SDK version usage..."

    local module_mains
    module_mains=$(find "$PROJECT_ROOT/GameModules" -path '*/Source/Core/Main.cpp' -type f | sort)

    if [ -z "$module_mains" ]; then
        log_warning "No module Main.cpp files found; skipping ABI contract check"
        return
    fi

    local file
    while IFS= read -r file; do
        [ -z "$file" ] && continue
        local rel="${file#$PROJECT_ROOT/}"

        if ! spark_file_matches 'CreateModule[[:space:]]*\(|SPARK_IMPLEMENT_MODULE[[:space:]]*\(' "$file"; then
            record_violation "Missing CreateModule export (or SPARK_IMPLEMENT_MODULE) in $rel"
        fi
        if ! spark_file_matches 'DestroyModule[[:space:]]*\(|SPARK_IMPLEMENT_MODULE[[:space:]]*\(' "$file"; then
            record_violation "Missing DestroyModule export (or SPARK_IMPLEMENT_MODULE) in $rel"
        fi
        if ! spark_file_matches 'SPARK_SDK_VERSION' "$file"; then
            record_violation "Missing SPARK_SDK_VERSION usage in $rel"
        fi
    done <<< "$module_mains"

    if [ "$VIOLATIONS" -eq 0 ]; then
        log_success "Module ABI exports and SDK version assignments look correct"
    fi
}

log_info "Running cross-utilization architectural checks..."

check_game_modules_boundary
check_deprecated_globals
check_subsystem_lateral_includes
check_ecs_system_direct_calls
check_module_abi_contract

if [ "$VIOLATIONS" -eq 0 ]; then
    log_success "All cross-utilization checks passed"
    exit 0
fi

log_error "$VIOLATIONS architectural violation(s) detected"
exit 1
