#!/bin/bash

# SparkEngine Editor Panel Registration Validator
# Verifies all Panel headers in SparkEditor/Source/Panels/ are registered
# in EditorPanelFactory.cpp (included and instantiated).
#
# Usage:
#   ./check-editor-panels.sh          # Check (default)
#   ./check-editor-panels.sh check    # Same as above

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
PANELS_DIR="$PROJECT_ROOT/SparkEditor/Source/Panels"
FACTORY="$PROJECT_ROOT/SparkEditor/Source/Core/EditorPanelFactory.cpp"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info()    { echo -e "${BLUE}[PANELS]${NC} $1"; }
log_success() { echo -e "${GREEN}[PANELS]${NC} $1"; }
log_warning() { echo -e "${YELLOW}[PANELS]${NC} $1"; }
log_error()   { echo -e "${RED}[PANELS]${NC} $1"; }

ISSUES=0
TOTAL=0

if [ ! -d "$PANELS_DIR" ]; then
    log_warning "Panels directory not found: $PANELS_DIR"
    exit 1
fi

if [ ! -f "$FACTORY" ]; then
    log_warning "EditorPanelFactory.cpp not found: $FACTORY"
    exit 1
fi

log_info "Checking editor panel registration..."

# Read factory AND EditorUI source (some panels are instantiated directly in EditorUI)
factory_content=$(cat "$FACTORY" "$PROJECT_ROOT/SparkEditor/Source/Core/EditorUI.cpp" 2>/dev/null)

while IFS= read -r header; do
    TOTAL=$((TOTAL + 1))
    filename=$(basename "$header")
    classname="${filename%.h}"

    # Here-strings, not `echo | grep -q`: grep -q exits on the first match and closes
    # the pipe while echo is still writing the whole factory, which spams
    # "echo: write error: Broken pipe" (and fails the pipeline under pipefail).

    # Check 1: Is the header #included in the factory?
    if ! grep -qF "$filename" <<< "$factory_content"; then
        # Also check factory for the class name directly (some panels are included via different paths)
        if ! grep -qF "$classname" <<< "$factory_content"; then
            log_error "  $classname — NOT included in EditorPanelFactory.cpp"
            ISSUES=$((ISSUES + 1))
            continue
        fi
    fi

    # Check 2: Is the class instantiated (make_shared<ClassName>)?
    if ! grep -qF "make_shared<$classname>" <<< "$factory_content"; then
        # Some panels might use a different class name than the file name
        # Check if ANY make_shared references the header's class
        local_class=$(grep -oP 'class\s+(\w+Panel)' "$header" 2>/dev/null | head -1 | awk '{print $2}')
        if [ -n "$local_class" ] && grep -qF "make_shared<$local_class>" <<< "$factory_content"; then
            continue  # Found under a different class name
        fi
        log_warning "  $classname — included but NOT instantiated (no make_shared<$classname>)"
        ISSUES=$((ISSUES + 1))
    fi
done < <(find "$PANELS_DIR" -name '*Panel.h' 2>/dev/null | sort)

if [ "$ISSUES" -eq 0 ]; then
    log_success "All $TOTAL editor panels are registered in EditorPanelFactory.cpp"
    exit 0
else
    log_warning "$ISSUES of $TOTAL panel(s) have registration issues"
    exit 1
fi
