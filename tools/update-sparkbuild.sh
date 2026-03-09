#!/usr/bin/env bash
# update-sparkbuild.sh -- Download SparkBuild binaries from GitHub Releases
#
# Downloads Windows, Linux, and macOS binaries.
#
# Usage:  ./tools/update-sparkbuild.sh              (fetches "latest" release)
#         ./tools/update-sparkbuild.sh v1.0.0       (fetches a specific version)

set -e

TAG="${1:-latest}"
REPO="Krilliac/SparkBuild"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Map of asset name -> local filename
declare -A BINARIES=(
    ["SparkBuild.exe"]="SparkBuild.exe"
    ["SparkBuild-linux"]="SparkBuild-linux"
    ["SparkBuild-macos"]="SparkBuild-macos"
)

download() {
    local url="$1" dest="$2"
    if command -v curl &>/dev/null; then
        curl -fL -o "$dest" "$url"
    elif command -v wget &>/dev/null; then
        wget -O "$dest" "$url"
    else
        echo "[-] Neither curl nor wget found." >&2
        return 1
    fi
}

echo "[*] Fetching SparkBuild binaries ($TAG) from $REPO ..."
echo

for asset in "${!BINARIES[@]}"; do
    LOCAL="${BINARIES[$asset]}"
    DEST="$SCRIPT_DIR/$LOCAL"
    URL="https://github.com/$REPO/releases/download/$TAG/$asset"

    printf "    %-20s -> %s\n" "$asset" "$DEST"

    if download "$URL" "$DEST" 2>/dev/null; then
        # Make Linux/macOS binaries executable
        case "$LOCAL" in
            SparkBuild-linux|SparkBuild-macos)
                chmod +x "$DEST"
                ;;
        esac
        echo "    [+] OK"
    else
        echo "    [~] Not available (skipped)"
    fi
done

echo
echo "[*] Done."
