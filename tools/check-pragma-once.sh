#!/bin/bash

# SparkEngine Header Guard Enforcer
# Verifies all .h files use #pragma once (not #ifndef guards).
#
# Usage:
#   ./check-pragma-once.sh          # Check (default)
#   ./check-pragma-once.sh check    # Same as above

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info()    { echo -e "${BLUE}[PRAGMA]${NC} $1"; }
log_success() { echo -e "${GREEN}[PRAGMA]${NC} $1"; }
log_warning() { echo -e "${YELLOW}[PRAGMA]${NC} $1"; }
log_error()   { echo -e "${RED}[PRAGMA]${NC} $1"; }

VIOLATIONS=0

log_info "Scanning headers for #pragma once..."

while IFS= read -r file; do
    # Check entire file for #pragma once
    if ! grep -q '#pragma once' "$file"; then
        rel=$(echo "$file" | sed "s|$PROJECT_ROOT/||")
        # Check if it uses #ifndef guards instead
        if grep -q '#ifndef.*_H' "$file"; then
            log_error "  $rel — uses #ifndef guard (should be #pragma once)"
        else
            log_warning "  $rel — missing #pragma once"
        fi
        VIOLATIONS=$((VIOLATIONS + 1))
    fi
done < <(find \
    "$PROJECT_ROOT/SparkEngine/Source" \
    "$PROJECT_ROOT/SparkEditor/Source" \
    "$PROJECT_ROOT/SparkConsole/src" \
    "$PROJECT_ROOT/SparkShaderCompiler/src" \
    "$PROJECT_ROOT/GameModules" \
    "$PROJECT_ROOT/SparkSDK" \
    -name '*.h' -o -name '*.hpp' 2>/dev/null)

if [ "$VIOLATIONS" -eq 0 ]; then
    log_success "All headers use #pragma once"
    exit 0
else
    log_warning "$VIOLATIONS header(s) missing #pragma once"
    exit 1
fi
