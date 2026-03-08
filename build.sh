#!/usr/bin/env bash
# build.sh  â”€ simple cross-platform build helper
set -e

CFG=Debug
GENERATOR="Unix Makefiles"
ENABLE_EDITOR=ON
ENABLE_CONSOLE=ON
ENABLE_AS=ON

usage() {
  echo "Usage: $0 [debug|release] [-E disable editor] [-C disable console] [-A disable AngelScript]"
  exit 1
}

# Parse flags
while [[ $# -gt 0 ]]; do
  case "$1" in
    debug)   CFG=Debug   ;;
    release) CFG=Release ;;
    -E) ENABLE_EDITOR=OFF ;;
    -C) ENABLE_CONSOLE=OFF ;;
    -A) ENABLE_AS=OFF ;;
    -g) if [[ $# -lt 2 ]]; then echo "Error: -g requires a generator name"; exit 1; fi; shift; GENERATOR="$1" ;;
    *) usage ;;
  esac
  shift
done

mkdir -p build && cd build
cmake .. -G "${GENERATOR}" \
  -DCMAKE_BUILD_TYPE=${CFG} \
  -DENABLE_EDITOR=${ENABLE_EDITOR} \
  -DENABLE_CONSOLE=${ENABLE_CONSOLE} \
  -DENABLE_ANGELSCRIPT=${ENABLE_AS}
cmake --build . -- -j$(nproc)
