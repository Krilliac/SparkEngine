#!/usr/bin/env bash
# update-sparkbuild.sh -- Download the latest SparkBuild.exe from GitHub Releases
#
# Usage:  ./tools/update-sparkbuild.sh              (fetches "latest" pre-release)
#         ./tools/update-sparkbuild.sh v1.0.0       (fetches a specific version)

set -e

TAG="${1:-latest}"
REPO="Krilliac/SparkBuild"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEST="$SCRIPT_DIR/SparkBuild.exe"

URL="https://github.com/$REPO/releases/download/$TAG/SparkBuild.exe"

echo "[*] Downloading SparkBuild.exe ($TAG) ..."
echo "    $URL"

if command -v curl &>/dev/null; then
    curl -L -o "$DEST" "$URL"
elif command -v wget &>/dev/null; then
    wget -O "$DEST" "$URL"
else
    echo "[-] Neither curl nor wget found." >&2
    exit 1
fi

echo "[+] Updated: $DEST"
