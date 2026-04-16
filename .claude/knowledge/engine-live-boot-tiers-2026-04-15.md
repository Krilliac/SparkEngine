# Engine live boot — Tiers 1–4 results (gVisor sandbox, 2026-04-15)

**Last updated:** 2026-04-15
**Type:** Observation
**Status:** Active
**Cross-references:** `wine-role-and-fallback-tiers-2026-04-14.md`,
`wine-gvisor-incompatibility.md`, `mingw-wine-cross-compilation.md`

## TL;DR

Live boot of `SparkEngine` exercised across all four tiers from the
fallback ladder. Inside this gVisor sandbox (`/proc/1/comm == process_api`,
`uname -r == 4.4.0`), tiers **3 and 4 boot end-to-end** (clean init →
tick → shutdown, `EXIT=0`); tiers **1 and 2 fail at the documented
gVisor + Wine signal-emulation bug**, identical to a hello-world PE.
The failure is environmental, not a SparkEngine bug — the CI job
`build-linux-mingw-wine` on `ubuntu-24.04` continues to validate tiers
1/2 on real Linux.

## Setup taken in this session

Sandbox started with no Wine, no MinGW, no Vulkan ICD, no SDL2. Installed:

```bash
sudo apt-get install -y mesa-vulkan-drivers libsdl2-dev libgl-dev \
    wine64 g++-mingw-w64-x86-64-posix mingw-w64-tools vulkan-tools
tools/setup-mingw-wine.sh --dxvk-only   # DXVK 2.5.3 to ThirdParty/dxvk
gcc -shared -fPIC -O2 -o tools/gvisor-wine-shim.so tools/gvisor-wine-shim.c -ldl
```

Verified Lavapipe ICD at `/usr/share/vulkan/icd.d/lvp_icd.json`, with
`vulkaninfo --summary` reporting:

```
deviceType    = PHYSICAL_DEVICE_TYPE_CPU
deviceName    = llvmpipe (LLVM 20.1.2, 256 bits)
driverID      = DRIVER_ID_MESA_LLVMPIPE
driverVersion = 25.2.8
```

Built two binaries:

```bash
cmake --preset linux-gcc-release
cmake --build build/linux-gcc-release --target SparkEngine --parallel $(nproc)
# → build/linux-gcc-release/bin/SparkEngine        (ELF, 57 MB)

cmake --preset linux-mingw-release
cmake --build build/linux-mingw-release --target SparkEngine --parallel $(nproc)
# → build/linux-mingw-release/bin/SparkEngine.exe  (PE32+ GUI x86-64, 12 MB)
```

The MinGW exe links exactly the Windows graphics path that would be
exercised under Wine + DXVK:

```
DLL Name: d3d11.dll        DLL Name: dxgi.dll
DLL Name: D3DCOMPILER_47.dll DLL Name: XAudio2_8.dll
DLL Name: dbghelp.dll      DLL Name: KERNEL32.dll
DLL Name: msvcrt.dll       DLL Name: ole32.dll
DLL Name: USER32.dll       DLL Name: WS2_32.dll
DLL Name: libwinpthread-1.dll
```

## Tier 4 — Native Linux + NullRHIDevice (headless)

```bash
build/linux-gcc-release/bin/SparkEngine -test-frames 30 -rhi=null -headless
```

**Result:** `EXIT=0`, ~1.5s wall clock, 267-line clean Logger trace.

Highlights from the boot trace:
- 10 game module DLLs loaded (`libSparkGameRacing.so`, `Platformer`,
  `OpenWorld`, `VisualScript`, `ARPG`, `RTS`, `MMO`, `RPG`, `SparkGame`,
  `FPS`)
- All 25 ECS systems initialized (TweenSystem, FixedTimestepAccumulator,
  ClusteredLightCulling, FoliageRenderer, ClipmapTerrain, …)
- `RPG areas registered with SeamlessAreaManager` — origin rebasing
  enabled at 3000m
- Networking up: `NetworkManager initialized`, `RemoteDebugSystem`,
  `NetworkHealthMonitor`
- Save / quest / inventory / dialogue / mod systems all init'd
- `MemoryIntegritySystem initialized (54 regions auto-discovered)`
- All systems shut down cleanly in reverse order — no leaks reported by
  `GPUResourceLeakDetector` or `MemoryMonitor`

This is the rung the rest of the ladder degrades to when graphics or PE
execution paths fail.

## Tier 3 — Native Linux + SDL2 + Mesa OpenGL (llvmpipe) under Xvfb

