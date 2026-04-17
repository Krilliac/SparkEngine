#!/bin/bash

# SparkEngine API Documentation Generator
# Replaces Doxygen — parses C++ headers and generates markdown API reference
# Works in any environment (no doxygen/graphviz required)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
OUTPUT_DIR="$SCRIPT_DIR/api"
SYMBOLS_TSV="$OUTPUT_DIR/.symbols.tsv"

# Source directories to scan (auto-discovers every GameModules/*/Source)
SOURCE_DIRS=(
    "$PROJECT_ROOT/SparkEngine/Source"
    "$PROJECT_ROOT/SparkEditor/Source"
    "$PROJECT_ROOT/SparkConsole/src"
    "$PROJECT_ROOT/SparkShaderCompiler/src"
    "$PROJECT_ROOT/SparkSDK"
    "$PROJECT_ROOT/Tests"
)
for _gm_src in "$PROJECT_ROOT"/GameModules/*/Source; do
    [ -d "$_gm_src" ] && SOURCE_DIRS+=("$_gm_src")
done
unset _gm_src

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
# Compute "../" prefix from an output .md path back to PROJECT_ROOT.
# E.g. docs/api/SparkEngine/Source/X/Y.md -> "../../../../../"
# ============================================================================
rel_prefix_for() {
    local md_path="$1"
    local rel="${md_path#$PROJECT_ROOT/}"
    local depth
    depth=$(awk -F/ '{print NF-1}' <<< "$rel")
    local prefix=""
    local i
    for (( i=0; i<depth; i++ )); do
        prefix+="../"
    done
    printf '%s' "$prefix"
}

# ============================================================================
# Emit one row to the symbol index TSV.
# Fields: kind  name  rel_path  line  brief
# ============================================================================
emit_symbol() {
    local kind="$1" name="$2" rel_path="$3" line="$4" brief="$5"
    # Sanitize tabs/newlines in brief
    brief=$(printf '%s' "$brief" | tr '\t\n' '  ')
    printf '%s\t%s\t%s\t%s\t%s\n' "$kind" "$name" "$rel_path" "$line" "$brief" >> "$SYMBOLS_TSV"
}

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

    # Extract #define macros (exclude header guards that end in _H/_HPP/_GUARD/_INCLUDED)
    local macros=""
    macros=$(grep -nE '^\s*#\s*define\s+[A-Z][A-Z0-9_]+' "$file" 2>/dev/null | \
        grep -vE '_(H|HPP|GUARD|INCLUDED)\s*$' || true)

    # Extract typedef / using aliases
    local aliases=""
    aliases=$(grep -nE '^\s*(typedef\s+.+\s+[A-Za-z_][A-Za-z0-9_]*\s*;|using\s+[A-Za-z_][A-Za-z0-9_]*\s*=)' "$file" 2>/dev/null || true)

    # Skip files with nothing interesting
    if [ -z "$classes" ] && [ -z "$enums" ] && [ -z "$functions" ] && [ -z "$macros" ] && [ -z "$aliases" ]; then
        return
    fi

    local stem="${rel_path%.h}"
    stem="${stem%.hpp}"
    local outfile="$OUTPUT_DIR/${stem}.md"
    mkdir -p "$(dirname "$outfile")"

    # Relative "../../.." prefix from this .md file back to project root, for clickable source links.
    local rp
    rp=$(rel_prefix_for "$outfile")

    # Single awk pass: walks the file once, buffers section content in arrays,
    # and emits grouped Markdown + TSV at END. One awk fork per file — avoids
    # the O(N·M) subshell explosion of the original per-class shell logic.
    awk -v rel="$rel_path" -v rp="$rp" -v outfile="$outfile" -v tsv="$SYMBOLS_TSV" -v file_brief="$file_brief" '
        BEGIN { last_brief = ""; nc = 0; ne = 0; nf = 0; nm = 0; na = 0 }

        /@brief/ {
            b = $0
            sub(/.*@brief[[:space:]]+/, "", b)
            sub(/[[:space:]]*\*\/[[:space:]]*$/, "", b)
            sub(/^[[:space:]]+/, "", b)
            sub(/[[:space:]]+$/, "", b)
            last_brief = b
        }

        /^[[:space:]]*(class|struct)[[:space:]]+[A-Z]/ && !/\/\// && !/forward/ {
            line_content = $0
            kind = (match(line_content, /^[[:space:]]*class/) ? "class" : "struct")
            tmp = line_content
            sub(/^[[:space:]]*(class|struct)[[:space:]]+/, "", tmp)
            if (match(tmp, /[A-Za-z_][A-Za-z0-9_]*/)) {
                name = substr(tmp, RSTART, RLENGTH)
            } else { next }
            base = ""
            if (match(line_content, /:[[:space:]]*public[[:space:]]+[A-Za-z_:][A-Za-z0-9_:]*/)) {
                base = substr(line_content, RSTART, RLENGTH)
                sub(/:[[:space:]]*public[[:space:]]+/, "", base)
            }
            entry = "### `" name "` <sup>[source](" rp rel "#L" NR ")</sup>\n"
            if (base != "") entry = entry "*Inherits from* `" base "`\n"
            if (last_brief != "") entry = entry "\n" last_brief "\n"
            classes_buf[++nc] = entry
            print kind "\t" name "\t" rel "\t" NR "\t" last_brief >> tsv
            last_brief = ""
            next
        }

        /^[[:space:]]*enum([[:space:]]+class)?[[:space:]]+[A-Z]/ {
            tmp = $0
            sub(/^[[:space:]]*enum([[:space:]]+class)?[[:space:]]+/, "", tmp)
            if (match(tmp, /[A-Za-z_][A-Za-z0-9_]*/)) {
                name = substr(tmp, RSTART, RLENGTH)
            } else { next }
            entry = "### `" name "` <sup>[source](" rp rel "#L" NR ")</sup>\n"
            if (last_brief != "") entry = entry "\n" last_brief "\n"
            enums_buf[++ne] = entry
            print "enum\t" name "\t" rel "\t" NR "\t" last_brief >> tsv
            last_brief = ""
            next
        }

        /^[[:space:]]*(static[[:space:]]+|inline[[:space:]]+|constexpr[[:space:]]+)*(void|bool|int|float|double|size_t|uint32_t|std::string|auto|HRESULT|DirectX::[A-Za-z_][A-Za-z0-9_]*)[[:space:]]+[A-Z][A-Za-z0-9_]*[[:space:]]*\(/ {
            sig = $0
            sub(/^[[:space:]]+/, "", sig)
            sub(/[[:space:]]*\{.*$/, "", sig)
            sub(/;[[:space:]]*$/, "", sig)
            md_sig = sig; gsub(/\|/, "\\|", md_sig)
            if (match(sig, /[A-Z][A-Za-z0-9_]*[[:space:]]*\(/)) {
                name = substr(sig, RSTART, RLENGTH)
                sub(/[[:space:]]*\($/, "", name)
            } else { next }
            funcs_buf[++nf] = "| `" name "` | [L" NR "](" rp rel "#L" NR ") | `" md_sig "` |"
            print "function\t" name "\t" rel "\t" NR "\t" >> tsv
            last_brief = ""
        }

        /^[[:space:]]*#[[:space:]]*define[[:space:]]+[A-Z][A-Z0-9_]+/ {
            if (match($0, /#[[:space:]]*define[[:space:]]+[A-Z][A-Z0-9_]+/)) {
                tok = substr($0, RSTART, RLENGTH)
                sub(/^#[[:space:]]*define[[:space:]]+/, "", tok)
                if (tok ~ /_(H|HPP|GUARD|INCLUDED)$/) next
                macros_buf[++nm] = "| `" tok "` | [L" NR "](" rp rel "#L" NR ") |"
                print "macro\t" tok "\t" rel "\t" NR "\t" >> tsv
            }
        }

        /^[[:space:]]*using[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]*=/ {
            if (match($0, /using[[:space:]]+[A-Za-z_][A-Za-z0-9_]*/)) {
                tok = substr($0, RSTART, RLENGTH)
                sub(/^using[[:space:]]+/, "", tok)
                aliases_buf[++na] = "| `" tok "` | [L" NR "](" rp rel "#L" NR ") |"
                print "alias\t" tok "\t" rel "\t" NR "\t" >> tsv
            }
        }

        /^[[:space:]]*typedef[[:space:]].+[[:space:]][A-Za-z_][A-Za-z0-9_]*[[:space:]]*;/ {
            tmp = $0
            sub(/;[[:space:]]*$/, "", tmp)
            if (match(tmp, /[A-Za-z_][A-Za-z0-9_]*[[:space:]]*$/)) {
                tok = substr(tmp, RSTART, RLENGTH)
                sub(/[[:space:]]+$/, "", tok)
                aliases_buf[++na] = "| `" tok "` | [L" NR "](" rp rel "#L" NR ") |"
                print "alias\t" tok "\t" rel "\t" NR "\t" >> tsv
            }
        }

        END {
            # Count "../" hops from outfile back to docs/api (outfile is under docs/api/<subpath>/<stem>.md)
            # rp already gives us the prefix from outfile back to project root.
            # To reach docs/api/README.md from here, we need rp + "docs/api/README.md".
            api_root = rp "docs/api/README.md"
            docs_root = rp "docs/README.md"

            # Header
            print "# `" rel "`" > outfile
            print ""               > outfile
            if (file_brief != "") {
                print "> " file_brief > outfile
                print ""             > outfile
            }
            print "[← API Reference](" api_root ") · [Documentation Index](" docs_root ") · [View source](" rp rel ")" > outfile
            print ""                           > outfile
            print "---"                        > outfile
            print ""                           > outfile

            if (nc > 0) {
                print "## Classes & Structs" > outfile
                print ""                     > outfile
                for (i = 1; i <= nc; i++) { print classes_buf[i] > outfile }
            }
            if (ne > 0) {
                print "## Enums" > outfile
                print ""         > outfile
                for (i = 1; i <= ne; i++) { print enums_buf[i] > outfile }
            }
            if (nf > 0) {
                print "## Free Functions"                 > outfile
                print ""                                  > outfile
                print "| Function | Source | Signature |" > outfile
                print "|----------|--------|-----------|" > outfile
                for (i = 1; i <= nf; i++) { print funcs_buf[i] > outfile }
                print ""                                  > outfile
            }
            if (nm > 0) {
                print "## Macros"            > outfile
                print ""                     > outfile
                print "| Macro | Source |"   > outfile
                print "|-------|--------|"   > outfile
                for (i = 1; i <= nm; i++) { print macros_buf[i] > outfile }
                print ""                     > outfile
            }
            if (na > 0) {
                print "## Type Aliases"      > outfile
                print ""                     > outfile
                print "| Alias | Source |"   > outfile
                print "|-------|--------|"   > outfile
                for (i = 1; i <= na; i++) { print aliases_buf[i] > outfile }
                print ""                     > outfile
            }
        }
    ' "$file"
}

# ============================================================================
# Parse a .cpp implementation file for TSV records only (no markdown page).
# Extracts free functions and out-of-line method definitions with source lines.
#
# Uses a single awk pass per file (much faster than piping grep into a shell
# loop — one fork per file instead of per match).
# ============================================================================
parse_cpp() {
    local file="$1"
    local rel_path="${file#$PROJECT_ROOT/}"

    awk -v rel="$rel_path" -v tsv="$SYMBOLS_TSV" '
        # Skip comment-only and preprocessor lines
        /^[[:space:]]*\/\// { next }
        /^[[:space:]]*#/    { next }

        # Out-of-line method definition: the line must START with an identifier
        # (not whitespace) so we skip indented calls like "    Class::Method()".
        # Pattern: ReturnType[<...>][*&]... Class::Method(
        match($0, /^[A-Za-z_][A-Za-z0-9_:<>,\*\&[:space:]]*[A-Za-z_][A-Za-z0-9_]*::[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\(/) {
            # Extract the Class::Method portion (the identifier pair immediately before "(")
            if (match($0, /[A-Za-z_][A-Za-z0-9_]*::[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\(/)) {
                qn = substr($0, RSTART, RLENGTH)
                sub(/[[:space:]]*\($/, "", qn)
                print "method\t" qn "\t" rel "\t" NR "\t" >> tsv
            }
            next
        }

        # Free function at column 0: "Type[*&] Name(" — skip control-flow keywords.
        match($0, /^(static[[:space:]]+|inline[[:space:]]+|constexpr[[:space:]]+)?[A-Za-z_][A-Za-z0-9_:<>,\*\&[:space:]]*[[:space:]][A-Za-z_][A-Za-z0-9_]+[[:space:]]*\(/) {
            sig = substr($0, RSTART, RLENGTH)
            if (match(sig, /[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\([[:space:]]*$/)) {
                name_start = RSTART
                name = substr(sig, RSTART, RLENGTH)
                sub(/[[:space:]]*\([[:space:]]*$/, "", name)
                if (name == "if" || name == "for" || name == "while" || name == "switch" ||
                    name == "return" || name == "sizeof" || name == "static_cast" ||
                    name == "dynamic_cast" || name == "const_cast" || name == "reinterpret_cast" ||
                    name == "decltype" || name == "typeid" || name == "noexcept" || name == "alignof") {
                    next
                }
                # Skip out-of-line method definitions (Class::Method). Check only
                # whether the matched name is preceded by "::" — not whether "::"
                # appears anywhere on the line, which would reject valid free
                # functions with namespace-qualified return types like
                # `std::unique_ptr<X> MakeFoo(`.
                if (name_start > 2 && substr(sig, name_start - 2, 2) == "::") next
                print "function\t" name "\t" rel "\t" NR "\t" >> tsv
            }
        }
    ' "$file"
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
        echo "[← Back to Documentation Index](../README.md) · [ECS Components](ComponentIndex.md) · [ECS Systems](SystemIndex.md)"
        echo ""
        echo "---"
        echo ""
        echo "## Per-module pages"
        echo ""
        echo "Each entry is a per-header API page with clickable source-line anchors back into the codebase. Pages are grouped by the top-level module directory."
        echo ""

        # Group by top-level directory (files directly under docs/api/ without a
        # subdirectory — like ComponentIndex.md, SystemIndex.md — are listed first
        # under an "Overview" section instead of becoming their own group headers).
        local current_group=""

        echo "## Overview"
        echo ""
        find "$OUTPUT_DIR" -maxdepth 1 -name '*.md' ! -name 'README.md' | sort | while IFS= read -r mdfile; do
            local rel="${mdfile#$OUTPUT_DIR/}"
            local title
            title=$(head -1 "$mdfile" | sed 's/^# //')
            echo "- [$title]($rel)"
        done
        echo ""

        find "$OUTPUT_DIR" -mindepth 2 -name '*.md' | sort | while IFS= read -r mdfile; do
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
        echo "[← API Reference](README.md) · [ECS Systems](SystemIndex.md) · [Documentation Index](../README.md)"
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
        echo "[← API Reference](README.md) · [ECS Components](ComponentIndex.md) · [Documentation Index](../README.md)"
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
            log_info "Generating API documentation from headers and sources..."
            rm -rf "$OUTPUT_DIR"
            mkdir -p "$OUTPUT_DIR"
            : > "$SYMBOLS_TSV"

            local tmplist cpplist
            tmplist=$(mktemp)
            cpplist=$(mktemp)
            for dir in "${SOURCE_DIRS[@]}"; do
                [ -d "$dir" ] || continue
                find "$dir" -type f \( -name '*.h' -o -name '*.hpp' \) >> "$tmplist"
                find "$dir" -type f -name '*.cpp' >> "$cpplist"
            done

            local count=0
            while IFS= read -r hfile; do
                [ -z "$hfile" ] && continue
                parse_header "$hfile"
                count=$((count + 1))
            done < "$tmplist"
            log_info "Parsed $count header files"

            local cpp_count=0
            while IFS= read -r cppfile; do
                [ -z "$cppfile" ] && continue
                parse_cpp "$cppfile"
                cpp_count=$((cpp_count + 1))
            done < "$cpplist"
            log_info "Scanned $cpp_count .cpp files for symbols"

            rm -f "$tmplist" "$cpplist"

            generate_component_index
            log_success "Component index generated"

            generate_system_index
            log_success "System index generated"

            generate_index
            log_success "API index generated"

            local page_count sym_count
            page_count=$(find "$OUTPUT_DIR" -name '*.md' | wc -l)
            sym_count=$(wc -l < "$SYMBOLS_TSV" 2>/dev/null || echo 0)
            log_success "Generated $page_count markdown pages and $sym_count symbol records in docs/api/"
            ;;

        check)
            # Check if docs are stale (sources newer than generated docs)
            local checksum_file="$SCRIPT_DIR/.api_checksums"
            local current=""
            for dir in "${SOURCE_DIRS[@]}"; do
                [ -d "$dir" ] || continue
                current="$current$(find "$dir" -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' \) -exec md5sum {} \; 2>/dev/null | sort)"
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
            echo "  generate  Parse all headers + .cpp files and generate markdown API reference (default)"
            echo "            Writes per-header pages and docs/api/.symbols.tsv (symbol index)"
            echo "  check     Regenerate only if any source file (.h/.hpp/.cpp) has changed"
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
