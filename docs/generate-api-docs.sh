#!/bin/bash

# SparkEngine API Documentation Generator
# Replaces Doxygen — parses C++ headers and generates markdown API reference
# Works in any environment (no doxygen/graphviz required)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
OUTPUT_DIR="$SCRIPT_DIR/api"

# Source directories to scan
SOURCE_DIRS=(
    "$PROJECT_ROOT/SparkEngine/Source"
    "$PROJECT_ROOT/SparkEditor/Source"
    "$PROJECT_ROOT/SparkConsole/src"
    "$PROJECT_ROOT/SparkShaderCompiler/src"
    "$PROJECT_ROOT/GameModules/SparkGame/Source"
    "$PROJECT_ROOT/GameModules/SparkGameMMO/Source"
    "$PROJECT_ROOT/SparkSDK"
)

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info()    { echo -e "${BLUE}[API-DOC]${NC} $1"; }
log_success() { echo -e "${GREEN}[API-DOC]${NC} $1"; }
log_warning() { echo -e "${YELLOW}[API-DOC]${NC} $1"; }
log_error()   { echo -e "${RED}[API-DOC]${NC} $1"; }

# ============================================================================
# Parse a single header file and extract documentation
# ============================================================================
parse_header() {
    local file="$1"
    local rel_path="${file#$PROJECT_ROOT/}"
    local basename=$(basename "$file" | sed 's/\.[^.]*$//')

    # Extract @file, @brief from the top-of-file doc comment
    local file_brief=""
    file_brief=$(sed -n '/\/\*\*/,/\*\//{ s/.*@brief \(.*\)/\1/p; }' "$file" | head -1)

    # Extract classes/structs with their doc comments
    local classes=""
    classes=$(grep -n '^\s*\(class\|struct\)\s\+[A-Z]' "$file" 2>/dev/null | grep -v '^\s*//' | grep -v 'forward' || true)

    # Extract enums
    local enums=""
    enums=$(grep -n '^\s*enum\s\+\(class\s\+\)\?[A-Z]' "$file" 2>/dev/null || true)

    # Extract free functions (non-member, non-inline-in-class)
    local functions=""
    functions=$(grep -nE '^\s*(static\s+|inline\s+|constexpr\s+)*(void|bool|int|float|double|size_t|uint32_t|std::string|auto|HRESULT|DirectX::\w+)\s+[A-Z][a-zA-Z0-9_]+\s*\(' "$file" 2>/dev/null | head -30 || true)

    # Skip files with nothing interesting
    if [ -z "$classes" ] && [ -z "$enums" ] && [ -z "$functions" ]; then
        return
    fi

    local outfile="$OUTPUT_DIR/${rel_path%.h}.md"
    outfile="${outfile%.hpp}.md"
    mkdir -p "$(dirname "$outfile")"

    {
        echo "# \`$rel_path\`"
        echo ""
        if [ -n "$file_brief" ]; then
            echo "> $file_brief"
            echo ""
        fi
        echo "---"
        echo ""

        # --- Classes/Structs ---
        if [ -n "$classes" ]; then
            echo "## Classes & Structs"
            echo ""
            echo "$classes" | while IFS=: read -r line_num line_content; do
                # Clean up the class/struct name
                local name
                name=$(echo "$line_content" | sed 's/.*\(class\|struct\)\s\+//' | sed 's/[:{; ].*//' | tr -d ' ')
                [ -z "$name" ] && continue

                # Look for @brief above this line
                local brief=""
                if [ "$line_num" -gt 1 ]; then
                    local start=$((line_num - 15))
                    [ "$start" -lt 1 ] && start=1
                    brief=$(sed -n "${start},${line_num}p" "$file" | sed -n '/@brief/{s/.*@brief \(.*\)/\1/;p;}' | tail -1)
                fi

                # Check for base class
                local base=""
                base=$(echo "$line_content" | grep -oP ':\s*public\s+\K[A-Za-z_:]+' || true)

                echo "### \`$name\`"
                [ -n "$base" ] && echo "*Inherits from* \`$base\`"
                [ -n "$brief" ] && echo "" && echo "$brief"
                echo ""

                # Extract public methods for this class (simplified: scan next ~200 lines)
                local end_line=$((line_num + 200))
                local total_lines
                total_lines=$(wc -l < "$file")
                [ "$end_line" -gt "$total_lines" ] && end_line=$total_lines

                local methods
                methods=$(sed -n "${line_num},${end_line}p" "$file" | \
                    grep -nE '^\s*(virtual\s+|static\s+|inline\s+|constexpr\s+)*(void|bool|int|float|double|size_t|uint32_t|const\s|std::|DirectX::|auto|HRESULT|[A-Z][a-zA-Z0-9_]*\*?)\s+[A-Za-z_][A-Za-z0-9_]*\s*\(' | \
                    grep -v '^\s*//' | head -25 || true)

                local members
                members=$(sed -n "${line_num},${end_line}p" "$file" | \
                    grep -E '^\s+(DirectX::|float|int|bool|uint32_t|std::|size_t|BlendMode|BodyType2D|ColliderShape2D)\s+m_[a-zA-Z]' | head -20 || true)

                if [ -n "$methods" ]; then
                    echo "| Method | Signature |"
                    echo "|--------|-----------|"
                    echo "$methods" | while IFS=: read -r _ mline; do
                        # Extract return type + name + params
                        local sig
                        sig=$(echo "$mline" | sed 's/^\s*//' | sed 's/{.*//' | sed 's/\s*$//' | sed 's/;$//')
                        local mname
                        mname=$(echo "$sig" | grep -oP '[A-Za-z_][A-Za-z0-9_]*\s*\(' | head -1 | sed 's/\s*($//')
                        [ -z "$mname" ] && continue
                        echo "| \`$mname\` | \`$sig\` |"
                    done
                    echo ""
                fi

                if [ -n "$members" ]; then
                    echo "<details><summary>Member Variables</summary>"
                    echo ""
                    echo "\`\`\`cpp"
                    echo "$members" | sed 's/^\s*//'
                    echo "\`\`\`"
                    echo "</details>"
                    echo ""
                fi
            done
        fi

        # --- Enums ---
        if [ -n "$enums" ]; then
            echo "## Enums"
            echo ""
            echo "$enums" | while IFS=: read -r line_num line_content; do
                local ename
                ename=$(echo "$line_content" | sed 's/.*enum\s\+\(class\s\+\)\?//' | sed 's/[:{; ].*//' | tr -d ' ')
                [ -z "$ename" ] && continue

                echo "### \`$ename\`"
                echo ""

                # Extract enum values (scan next ~30 lines)
                local end_line=$((line_num + 30))
                local total_lines
                total_lines=$(wc -l < "$file")
                [ "$end_line" -gt "$total_lines" ] && end_line=$total_lines

                local values
                values=$(sed -n "${line_num},${end_line}p" "$file" | \
                    grep -E '^\s+[A-Z][A-Za-z0-9_]*' | \
                    sed 's/^\s*//' | sed 's/\/\/\/<\?\s*/— /' | head -20 || true)

                if [ -n "$values" ]; then
                    echo "\`\`\`"
                    echo "$values"
                    echo "\`\`\`"
                    echo ""
                fi
            done
        fi

    } > "$outfile"
}