```bash
Xvfb :99 -screen 0 1024x768x24 -nolisten tcp &
DISPLAY=:99 LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe \
    MESA_LOADER_DRIVER_OVERRIDE=llvmpipe \
    build/linux-gcc-release/bin/SparkEngine -test-frames 30 -window-size 640x480
```

**Result:** `EXIT=0`, ~1.3s wall clock, 306-line trace. Full graphics
pipeline initialized.

Key markers from the boot:
```
[Input    ] InputManager initialized (Linux/SDL2 backend)
[Graphics ] RHIBridge::Initialize 1280x720
[Graphics ] GLDevice::Initialize starting
[Graphics ] Existing EGL context detected (SDL2/host-owned) — skipping EGL bootstrap
[Graphics ] OpenGL 4.5 (Core Profile) Mesa 25.2.8-0ubuntu0.24.04.1 (GLSL 4.50)
            — Renderer: llvmpipe (LLVM 20.1.2, 256 bits)
[Graphics ] OpenGL swap chain: windowed mode (1280x720)
[Graphics ] MaterialSystem (Linux) initialized
[Graphics ] LightingSystem (Linux) initialized
[Graphics ] PostProcessingPipeline initialized (1280x720)
[Graphics ] LightManager initialized (1280x720, tileSize=16, grid=80x45)
[Graphics ] ScreenSpaceEffects initialized (1280x720, SSAO kernel=32)
[Graphics ] TerrainRenderer initialized (CPU-only mode)
[Graphics ] Initialized on Linux via RHI (OpenGL)
[Audio    ] Audio backend: OpenAL Soft
…
[Graphics ] RHIBridge::Shutdown
[Graphics ] GLDevice::Shutdown
[Graphics ] Shutdown complete
```

The OpenGLDevice picked up the SDL2 host-owned EGL context — the
`SDL_HINT_VIDEO_X11_FORCE_EGL` workaround in `RunSDL2Windowed`
(`SparkEngineLinux.cpp:752`) does its job. `RHIBridge` selected the
OpenGL backend because SDL2 already owns the GL context; the
`SelectBestBackend()` Linux ladder (`Vulkan → OpenGL`) doesn't kick in
when a host context is pre-bound.

