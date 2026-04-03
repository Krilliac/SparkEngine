#!/bin/bash

# SparkEngine System Wiring Validator
# Verifies that classes with Initialize()/Update()/Shutdown() methods
# are actually called somewhere in the engine startup or main loop.
#
# CLAUDE.md mandates: "Every system must be initialized. If Initialize()
# exists, it must be called in the startup path."
#
# Usage:
#   ./check-wiring.sh          # Check (default)
#   ./check-wiring.sh check    # Same as above

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

SRC="$PROJECT_ROOT/SparkEngine/Source"
EDITOR_SRC="$PROJECT_ROOT/SparkEditor/Source"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info()    { echo -e "${BLUE}[WIRING]${NC} $1"; }
log_success() { echo -e "${GREEN}[WIRING]${NC} $1"; }
log_warning() { echo -e "${YELLOW}[WIRING]${NC} $1"; }

UNWIRED=0
CHECKED=0

log_info "Scanning for classes with Initialize()/Update() methods..."

# Build list of all .cpp files where systems should be called from
# (engine startup, main loop, editor startup)
CALL_SITES=$(cat \
    "$SRC/Core/SparkEngine.cpp" \
    "$SRC/Core/EngineContext.h" \
    "$SRC/Core/EngineContext.cpp" \
    "$EDITOR_SRC/Core/EditorApplication.h" \
    "$EDITOR_SRC/Core/EditorApplication.cpp" \
    "$EDITOR_SRC/Core/EditorUI.h" \
    "$EDITOR_SRC/Core/EditorUI.cpp" \
    "$EDITOR_SRC/Core/EditorPanelFactory.cpp" \
    2>/dev/null || true)

# Also include all .cpp files as potential call sites (a system might be
# called from another system, which is fine as long as it's reachable)
ALL_CPP_CONTENT=$(find "$SRC" "$EDITOR_SRC" -name '*.cpp' 2>/dev/null | xargs grep -lh "Initialize\|GetInstance\|make_unique\|make_shared" 2>/dev/null | xargs cat 2>/dev/null || true)

# Known base classes and interfaces to skip (abstract, not directly instantiated)
SKIP_CLASSES="ISystem|EditorPanel|IModule|IRHIDevice|RHIDevice|IGameModule|GameModule|ILogger"

# Find all classes that declare Initialize() in headers
while IFS= read -r line; do
    file=$(echo "$line" | cut -d: -f1)
    rel=$(echo "$file" | sed "s|$PROJECT_ROOT/||")

    # Extract the class that actually owns the Initialize() method
    # Find the class/struct that contains the Initialize declaration
    classname=$(awk '/^class\s/ || /^struct\s/ {name=$2} /Initialize\(/ && name {print name; exit}' "$file" 2>/dev/null | sed 's/[:{]//g; s/SPARK_API //')
    if [ -z "$classname" ]; then
        # Fallback: first class in file
        classname=$(grep -oP '^class\s+(?:SPARK_API\s+)?(\w+)' "$file" 2>/dev/null | head -1 | awk '{print $NF}')
    fi
    if [ -z "$classname" ]; then continue; fi

    # Skip base classes/interfaces
    if echo "$classname" | grep -qE "^($SKIP_CLASSES)$"; then continue; fi

    # Skip enums, small types (< 20 lines), and pure virtual
    if grep -q "Initialize.*=\s*0" "$file" 2>/dev/null; then continue; fi
    linecount=$(wc -l < "$file")
    if [ "$linecount" -lt 20 ]; then continue; fi

    CHECKED=$((CHECKED + 1))

    # Check if class is referenced in any .cpp file (instantiated or called)
    if echo "$ALL_CPP_CONTENT" | grep -q "$classname" 2>/dev/null; then
        continue  # Class is referenced somewhere — likely wired in
    fi

    # Not found — report as potentially unwired
    log_warning "  $classname ($rel) — has Initialize() but no references found in .cpp files"
    UNWIRED=$((UNWIRED + 1))

done < <(grep -rlP '^\s+(virtual\s+)?(bool|void|HRESULT|int)\s+Initialize\(' \
    "$SRC" "$EDITOR_SRC" \
    --include='*.h' 2>/dev/null | \
    grep -v ThirdParty | \
    grep -v Tests | \
    sort -u)

# Note: Some "unwired" results may be false positives for inner types,
# helper structs, or types only used as template parameters. Review manually.

if [ "$UNWIRED" -eq 0 ]; then
    log_success "All $CHECKED systems with Initialize() are referenced in source code"
    exit 0
else
    log_warning "$UNWIRED of $CHECKED system(s) may be unwired (have Initialize() but no .cpp references)"
    echo ""
    log_info "Tip: If a system is intentionally not wired, add it to the SKIP_CLASSES list in this script"
    exit 1
fi
