#!/bin/bash

# SparkEngine Doxygen Comment Coverage Checker
# Verifies public headers have @file/@brief documentation.
#
# Checks:
#   1. Each .h file has @file or @brief in the first 15 lines
#   2. Reports coverage percentage
#
# Usage:
#   ./check-doxygen-coverage.sh              # Report coverage
#   ./check-doxygen-coverage.sh check        # Exit 1 if below threshold
#   ./check-doxygen-coverage.sh --threshold 90  # Custom threshold (default: 95)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

THRESHOLD=95

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info()    { echo -e "${BLUE}[DOXYGEN]${NC} $1"; }
log_success() { echo -e "${GREEN}[DOXYGEN]${NC} $1"; }
log_warning() { echo -e "${YELLOW}[DOXYGEN]${NC} $1"; }

# Parse args
MODE="report"
while [ $# -gt 0 ]; do
    case "$1" in
        check)       MODE="check"; shift ;;
        --threshold) THRESHOLD="$2"; shift 2 ;;
        *)           shift ;;
    esac
done

TOTAL=0
DOCUMENTED=0
UNDOCUMENTED_FILES=""

log_info "Scanning headers for Doxygen documentation..."

SOURCE_DIRS=(
    "$PROJECT_ROOT/SparkEngine/Source"
    "$PROJECT_ROOT/SparkEditor/Source"
    "$PROJECT_ROOT/SparkConsole/src"
    "$PROJECT_ROOT/SparkShaderCompiler/src"
    "$PROJECT_ROOT/SparkSDK"
)

while IFS= read -r file; do
    TOTAL=$((TOTAL + 1))

    # Check first 30 lines for @file, @brief, or \brief (Doxygen blocks can be long)
    if head -30 "$file" | grep -qE '@file|@brief|\\brief|\\file'; then
        DOCUMENTED=$((DOCUMENTED + 1))
    else
        rel=$(echo "$file" | sed "s|$PROJECT_ROOT/||")
        UNDOCUMENTED_FILES="$UNDOCUMENTED_FILES\n  $rel"
    fi
done < <(find "${SOURCE_DIRS[@]}" -name '*.h' -o -name '*.hpp' 2>/dev/null | sort)

# Calculate percentage
if [ "$TOTAL" -gt 0 ]; then
    PERCENT=$((DOCUMENTED * 100 / TOTAL))
else
    PERCENT=100
fi

UNDOC_COUNT=$((TOTAL - DOCUMENTED))

echo ""
log_info "Coverage: $DOCUMENTED / $TOTAL headers ($PERCENT%)"

if [ "$UNDOC_COUNT" -gt 0 ]; then
    echo ""
    log_warning "$UNDOC_COUNT header(s) missing @file/@brief:"
    echo -e "$UNDOCUMENTED_FILES"
fi

echo ""
if [ "$PERCENT" -ge "$THRESHOLD" ]; then
    log_success "Doxygen coverage $PERCENT% meets threshold ($THRESHOLD%)"
    exit 0
else
    log_warning "Doxygen coverage $PERCENT% is below threshold ($THRESHOLD%)"
    if [ "$MODE" = "check" ]; then
        exit 1
    fi
    exit 0
fi