(For a pure Vulkan-via-Lavapipe run the engine binary would need to be
launched without an SDL2 GL context; not exercised here because the
default Linux launcher always creates one. A future session could add
`SPARK_RHI_BACKEND=vulkan` parsing to short-circuit the SDL2 GL path —
that is action item #5 from `wine-role-and-fallback-tiers-2026-04-14.md`.)

## Tier 2 / Tier 1 — MinGW + Wine + DXVK / WineD3D + Lavapipe / llvmpipe

Both attempts run the full `tools/wine-run.sh` dispatcher, which
correctly auto-selects:

```
[wine-run] DXVK found at .../ThirdParty/dxvk/x64 — D3D11 will translate to Vulkan
[wine-run]   Vulkan ICD: /usr/share/vulkan/icd.d/lvp_icd.json
[wine-run]   gVisor shim: .../tools/gvisor-wine-shim.so
```

**Tier 1 attempt** (Wine → DXVK → Vulkan → Lavapipe):

```bash
SPARK_WINE_GVISOR_SHIM=1 SPARK_WINE_PROBE=/tmp/hello.exe \
    tools/wine-run.sh build/linux-mingw-release/bin/SparkEngine.exe \
    -test-frames 5 -headless
```

**Tier 2 attempt** (forced Wine → WineD3D → OpenGL → llvmpipe):

```bash
SPARK_WINE_GVISOR_SHIM=1 \
WINEDLLOVERRIDES="d3d11=n,b" \
GALLIUM_DRIVER=llvmpipe LIBGL_ALWAYS_SOFTWARE=1 \
    tools/wine-run.sh build/linux-mingw-release/bin/SparkEngine.exe \
    -test-frames 5 -headless
```

**Result for both:** Wine bails out at the documented `virtual_setup_exception`
gVisor stack-bookkeeping bug:

```
[gvisor-shim] installed SIGSEGV trampoline (wine handler=0x7ebaba57e2e0)
0024:err:virtual:virtual_setup_exception stack overflow 64 bytes
    addr 0x6fffffcad75e
    stack 0x7ebab9a00fc0 (0x7ebab9a00000-0x7ebab9a01000-0x7ebab9c00000)
```

The same address (`0x6fffffcad75e`) appears whether the binary is
SparkEngine.exe or `/tmp/hello.exe` — meaning the failure happens before
control ever reaches engine code. Reproduced with `tools/SparkBuild.exe`
as well.

The gVisor shim **does** get past the `trap 0` infinite loop (first
gVisor + Wine bug) — both `[gvisor-shim] installed SIGSEGV trampoline`
lines confirm the LD_PRELOAD took effect — but cannot fix the second
bug, which needs a Wine source patch to `virtual_setup_exception`.

This is exactly the failure mode documented in
`wine-gvisor-incompatibility.md` and there is no engine-side workaround.
On real Linux (CI's `ubuntu-24.04`, a normal VM, a Docker container
under runc), neither bug fires and tiers 1/2 work.

## Sandbox vs CI matrix

| Tier | This gVisor sandbox | CI `build-linux-mingw-wine` (ubuntu-24.04) |
|------|---------------------|---------------------------------------------|
| 4 — Native + NullRHI       | ✅ Works | ✅ (build-linux-gcc) |
| 3 — Native + Xvfb llvmpipe | ✅ Works | ✅ (xvfb-based editor tests) |
| 2 — Wine + WineD3D + GL    | ❌ Wine virtual_setup_exception | ✅ |
| 1 — Wine + DXVK + Vulkan   | ❌ Wine virtual_setup_exception | ✅ |

## What this confirms about the engine

1. The native Linux build is **healthy**: 25 ECS systems, 10 game
   modules, full graphics pipeline (16 post-process passes, clustered
   lighting, foliage, terrain, shadow atlas), audio, networking,
   memory-integrity scanning — every subsystem initializes and shuts
   down cleanly in both NullRHI and OpenGL paths.
2. The MinGW cross-compile is **healthy**: produces a 12 MB PE32+
   binary that links the correct Windows D3D11 / DXGI / D3DCOMPILER /
   XAudio2 imports. The build is reproducible from `linux-mingw-release`
   preset with no patches.
3. `tools/wine-run.sh` correctly auto-detects DXVK + Lavapipe + the
   gVisor shim and starts Wine — the failure is below it.
4. The gVisor + Wine incompatibility from
   `wine-gvisor-incompatibility.md` is **still** the only blocker for
   tiers 1/2 in this sandbox class. Nothing in this session changes
   that diagnosis.

## What still needs work (action items still open from
`wine-role-and-fallback-tiers-2026-04-14.md`)

1. `SPARK_SKIP_WINE=1` umbrella that automatically falls through tier 4
   when Wine is broken in the environment — would let `tools/wine-run.sh
   <engine.exe>` succeed silently in any sandbox by transparently
   running the native ELF instead. Highest-value escape hatch for this
   exact session.
2. `IsRunningUnderGvisor()` runtime check in `Utils/WineDetection.{h,cpp}`
   — would let the engine startup banner print "gVisor detected — Wine
   tiers 1/2 will likely fail, falling back to tier 4" before the user
   has to read this entry.
3. `SPARK_RHI_BACKEND={null,opengl,vulkan,d3d11}` env var so the Linux
   launcher can take the Vulkan path without going through SDL2's GL
   context (Tier 3 vulkan variant).

None of these are urgent — Tier 4 already provides a working live boot
inside the sandbox today. They are quality-of-life so a future session
in a similar environment can reach the same conclusion in seconds
instead of minutes.

## Reproduction commands (for the next session)

```bash
# Build (assumes apt prerequisites already installed; see "Setup" above)
cmake --preset linux-gcc-release && \
    cmake --build build/linux-gcc-release --target SparkEngine --parallel $(nproc)
cmake --preset linux-mingw-release && \
    cmake --build build/linux-mingw-release --target SparkEngine --parallel $(nproc)

# Tier 4 (always works inside any sandbox)
build/linux-gcc-release/bin/SparkEngine -test-frames 30 -rhi=null -headless

# Tier 3 (works wherever Mesa llvmpipe + Xvfb work, including most sandboxes)
Xvfb :99 -screen 0 1024x768x24 -nolisten tcp &
DISPLAY=:99 LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe \
    build/linux-gcc-release/bin/SparkEngine -test-frames 30 -window-size 640x480

# Tier 1 (works on real Linux; blocked by gVisor signal bug in sandbox)
SPARK_WINE_GVISOR_SHIM=1 \
    tools/wine-run.sh build/linux-mingw-release/bin/SparkEngine.exe \
    -test-frames 5 -headless
```
