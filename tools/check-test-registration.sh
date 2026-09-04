#!/bin/bash

# SparkEngine Test Registration Checker
# Verifies every Tests/Test*.cpp file is referenced in Tests/CMakeLists.txt.
#
# Usage:
#   ./check-test-registration.sh          # Exit 1 if unregistered tests found
#   ./check-test-registration.sh check    # Same as above (default)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
TESTS_DIR="$PROJECT_ROOT/Tests"
CMAKE_FILE="$TESTS_DIR/CMakeLists.txt"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info()    { echo -e "${BLUE}[TEST-REG]${NC} $1"; }
log_success() { echo -e "${GREEN}[TEST-REG]${NC} $1"; }
log_warning() { echo -e "${YELLOW}[TEST-REG]${NC} $1"; }

if [ ! -f "$CMAKE_FILE" ]; then
    log_warning "Tests/CMakeLists.txt not found"
    exit 1
fi

log_info "Checking test source registration in Tests/CMakeLists.txt..."

UNREGISTERED=0

while IFS= read -r -d '' test_file; do
    basename=$(basename "$test_file")
    rel=$(echo "$test_file" | sed "s|$PROJECT_ROOT/||")

    if ! grep -qF "$basename" "$CMAKE_FILE"; then
        echo -e "  ${YELLOW}✗${NC} $rel"
        UNREGISTERED=$((UNREGISTERED + 1))
    fi
done < <(find "$TESTS_DIR" -maxdepth 1 -name 'Test*.cpp' -print0 | sort -z)

# Also check Integration/ subdirectory
while IFS= read -r -d '' test_file; do
    basename="Integration/$(basename "$test_file")"
    rel=$(echo "$test_file" | sed "s|$PROJECT_ROOT/||")

    if ! grep -qF "$basename" "$CMAKE_FILE"; then
        echo -e "  ${YELLOW}✗${NC} $rel"
        UNREGISTERED=$((UNREGISTERED + 1))
    fi
done < <(find "$TESTS_DIR/Integration" -maxdepth 1 -name 'Test*.cpp' -print0 2>/dev/null | sort -z)

TOTAL=$(find "$TESTS_DIR" -maxdepth 1 -name 'Test*.cpp' | wc -l)
REGISTERED=$((TOTAL - UNREGISTERED))

if [ "$UNREGISTERED" -eq 0 ]; then
    log_success "All $TOTAL test sources registered"
    exit 0
else
    log_warning "$UNREGISTERED of $TOTAL test source(s) not found in CMakeLists.txt"
    exit 1
fi
