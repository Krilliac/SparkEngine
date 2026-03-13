#!/usr/bin/env bash
# update-sparkbuild.sh -- Download SparkBuild binaries
#
# By default fetches from the latest GitHub Release.
# Pass --artifacts to instead pull from the latest successful CI run.
#
# Usage:
#   ./tools/update-sparkbuild.sh                   (latest release)
#   ./tools/update-sparkbuild.sh v1.0.0            (specific release tag)
#   ./tools/update-sparkbuild.sh --artifacts       (latest CI build artifacts)
#   GH_TOKEN=<token> ./tools/update-sparkbuild.sh --artifacts

set -e

REPO="Krilliac/SparkBuild"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

MODE="release"
TAG="latest"

for arg in "$@"; do
    case "$arg" in
        --artifacts) MODE="artifacts" ;;
        *)           TAG="$arg" ;;
    esac
done

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

download() {
    local url="$1" dest="$2" label="${3:-$dest}"
    echo "    Downloading $label ..."
    if command -v curl &>/dev/null; then
        curl -fsSL ${GH_TOKEN:+-H "Authorization: Bearer $GH_TOKEN"} -o "$dest" "$url"
    elif command -v wget &>/dev/null; then
        wget -q -O "$dest" "$url"
    else
        echo "[-] Neither curl nor wget found." >&2
        return 1
    fi
}

api_get() {
    local url="$1"
    curl -sL \
        -H "Accept: application/vnd.github+json" \
        ${GH_TOKEN:+-H "Authorization: Bearer $GH_TOKEN"} \
        "$url"
}

make_executable() {
    local file="$1"
    [ -f "$file" ] && chmod +x "$file"
}

# ---------------------------------------------------------------------------
# Artifact download (latest successful CI run)
# ---------------------------------------------------------------------------

download_artifacts() {
    echo "[*] Fetching latest CI build artifacts from $REPO ..."
    echo

    local runs
    runs=$(api_get "https://api.github.com/repos/$REPO/actions/runs?status=success&per_page=1")
    local run_id
    run_id=$(echo "$runs" | grep -o '"id":[0-9]*' | head -1 | cut -d: -f2)

    if [ -z "$run_id" ]; then
        echo "[-] Could not find a successful CI run. Check GH_TOKEN or network." >&2
        exit 1
    fi
    echo "[*] Latest successful run: $run_id"
    echo

    local artifacts
    artifacts=$(api_get "https://api.github.com/repos/$REPO/actions/runs/$run_id/artifacts")

    # Artifact name -> local filename inside tools/
    declare -A ARTIFACT_MAP=(
        ["SparkBuild-Windows"]="SparkBuild.exe"
        ["SparkBuild-Linux-gcc"]="SparkBuild-linux-gcc"
        ["SparkBuild-Linux-clang"]="SparkBuild-linux-clang"
        ["SparkBuild-macOS"]="SparkBuild-macos"
    )

    local tmp
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT

    for artifact_name in "${!ARTIFACT_MAP[@]}"; do
        local local_name="${ARTIFACT_MAP[$artifact_name]}"
        local dest="$SCRIPT_DIR/$local_name"

        # Extract artifact id via simple grep (avoids jq dependency)
        local artifact_block
        artifact_block=$(echo "$artifacts" | tr '{}' '\n' | grep "\"name\":\"$artifact_name\"")
        local artifact_id
        artifact_id=$(echo "$artifact_block" | grep -o '"id":[0-9]*' | head -1 | cut -d: -f2)

        printf "    %-30s -> %s\n" "$artifact_name" "$dest"

        if [ -z "$artifact_id" ]; then
            echo "    [~] Not found in this run (skipped)"
            continue
        fi

        local zip_url="https://api.github.com/repos/$REPO/actions/artifacts/$artifact_id/zip"
        local zip_file="$tmp/${artifact_name}.zip"
        local extract_dir="$tmp/$artifact_name"

        if download "$zip_url" "$zip_file" "$artifact_name" 2>/dev/null; then
            mkdir -p "$extract_dir"
            unzip -q -o "$zip_file" -d "$extract_dir"
            # Pick the first regular file from the extracted archive
            local binary
            binary=$(find "$extract_dir" -maxdepth 2 -type f | sort | head -1)
            if [ -n "$binary" ]; then
                cp "$binary" "$dest"
                case "$local_name" in
                    SparkBuild-linux-*|SparkBuild-macos)
                        chmod +x "$dest" ;;
                esac
                echo "    [+] OK"
            else
                echo "    [~] Zip was empty (skipped)"
            fi
        else
            echo "    [~] Download failed (skipped)"
        fi
    done

    # Convenience symlink: SparkBuild-linux -> gcc build (backward compat)
    if [ -f "$SCRIPT_DIR/SparkBuild-linux-gcc" ]; then
        ln -sf SparkBuild-linux-gcc "$SCRIPT_DIR/SparkBuild-linux"
        echo
        echo "[*] Symlink: SparkBuild-linux -> SparkBuild-linux-gcc"
    fi
}

# ---------------------------------------------------------------------------
# Release download
# ---------------------------------------------------------------------------

download_release() {
    echo "[*] Fetching SparkBuild release ($TAG) from $REPO ..."
    echo

    # Asset name -> local filename
    declare -A BINARIES=(
        ["SparkBuild.exe"]="SparkBuild.exe"
        ["SparkBuild-linux-gcc"]="SparkBuild-linux-gcc"
        ["SparkBuild-linux-clang"]="SparkBuild-linux-clang"
        ["SparkBuild-macos"]="SparkBuild-macos"
    )

    for asset in "${!BINARIES[@]}"; do
        local local_name="${BINARIES[$asset]}"
        local dest="$SCRIPT_DIR/$local_name"
        local url="https://github.com/$REPO/releases/download/$TAG/$asset"

        printf "    %-25s -> %s\n" "$asset" "$dest"

        if download "$url" "$dest" "$asset" 2>/dev/null; then
            case "$local_name" in
                SparkBuild-linux-*|SparkBuild-macos)
                    chmod +x "$dest" ;;
            esac
            echo "    [+] OK"
        else
            echo "    [~] Not available (skipped)"
        fi
    done

    # Convenience symlink: SparkBuild-linux -> gcc build (backward compat)
    if [ -f "$SCRIPT_DIR/SparkBuild-linux-gcc" ]; then
        ln -sf SparkBuild-linux-gcc "$SCRIPT_DIR/SparkBuild-linux"
        echo
        echo "[*] Symlink: SparkBuild-linux -> SparkBuild-linux-gcc"
    fi
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

if [ "$MODE" = "artifacts" ]; then
    download_artifacts
else
    download_release
fi

echo
echo "[*] Done."