# ============================================================================
# Generate index page
# ============================================================================
generate_index() {
    local index_file="$OUTPUT_DIR/README.md"

    {
        echo "# SparkEngine API Reference"
        echo ""
        echo "> Auto-generated from source headers on $(date '+%Y-%m-%d %H:%M:%S')"
        echo ">"
        echo "> **Generator:** \`docs/generate-api-docs.sh\` (no Doxygen required)"
        echo ""
        echo "---"
        echo ""

        # Group by top-level directory
        local current_group=""

        find "$OUTPUT_DIR" -name '*.md' ! -name 'README.md' | sort | while IFS= read -r mdfile; do
            local rel="${mdfile#$OUTPUT_DIR/}"
            local group
            group=$(echo "$rel" | cut -d'/' -f1)

            if [ "$group" != "$current_group" ]; then
                current_group="$group"
                echo ""
                echo "## $group"
                echo ""
            fi

            # Get the first heading from the file
            local title
            title=$(head -1 "$mdfile" | sed 's/^# //')

            echo "- [$title]($rel)"
        done

        echo ""
        echo "---"
        echo ""
        echo "## Statistics"
        echo ""
        local header_count=0
        local class_count=0
        local enum_count=0
        for dir in "${SOURCE_DIRS[@]}"; do
            [ -d "$dir" ] || continue
            header_count=$((header_count + $(find "$dir" -name '*.h' -o -name '*.hpp' | wc -l)))
        done
        class_count=$(grep -rl '### `' "$OUTPUT_DIR" 2>/dev/null | wc -l || echo 0)
        local page_count
        page_count=$(find "$OUTPUT_DIR" -name '*.md' ! -name 'README.md' | wc -l)

        echo "| Metric | Count |"
        echo "|--------|-------|"
        echo "| Headers scanned | $header_count |"
        echo "| API pages generated | $page_count |"
        echo "| Last generated | $(date '+%Y-%m-%d %H:%M:%S') |"

    } > "$index_file"
}

# ============================================================================
# Generate component quick-reference
# ============================================================================
generate_component_index() {
    local outfile="$OUTPUT_DIR/ComponentIndex.md"

    {
        echo "# Component Quick Reference"
        echo ""
        echo "> All ECS components available in SparkEngine"
        echo ""
        echo "---"
        echo ""

        # Scan all *Components.h files
        find "$PROJECT_ROOT/SparkEngine/Source" -name '*Components.h' | sort | while IFS= read -r cfile; do
            local rel="${cfile#$PROJECT_ROOT/}"
            echo "## \`$rel\`"
            echo ""

            # Extract struct names that look like components
            grep -E '^\s*struct\s+[A-Z][A-Za-z0-9]+' "$cfile" | while read -r sline; do
                local sname
                sname=$(echo "$sline" | sed 's/.*struct\s\+//' | sed 's/[:{; ].*//' | tr -d ' ')
                [ -z "$sname" ] && continue

                # Get @brief
                local sbrief
                sbrief=$(grep -B10 "struct\s\+$sname" "$cfile" | sed -n '/@brief/{s/.*@brief \(.*\)/\1/;p;}' | tail -1)

                if [ -n "$sbrief" ]; then
                    echo "- **\`$sname\`** — $sbrief"
                else
                    echo "- **\`$sname\`**"
                fi
            done
            echo ""
        done

    } > "$outfile"
}

