#!/usr/bin/env bash
# build.sh  — simple cross-platform build helper
set -e

CFG=Debug
GENERATOR="Ninja"
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

# -----------------------------------------------------------------
# Check and initialize git submodules
# -----------------------------------------------------------------
echo ""
echo "=== Checking git submodules ==="

SUBMODULES=(
  "ThirdParty/Utils/miniz|miniz|Compression (crash dumps, save files)"
  "ThirdParty/Physics/bullet3|Bullet Physics|Rigid body physics, collision, raycasting"
  "ThirdParty/UI/imgui|Dear ImGui|Editor UI, debug overlays"
  "ThirdParty/ECS/entt|EnTT|Entity component system"
  "ThirdParty/Scripting/angelscript-mirror|AngelScript|Hot-reload scripting"
  "ThirdParty/Networking/curl|curl|HTTP networking (optional)"
)

missing_submodules=0
for entry in "${SUBMODULES[@]}"; do
  IFS='|' read -r path name desc <<< "$entry"
  # A submodule directory exists but is empty (not initialized) when it has no files
  if [ -d "$path" ] && [ -z "$(ls -A "$path" 2>/dev/null)" ]; then
    echo "  [MISSING] $name ($path) — $desc"
    missing_submodules=$((missing_submodules + 1))
  elif [ ! -d "$path" ]; then
    echo "  [MISSING] $name ($path) — $desc"
    missing_submodules=$((missing_submodules + 1))
  else
    echo "  [OK]      $name"
  fi
done

if [ "$missing_submodules" -gt 0 ]; then
  echo ""
  echo "WARNING: $missing_submodules submodule(s) not initialized."
  echo "  The engine will build but with DEGRADED functionality."
  echo "  Initializing submodules now..."
  echo ""
  git submodule update --init --recursive
  echo ""
  echo "Submodules initialized successfully."
fi

# -----------------------------------------------------------------
# Check system dependencies (Linux)
# -----------------------------------------------------------------
echo ""
echo "=== Checking system dependencies ==="

missing_deps=0

# SDL2
if pkg-config --exists sdl2 2>/dev/null; then
  echo "  [OK]      SDL2 ($(pkg-config --modversion sdl2))"
elif [ -f /usr/include/SDL2/SDL.h ] || [ -f /usr/local/include/SDL2/SDL.h ]; then
  echo "  [OK]      SDL2 (headers found)"
else
  echo "  [MISSING] SDL2 — Install with: sudo apt install libsdl2-dev"
  echo "            Without SDL2, Linux builds will run in headless/fallback mode only."
  missing_deps=$((missing_deps + 1))
fi

# Vulkan SDK (optional)
if pkg-config --exists vulkan 2>/dev/null || [ -n "${VULKAN_SDK:-}" ]; then
  echo "  [OK]      Vulkan SDK"
else
  echo "  [INFO]    Vulkan SDK not found (optional — Vulkan backend will be disabled)"
fi

# CMake
if command -v cmake &>/dev/null; then
  echo "  [OK]      CMake ($(cmake --version | head -1 | sed 's/cmake version //'))"
else
  echo "  [MISSING] CMake — Install with: sudo apt install cmake"
  missing_deps=$((missing_deps + 1))
fi

# Build tool
if [ "$GENERATOR" = "Ninja" ]; then
  if command -v ninja &>/dev/null; then
    echo "  [OK]      Ninja ($(ninja --version))"
  else
    echo "  [MISSING] Ninja — Install with: sudo apt install ninja-build"
    echo "            Or use -g 'Unix Makefiles' to build with make instead."
    missing_deps=$((missing_deps + 1))
  fi
fi

if [ "$missing_deps" -gt 0 ]; then
  echo ""
  echo "WARNING: $missing_deps system dependency(ies) missing."
  echo "  Install the missing packages above for full functionality."
  echo "  Quick install (Ubuntu/Debian): sudo apt install build-essential ninja-build cmake libsdl2-dev"
  echo ""
fi

echo ""
echo "=== Configuring and building SparkEngine ($CFG) ==="
echo ""

mkdir -p build && cd build
cmake .. -G "${GENERATOR}" \
  -DCMAKE_BUILD_TYPE=${CFG} \
  -DENABLE_EDITOR=${ENABLE_EDITOR} \
  -DENABLE_CONSOLE=${ENABLE_CONSOLE} \
  -DENABLE_ANGELSCRIPT=${ENABLE_AS}
cmake --build . -- -j$(nproc)
