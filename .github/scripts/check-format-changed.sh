#!/usr/bin/env bash
# check-format-changed.sh -- the check-format CI gate (CI-100).
#
# Every C++ surface is in scope, but source checks are incremental so
# unrelated legacy formatting debt does not hide new debt. The style file
# itself is always validated and is part of change detection.
#
# Run from the repository root. Inputs:
#   FORMAT_BASE_SHA  optional commit to diff against (the pull-request base or
#                    the push's "before" SHA). A missing, malformed, all-zero,
#                    or unknown SHA falls back to HEAD^; with no HEAD^ either
#                    (a root commit), every tracked file in scope is checked.
#                    A failing git query fails the gate.
#   clang-format     resolved from PATH.
#
# clang-format output is written to check-format-output.log in the current
# directory so the workflow's failure-only summary step can extract it.
#
# Exit status: 0 when the style file parses and every changed C++ source is
# formatted; nonzero for a formatting violation, an unparsable or missing
# .clang-format, a missing source root, a missing clang-format, or any
# clang-format failure. .github/scripts/test-check-format-changed.sh exercises
# each of these paths against throwaway repositories.
set -euo pipefail

FORMAT_ROOTS=(SparkEngine/Source GameModules SparkEditor/Source SparkConsole/src SparkShaderCompiler/src SparkBuild/src SparkInstaller/src SparkDaemon/src SparkServer/src SparkGateway/src SparkCooker/src SparkWorker/src SparkAutomation/src SparkLauncher/src Tests)
for root in "${FORMAT_ROOTS[@]}"; do
    if [[ ! -d "$root" ]]; then
        echo "::error::Formatting source root is missing: $root"
        exit 1
    fi
done

BASE_SHA="${FORMAT_BASE_SHA:-}"
if [[ ! "$BASE_SHA" =~ ^[0-9a-fA-F]{40}$ ]] ||
    [[ "$BASE_SHA" == "0000000000000000000000000000000000000000" ]] ||
    ! git cat-file -e "${BASE_SHA}^{commit}" 2>/dev/null; then
    # --verify --quiet prints nothing for a root commit; plain rev-parse would
    # echo the literal "HEAD^" and make the diff below select nothing.
    BASE_SHA=$(git rev-parse --verify --quiet "HEAD^" 2>/dev/null || true)
fi

# Collect the selection through a file so a failing git command fails the
# gate instead of reading as "no changed sources".
CHANGED_PATHS_FILE=$(mktemp)
trap 'rm -f "$CHANGED_PATHS_FILE"' EXIT
if [[ -n "$BASE_SHA" ]]; then
    if ! git diff --name-only --diff-filter=ACMRD -z "$BASE_SHA" HEAD -- "${FORMAT_ROOTS[@]}" .clang-format \
        > "$CHANGED_PATHS_FILE"; then
        echo "::error::Could not list sources changed since ${BASE_SHA}."
        exit 1
    fi
elif ! git ls-files -z -- "${FORMAT_ROOTS[@]}" .clang-format > "$CHANGED_PATHS_FILE"; then
    echo "::error::Could not list tracked sources."
    exit 1
fi
mapfile -d '' CHANGED_PATHS < "$CHANGED_PATHS_FILE"

FORMAT_SOURCES=()
FORMAT_CONFIG_CHANGED=false
for source in "${CHANGED_PATHS[@]}"; do
    case "$source" in
        .clang-format) FORMAT_CONFIG_CHANGED=true ;;
        */Metal/*) ;;
        *.h|*.hpp|*.inl|*.cpp)
            if [[ -f "$source" ]]; then
                FORMAT_SOURCES+=("$source")
            fi
            ;;
    esac
done

if ! command -v clang-format >/dev/null 2>&1; then
    echo "::error::clang-format is not installed."
    exit 1
fi
if [[ ! -f .clang-format ]]; then
    echo "::error::.clang-format is missing."
    exit 1
fi
if ! clang-format --style=file --dump-config > /dev/null 2> check-format-output.log; then
    cat check-format-output.log
    echo "::error::.clang-format could not be parsed."
    exit 1
fi
if [[ "$FORMAT_CONFIG_CHANGED" == true ]]; then
    echo "Validated changed .clang-format configuration."
fi
if (( ${#FORMAT_SOURCES[@]} == 0 )); then
    echo "No changed C++ sources require formatting."
    exit 0
fi

set +e
clang-format --dry-run --Werror "${FORMAT_SOURCES[@]}" > check-format-output.log 2>&1
FORMAT_STATUS=$?
set -e
if (( FORMAT_STATUS != 0 )); then
    cat check-format-output.log
    if grep -q -- "-Wclang-format-violations" check-format-output.log; then
        echo "::error::Code formatting violations found. Run 'clang-format -i' on the listed files."
    else
        echo "::error::clang-format failed to parse or format the changed sources (exit ${FORMAT_STATUS})."
    fi
    exit "$FORMAT_STATUS"
fi
echo "Checked formatting of ${#FORMAT_SOURCES[@]} changed C++ source(s)."