# ============================================================================
# Generate system quick-reference
# ============================================================================
generate_system_index() {
    local outfile="$OUTPUT_DIR/SystemIndex.md"

    {
        echo "# System Quick Reference"
        echo ""
        echo "> All ECS systems in SparkEngine"
        echo ""
        echo "---"
        echo ""

        find "$PROJECT_ROOT/SparkEngine/Source" -name '*Systems*.h' -o -name '*System.h' | sort | while read -r sfile; do
            local rel="${sfile#$PROJECT_ROOT/}"

            # Extract class names ending in System
            local systems
            systems=$(grep -E '^\s*class\s+[A-Z][A-Za-z0-9]*System' "$sfile" || true)
            [ -z "$systems" ] && continue

            echo "## \`$rel\`"
            echo ""

            echo "$systems" | while read -r sline; do
                local sname
                sname=$(echo "$sline" | sed 's/.*class\s\+//' | sed 's/[:{; ].*//' | tr -d ' ')
                [ -z "$sname" ] && continue

                local sbrief
                sbrief=$(grep -B10 "class\s\+$sname" "$sfile" | sed -n '/@brief/{s/.*@brief \(.*\)/\1/;p;}' | tail -1)

                if [ -n "$sbrief" ]; then
                    echo "- **\`$sname\`** — $sbrief"
                else
                    echo "- **\`$sname\`**"
                fi
            done
            echo ""
        done

    } > "$outfile"
}

# ============================================================================
# Main
# ============================================================================
main() {
    local command="${1:-generate}"

    case "$command" in
        generate|full)
            log_info "Generating API documentation from headers..."
            rm -rf "$OUTPUT_DIR"
            mkdir -p "$OUTPUT_DIR"

            local tmplist
            tmplist=$(mktemp)
            for dir in "${SOURCE_DIRS[@]}"; do
                [ -d "$dir" ] || continue
                find "$dir" -type f \( -name '*.h' -o -name '*.hpp' \) >> "$tmplist"
            done
            local count=0
            while IFS= read -r hfile; do
                [ -z "$hfile" ] && continue
                parse_header "$hfile"
                count=$((count + 1))
            done < "$tmplist"
            rm -f "$tmplist"

            log_info "Parsed $count header files"

            generate_component_index
            log_success "Component index generated"

            generate_system_index
            log_success "System index generated"

            generate_index
            log_success "API index generated"

            local page_count
            page_count=$(find "$OUTPUT_DIR" -name '*.md' | wc -l)
            log_success "Generated $page_count markdown pages in docs/api/"
            ;;

        check)
            # Check if docs are stale (headers newer than generated docs)
            local checksum_file="$SCRIPT_DIR/.api_checksums"
            local current=""
            for dir in "${SOURCE_DIRS[@]}"; do
                [ -d "$dir" ] || continue
                current="$current$(find "$dir" -type f \( -name '*.h' -o -name '*.hpp' \) -exec md5sum {} \; 2>/dev/null | sort)"
            done

            if [ -f "$checksum_file" ]; then
                local previous
                previous=$(cat "$checksum_file")
                if [ "$current" = "$previous" ]; then
                    log_info "API docs are up to date"
                    exit 0
                fi
            fi

            log_warning "API docs are stale — regenerating..."
            echo "$current" > "$checksum_file"
            main generate
            ;;

        status)
            if [ -d "$OUTPUT_DIR" ]; then
                local page_count
                page_count=$(find "$OUTPUT_DIR" -name '*.md' | wc -l)
                log_info "API docs: $page_count pages in docs/api/"
                if [ -f "$OUTPUT_DIR/README.md" ]; then
                    log_info "Last generated: $(stat -c %y "$OUTPUT_DIR/README.md" 2>/dev/null || stat -f %Sm "$OUTPUT_DIR/README.md" 2>/dev/null || echo "unknown")"
                fi
            else
                log_warning "No API docs found. Run: docs/generate-api-docs.sh generate"
            fi
            ;;

        help|-h|--help)
            echo "SparkEngine API Documentation Generator"
            echo ""
            echo "Usage: $0 [generate|check|status|help]"
            echo ""
            echo "  generate  Parse all headers and generate markdown API reference (default)"
            echo "  check     Regenerate only if source headers have changed"
            echo "  status    Show doc generation status"
            echo "  help      Show this message"
            ;;

        *)
            log_error "Unknown command: $command"
            main help
            exit 1
            ;;
    esac
}

main "$@"
