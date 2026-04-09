#!/bin/bash

# SparkEngine Deprecated Submodule Guard
# Fails if banned/deprecated submodules are reintroduced in .gitmodules.
#
# Usage:
#   ./check-deprecated-submodules.sh
#   ./check-deprecated-submodules.sh check

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
GITMODULES_FILE="$PROJECT_ROOT/.gitmodules"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info()    { echo -e "${BLUE}[DEPRECATED-SUBMODULES]${NC} $1"; }
log_success() { echo -e "${GREEN}[DEPRECATED-SUBMODULES]${NC} $1"; }
log_warning() { echo -e "${YELLOW}[DEPRECATED-SUBMODULES]${NC} $1"; }

if [ ! -f "$GITMODULES_FILE" ]; then
    log_success ".gitmodules not present; no deprecated submodules found"
    exit 0
fi

# Paths that must never return.
readonly DEPRECATED_SUBMODULE_PATHS=(
    "ThirdParty/Physics/bullet3"
)

issues=0

log_info "Checking .gitmodules for deprecated submodule entries..."

for path in "${DEPRECATED_SUBMODULE_PATHS[@]}"; do
    if git config --file "$GITMODULES_FILE" --get-regexp '^submodule\..*\.path$' | awk '{print $2}' | grep -Fxq "$path"; then
        log_warning "Deprecated submodule detected: $path"
        issues=$((issues + 1))
    fi
done

if [ "$issues" -eq 0 ]; then
    log_success "No deprecated submodules detected"
    exit 0
fi

log_warning "$issues deprecated submodule entr$( [ "$issues" -eq 1 ] && echo 'y' || echo 'ies' ) found"
exit 1
