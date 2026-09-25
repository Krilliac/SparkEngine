# Linux Support Evidence (PLT-210)

> **Scope of this document.** One full Linux build plus a CTest run on one
> host, with fixes for the Linux-only defects that run exposed. It **does not
> certify** Linux support. The PLT-210 acceptance criteria (clean-machine
> package/install/uninstall, GPU drivers, desktop/audio/input stacks, rendered
> golden scenes) were **not** exercised. Every claim below is limited to what
> was actually run.

## 1. Exact inputs

| Item | Value |
|---|---|
| Base commit | `b5debbd43` (`claude/stable-v1-release`) for the before/after runs; branch later rebased onto `8738dff59` and re-verified (§4) |
| Evidence head | `claude/cloud-plt-210`. After the rebase, the fixes are commits `f68b23b`, `b68ae53`, `d5c770d` and `7cbba1f` on `8738dff59`. The before/after runs were built on the pre-rebase base `b5debbd`, where the same fixes were `5a823b2`…`3e5da14`. |
| Host OS | Ubuntu 24.04.4 LTS, kernel 6.18.44, x86_64, **gVisor (runsc) sandbox** (the engine logs this at startup) |
| CPU / RAM | 4 vCPU / 15 GiB, no GPU, no `/dev/dri` |
| Compiler | GCC 13.3.0 (`Ubuntu 13.3.0-6ubuntu2~24.04.1`), GNU ld 2.42 |
| Build tools | CMake 3.28.3, Ninja 1.11.1, clang-format 18.1.3, Python 3.11.15 |
| Graphics userland | Mesa 25.2.8 (llvmpipe), Xvfb. `libgl-dev` absent: configure used the bundled `ThirdParty/OpenGL` headers. Vulkan SDK absent, so the Vulkan backend was disabled at configure time. |
| Submodules | `git submodule update --init --recursive --depth 1` (recast, entt, SDL2, angelscript, imgui, miniz) |

## 2. Commands

```bash
git submodule update --init --recursive --depth 1
CMAKE_GENERATOR=Ninja cmake --preset linux-gcc-release -DBUILD_TESTS=ON -DBUILD_GAME_MODULES=ON
cmake --build build/linux-gcc-release -j4 -- -k 0          # 2294 steps, ~22 min
cd build/linux-gcc-release && ctest -j4 --output-on-failure --output-junit ctest-junit.xml
```

The preset sets no generator. `CMAKE_GENERATOR=Ninja` only selects the
generator; every cache variable came from the preset. Effective flags include
`ENABLE_LTO=ON` (the preset default), `ENABLE_OPENGL=ON`, `ENABLE_SDL2=ON`,
`ENABLE_EDITOR=ON`, `ENABLE_NETWORKING=ON`, and `ENABLE_VULKAN=ON` (but no
Vulkan SDK was found).

## 3. Build result

* **Every target built except `bin/SparkTests`.** That includes `SparkEngine`,
  `SparkEditor`, `SparkConsole`, `SparkServer`, `SparkGateway`, `SparkDaemon`,
  `SparkLauncher`, `SparkInstaller`, `SparkBuild`, `SparkCooker`,
  `SparkWorker`, `SparkAutomation`, `SparkShaderCompiler`,
  `SparkCrashReporter`, and all 11 game modules (`lib*.so` + `.sparkabi`).
  The build emitted 713 compiler warnings, none fatal.
* **Known broken: `SparkTests` fails to link** with the preset as shipped:
  `multiple definition of ...` (thousands of symbols, reported as
  "symbol from plugin"). Root cause, confirmed from `build.ninja`: the
  `SparkTests` link line contains
  `-Wl,--push-state,--whole-archive lib/Release/libSparkEngineLib.a -Wl,--pop-state`
  **twice**. One copy comes from
  `LINK_LIBRARY_OVERRIDE "WHOLE_ARCHIVE,SparkEngineLib"`
  (`Tests/CMakeLists.txt`, `SparkServerCore` block); the other comes through
  another dependency. Removing the second group and relinking by hand
  succeeds (2 min 50 s). CI's `build-linux-gcc` job does not report this. Its
  runner CMake is not pinned in `build.yml`, and a newer CMake than the
  distro's 3.28.3 probably de-duplicates the group; **that is a hypothesis
  and was not verified here.**
  *Needed change (owned by the build/CMake lane; this lane may not edit
  `CMakeLists.txt`/`cmake/**`):* ensure `SparkEngineLib` appears only once
  as a whole-archive group on the `SparkTests` link line, or raise
  `cmakeMinimumRequired` to the first version that de-duplicates it. Until
  then, Ubuntu 24.04's stock CMake cannot produce `SparkTests`.
  All test numbers below come from the hand-relinked binary (same objects,
  same flags, with the duplicate group removed).

## 4. Test results

"Before" means base `b5debbd` (the `ProcessLinux.cpp` object was rebuilt from
the base source before this run). "After" means head `3e5da14` (the same fixes on `b5debbd`). "Rebased" means the fixes on `8738dff59` (tip `7cbba1f`).

| Suite | Before | After | Rebased |
|---|---|---|---|
| CTest entries | **83 / 83 passed**, 0 failed, 0 not run (65.6 s) | **83 / 83 passed**, 0 failed, 0 not run (63.2 s) | **86 / 86 passed** (base added 3 entries; 63.9 s) |
| `SparkEngineTests` (in-binary tests, JUnit) | 7277 run, **0 failed, 9 skipped** | 7289 run (+12 new PLT-210 tests), **0 failed, 9 skipped** | 7306 run, **0 failed, 9 skipped**; all 12 PLT-210 tests pass |
| `SparkEngineLoadTests` (load lane) | 22 / 22 | 22 / 22 | 22 / 22 |

The 9 skips are the same in all three runs. After the rebase the stock `SparkTests` link still fails the same way (two whole-archive groups). All are declared skips:
Windows-only regressions (`GatewayAreaControl_*` ×2,
`SceneManager_UnicodePathRoundTripsWithCacheAndAsciiControl`,
`ShaderDiskCachePhaseV_UnicodeDirectoryRoundTripsOnWindows`), MSan canaries
(×2, no MSan here), process-wide `EngineContext` limits (`EditorPanels_*` ×2),
and `TestFramework_DynamicSkipIsReported` (intentional).

**A green suite did not mean Linux worked.** Every defect in §5 was found by
running the real executables, not by the suite.

## 5. Linux defects found and fixed (each with a test)

| # | Defect (Linux-only behaviour) | Fix | Test (runs production code) |
|---|---|---|---|
| 1 | `Spark::Process` launched a **missing executable or working directory "successfully"**: fork succeeded, and the child exited 127/126 later. Windows fails `Launch()` outright. `ConsoleProcessManager` therefore thought a missing `SparkConsole` was running. | Close-on-exec error pipe reports the child's `chdir`/`exec` errno, so `Launch()` returns an error. `SparkEngine/Source/Utils/ProcessLinux.cpp` (`execErrorPipe`, `reportFailureAndExit`). | `PLT210_ProcessPosix_Missing{Executable,WorkingDirectory}FailsLaunch`, `..._DetachedMissingExecutableFailsLaunch` |
| 2 | `Process::WriteStdin` to a child that had exited raised **SIGPIPE and killed the launching process**. Before the fix, a standalone harness died with exit code 141. Only `SparkEngine` and `SparkDaemon` ignore SIGPIPE; the editor, tools and test runner do not. | Block SIGPIPE on the writing thread and discard the generated signal (`sigtimedwait`). `ProcessLinux.cpp` `Process::WriteStdin`. | `PLT210_ProcessPosix_WriteToExitedChildDoesNotRaiseSigpipe` |
| 3 | **Detached children were never reaped.** Each one stayed a zombie of the launcher (3 zombies in the pre-fix harness). | Double fork: the intermediate child is reaped and the grandchild is re-parented to init. | `PLT210_ProcessPosix_DetachedChildLeavesNoZombie` |
| 4 | **Detached children inherited the launcher's stdio.** `SparkCrashReporter` (launched detached by the engine) held the engine's stdout, so a supervisor reading stdout to EOF **hung forever**: the reporter waits for an engine PID that stays a zombie until the supervisor calls `waitpid`. | Uncaptured stdio of a detached child goes to `/dev/null`. `ProcessLinux.cpp`. | `PLT210_ProcessPosix_DetachedChildDoesNotHoldLauncherStdout`, and end to end in `PLT210_Module_MMOFPSLoadsInHeadlessEngine` (which hung before this fix) |
| 5 | **`SparkConsole --engine-pipe` fed its own display back to the engine as commands.** Its prompt, key echo, engine-log echo, duplicate notices and results went to stdout, which is the engine's command channel. The pipe-mode keyboard thread also read stdin, which is the engine's log pipe. Each 120-frame headless run logged **107–186 `Unknown command`** results, and engine error messages (e.g. module load failures) were consumed instead of shown. | In `SparkConsole/src/ConsoleApp.cpp` (POSIX branches), display output goes to stderr (batch mode keeps stdout), and keyboard input is read from `/dev/tty`, disabled when there is no controlling terminal. After the fix, **0** `Unknown command` in every run. | `PLT210_SparkConsole_EnginePipeStdoutCarriesOnlyCommands` (launches the built `SparkConsole` the way the engine does) |
| 6 | **`libSparkGameMMOFPS.so` failed `dlopen(RTLD_NOW)`**: `undefined symbol GraphicsEngine::SetBasicBlendMode` (also `SetBasicDepthMode` and `GetOrCreateSoftCircleShadowSRV`). They were declared for all platforms but defined only in the D3D11 source. | Linux definitions following the existing basic-path no-op/`nullptr` pattern, in `SparkEngine/Source/Graphics/GraphicsDeviceResourcesLinuxShaders.cpp`. A symbol audit (`nm -D`) of all 11 modules against the engine executable now shows no unresolved engine symbols. | `PLT210_Module_MMOFPSLoadsInHeadlessEngine` (real engine + `-require-game`), `PLT210_GraphicsBasicPath_MMOFPSSurfaceDefinedOnLinux` |
| 7 | **Every AngelScript build failed on x86-64 System V**: "Don't support returning type 'Vector3' by value from application in native calling convention on this platform". `XMFLOAT3` is returned in XMM registers there and was registered without `asOBJ_APP_CLASS_ALLFLOATS`. This blocked `SparkGameVisualScript`. | Add `asOBJ_APP_CLASS_ALLFLOATS` in `SparkEngine/Source/Engine/Scripting/AngelScriptEngine.cpp` `RegisterMathTypes` (other platforms ignore the flag). | `PLT210_AngelScript_Vector3ReturnByValueCompiles` and `..._Vector3ReturnedValuesSurviveNativeCall` (both **failed** on the base and pass after; the second round-trips real `Transform` values through native calls) |

Also checked and found clean: no case-mismatched `#include "..."` paths
repo-wide, and no case-mismatched asset path literals in engine/module sources
or `Assets/` data files. The check was a script that compared every include
and asset literal against the on-disk file names case-sensitively.

## 6. Runtime smoke (real binaries)

Command: `bin/SparkEngine -headless -game <abs path>/lib<Module>.so -require-game -test-frames 120`, run from `bin/`.

| Module | Before | After |
|---|---|---|
| SparkGame, ARPG, FPS, MMO, OpenWorld, Platformer, RPG, RTS, Racing | load, exit 0 (with console feedback-loop spam) | load, exit 0, no spam |
| SparkGameMMOFPS | **exit 2**: `dlopen` undefined symbol | load, exit 0 |
| SparkGameVisualScript | **exit 2**: scripts not found | **still exit 2** from `bin/` (see §7.1); loads and exits 0 when its scripts are reachable (cwd `lib/`) |

Notes:
* On Linux, `-game` takes a **path** to the `.so`. A bare module name is
  rejected ("Explicit game module not found"). Before fix #5 that message was
  never shown to the user.
* **Windowed/editor:** under Xvfb, `SparkEditor` started on **OpenGL 4.5 Core
  (Mesa llvmpipe)**, initialized ImGui, fonts and panels, and was still running
  when the 25 s timeout killed it. No interaction or rendering correctness was
  checked.
* **Engine rendering on Linux was not demonstrated** in the original run.
  Without a backend override, the SDL2 path chose `NullRHIDevice` because of
  the gVisor detection. With `SPARK_RHI_BACKEND=opengl` under Xvfb, the RHI
  still logged "No graphics backend available — falling back to
  NullRHIDevice", with no reason given, and the run exited 0.
* **Root cause and fix (added later, PLT-210 / RHI-240).** The SDL2 host
  unconditionally set `SDL_HINT_VIDEO_X11_FORCE_EGL=1`. This host has no
  `libEGL` (only `libGL`/`libGLX_mesa`), so `SDL_CreateWindow` failed with
  "Could not load EGL library". The failure was logged through `SimpleConsole`
  before the console existed, so nothing reached stdout. `GraphicsEngine` then
  received a null window and picked NullRHI. Commit `b3607df` (RHI-240) now
  forces EGL only in `SPARK_EGL_SUPPORT` builds, which link `libEGL` and whose
  `GLDevice` reuses a host EGL context. GLX builds keep SDL's GLX context,
  which `GLDevice` already reuses. That commit also logs SDL failures through
  `SPARK_LOG_ERROR` and refuses to start when `SPARK_RHI_BACKEND` names a GPU
  backend that did not come up. PLT-210 then made the windowed host fail
  closed whenever it tried to create a window, Metal view or GL context and
  could not, whether or not a backend was named. It logs
  "Windowed startup could not create its render window or context — refusing
  to continue on NullRHIDevice" and exits 1. When the window and context exist
  but `GraphicsEngine::Initialize` fails for them (RHIBridge refuses the
  headless fallback for a windowed surface in Release builds), the host logs
  "Windowed startup could not initialize a render device for its window —
  refusing to continue on NullRHIDevice" and exits 1 as well. A failed Vulkan
  init is not final there: the Vulkan-to-OpenGL rebuild runs, and only its
  result decides. Runs that never try to create a window are unchanged:
  `-headless`, `SPARK_RHI_BACKEND=null`, and hosts where the RHI recommends no
  GPU backend. Debug builds still pass `allowHeadlessFallback=true` to
  RHIBridge, so a Debug windowed run whose GPU backends all fail comes up on
  NullRHI unless `SPARK_RHI_BACKEND` names a backend. The render-device refusal
  is checked by reading the code only. On this host, runs with no backend named
  (or `SPARK_RHI_BACKEND=auto`) get "recommended backend = None" and never try a
  window. A named backend that fails is still refused with the explicit-backend
  message, which is checked before the render-device message. Re-run on this host at HEAD `d44383d`
  plus this change (local, not CI):
  * From `bin/`: `xvfb-run -a env SPARK_RHI_BACKEND=opengl ./SparkEngine -game
    $PWD/libSparkGameFPS.so -require-game -test-frames 30 -no-subprocess`
    exits 0 on "OpenGL 4.5 (Core Profile) Mesa 25.2.8 … llvmpipe". It logs
    exactly one "Initialized on Linux via RHI (OpenGL)" and no NullRHI
    selection.
  * The same command with `DISPLAY` unset and no Xvfb exits 1. SDL picks its
    `offscreen` driver, which reports "SDL_CreateWindow failed: Could not load
    EGL library", and the refusal follows.
  * With `SDL_VIDEO_X11_FORCE_EGL=1` exported (the old forced hint), the
    run under Xvfb now exits 1 with the same SDL reason. It no longer exits 0
    on NullRHI.
  * CTest `SparkEngineExplicitOpenGLStartup` (labels `opengl;llvmpipe;linux;
    sdl2`) covers four cases. It checks the explicit OpenGL start and the
    `SPARK_DISABLE_OPENGL=1` refusal. It runs SparkGameFPS for 30 frames on
    the OpenGL window and requires exactly one OpenGL initialization. It also
    requires a non-zero exit with the SDL reason and the refusal on
    `SDL_VIDEODRIVER=dummy`, where window creation fails on every host.
* Audio: the OpenAL Soft backend initialized. There was no audio device and
  no audible output was verified.
* **Headless NullRHI records (added later, PLT-210).** `RunHeadlessLinux` now
  prints the same post-teardown `SPARK_HEADLESS_RHI` and
  `SPARK_HEADLESS_LIFECYCLE` records as the Windows headless host, and exits 3
  if the NullRHI bridge survives teardown. CTest `NullRHI_Linux_FPSLifecycle`
  runs the real `SparkGameFPS` module for 8 frames and checks the records with
  the strict parser in `cmake/RunSparkHeadlessNullRHILifecycle.cmake`: exactly
  one ready, rhi and lifecycle record, in that order, with `rendered=0` and
  `faults=0`. This is source-tree evidence only. It does not certify a package.

### 6.1 Installed-tree runtime closure (added later, PLT-210)

`Tests/PackageSmoke/VerifyLinuxInstalledRuntime.cmake` checks an installed
tree, not the build tree. CTest `VerifyLinuxInstalledRuntime` (labels
`package;linux;integration`) runs `cmake --install` into a fresh prefix under
the build directory. The same script also runs standalone against an existing
prefix, such as a `linux-shipping` install (that preset sets `BUILD_TESTS=OFF`).
The script fails on any of the following:

* a RUNPATH/RPATH entry that is absolute, empty, or `$ORIGIN`-relative but
  escapes the prefix (checked with `readelf -d` on every ELF in the prefix);
* a library in any ELF's `ldd` closure (run with an empty environment) that is
  not found, resolves into the source or build tree, or resolves outside both
  the prefix and the host's system library directories (the multilib defaults
  plus the `ldconfig` cache);
* a library the package ships in `<prefix>/lib` (today `libSDL2-2.0.so.0`)
  that resolves anywhere else;
* a symlink that leaves the prefix;
* a `.sparkabi` sidecar whose `binary_sha256` does not hash its installed
  module;
* a run of the installed `SparkEngine -headless` with the installed
  `libSparkGameFPS.so`, started from cwd `/` with an empty environment and fresh
  `HOME`/`XDG_*` directories, that does not pass the shared NullRHI record
  parser, prints no clean `SPARK_MODULE_LIFECYCLE module=SparkGameFPS` record,
  names the source or build tree in its output, adds, removes or changes any
  file in the prefix (SHA256 of every file and every symlink target, taken
  before and after the run), or creates a new top-level entry in its working
  directory `/`. Writes deeper under `/` (for example `/var`) are not
  observed; `HOME`, the `XDG_*` directories and `TMPDIR` point into the test
  root.

On a pass the install-mode test deletes the installed prefix and the scratch
`HOME`/`TMPDIR` and keeps only `runtime-closure-report.txt`, `install.log` and
the engine logs; the self test deletes each fixture prefix once its case
passes. A failure keeps everything for diagnosis.

CTest `LinuxInstalledRuntime_ClosureDetection` builds defective copies of the
real build-tree images and checks that each rule rejects its copy. It also runs
a clean install-shaped positive control, and checks that the prefix snapshot
detects a same-name, same-size in-place rewrite.

**Defect found and fixed.** Before this check, every installed Linux game
module was rejected at load. `cmake --install` rewrote each module's
build-tree RUNPATH to `$ORIGIN/../lib`, so the image no longer matched the
`binary_sha256` its POST_BUILD sidecar had recorded, and `ModuleManager`
refused it before `dlopen`. The installed engine then exited 2 under
`-require-game` with `SPARK_HEADLESS_LIFECYCLE initialized=0`, and printed no
rejection reason to stdout or stderr. The fix is in `cmake/SparkGameModule.cmake`:
`spark_configure_module_abi` sets `BUILD_WITH_INSTALL_RPATH` on ELF, so the
build-tree image and the installed image are byte-identical. On the tree
before the fix the script reported all 11 sidecars as mismatched and the
installed run exited 2. After the fix it passed.

Host evidence from 2026-09-25 (linux-gcc-release, GCC 13.3, Release):

| Item | Value |
|---|---|
| Host | Ubuntu 24.04.4 LTS x86-64 under gVisor (kernel 6.18.44-fc-v37) |
| glibc | Ubuntu GLIBC 2.39-0ubuntu8.9 |
| libstdc++ | `/usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.33` |
| ELF images / module sidecars in the prefix | 30 / 11 |
| Libraries from the host | glibc (`libc`, `libm`, `ld-linux`), `libstdc++`, `libgcc_s`, `libGL`/`libGLX`/`libGLdispatch`, `libX11`/`libxcb`/`libXau`/`libXdmcp`, `libfreetype` with `libpng16`/`libz`/`libbz2`/`libbrotli*`, `libbsd`/`libmd` |
| Shipped in the prefix | `libSDL2-2.0.so.0` |
| Installed run records | `SPARK_MODULE_READY count=1`; `SPARK_HEADLESS_RHI backend=null initialized=1 frames=8 shutdown=1`; `SPARK_HEADLESS_LIFECYCLE initialized=1 updated=8 fixed=10 rendered=0 unloaded=1 faults=0` |

The host-library list shows what this build links against on this host. It
is not a declared dependency set or a distribution range. `bin/SparkEngine`
itself has `NEEDED` entries for `libGL.so.1` and `libX11.so.6`, which pull in
`libGLX`, `libGLdispatch`, `libxcb`, `libXau`, `libXdmcp` and, through
`libXdmcp`, `libbsd`/`libmd`. The same holds for `SparkServer`, `SparkGateway`
and `SparkShaderCompiler`. The loader needs these even for `-headless` (see
§7). Only `libfreetype` and its `libpng16`/`libz`/`libbz2`/`libbrotli*`
dependencies come from other tools (`SparkEditor`, `SparkLauncher`,
`SparkInstaller`, `SparkTests`), not from the engine. Negative checks run: a copy of the
installed prefix whose `bin/SparkEngine` RUNPATH was set to the build `lib/`
failed with both the RUNPATH and the `libSDL2` resolution rule. Pointing the
script at the build tree itself failed on the absolute and empty build
RUNPATHs.

The `linux-shipping` preset (MinSizeRel, `STRIP_DEBUG_SYMBOLS=ON`) was also
configured and built locally, with `-DSPARK_GAME_MODULES=SparkGameFPS` to
limit build time, and its install was checked. The script ran once in install
mode and once in standalone mode on that prefix, and both passed. The prefix
held 36 ELF images, including 18 split `symbols/*.debug` files, and 1 module
sidecar. It resolved the same host libraries as the table above, and the
installed run printed the same records. The full 11-module shipping set was
not built.

### 6.2 Sanitizer and bounded-soak evidence for the shared headless FPS/NullRHI path (HEAD-220)

> **Linux shared-code evidence only.** These runs exercise the engine core,
> the NullRHI device and the real `SparkGameFPS` module through the production
> Linux headless host (`RunHeadlessLinux`). They are **not** Windows package,
> clean-machine, no-display-host or hosted exact-SHA evidence, and they do not
> replace the planned `headless-windows-package` / `headless-windows-soak`
> jobs.

**Inputs.** Base commit `b71fe358c81ad4923971153de24dbd8f5a71bf07` plus this
slice's working-tree changes (the commit that adds this section). Same host as
§1 (gVisor, 4 vCPU, no GPU), GCC 13.3.0, CMake 4.4.3, Python 3.11.15, Unix
Makefiles, ccache.

**What the runs check.** The `nullrhi-headless` label (12 tests): the strict
lifecycle parser and `NullRHI_Linux_FPSLifecycle`; the shutdown harness
(`HeadlessShutdown_Graceful`, `_ForcedRecovery`, `_BootInterrupted`, i.e.
SIGTERM, SIGKILL-then-restart and kill-during-boot); `NullRHIResourceLifetime`
(8 `NullRHI_Lifetime_*` tests); `HeadlessTickStats`;
`Benchmark_HeadlessTickLoop`; the 120 s `Soak_NullRHIHeadlessSmoke`; and the
FPS headless arena (`FPSSinglePlayerSlice_HeadlessArenaLinux`). There is no
NullRHI save/reload test yet, so save/reload is **not** covered here.

The host now prints `SPARK_HEADLESS_NULLRHI_RESOURCES live=N` after teardown.
`N` is the number of NullRHI resources that some owner still held when
`EngineRuntime::ShutdownHeadlessRhi` released the device
(`NullRHIDevice::GetLiveResourceCountAtShutdown`, carried out through
`RHIBridge::GetNullResourcesLiveAtShutdown`). `tools/perf-budget/run_nullrhi_soak.py`
requires exactly one such record with `N == 0`.

**Scope of that record.** Game modules cannot reach the headless NullRHI
device: `EngineRuntime::headlessRhiBridge` is not exposed through
`EngineContext`, and the headless hosts only call its `BeginFrame` and
`EndFrame`. SparkGameFPS therefore creates no NullRHI resource, and `live=0`
only proves that the bridge and device release their own resources. It is a
teardown guard, not an FPS resource-leak check; it becomes one once module or
render work allocates on the device.

**Commands.**

```bash
# ASan + UBSan + LSan (preset plus the CI job's flags; -g1 keeps the tree ~11 GiB)
cmake --preset ci-linux-asan -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  "-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer" \
  "-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer" \
  "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined" "-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address,undefined" \
  -DCMAKE_CXX_FLAGS_DEBUG=-g1 -DCMAKE_C_FLAGS_DEBUG=-g1
cmake --build build/ci-linux-asan --target SparkEngine SparkGameFPS SparkGame SparkTests -j4
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:check_initialization_order=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
LSAN_OPTIONS=suppressions=$PWD/Tests/lsan_suppressions.txt:print_suppressions=0 \
  ctest --test-dir build/ci-linux-asan -L nullrhi-headless --output-on-failure --no-tests=error

# TSan (same pattern, -fsanitize=thread; the CI job's TSAN_OPTIONS)
TSAN_OPTIONS=halt_on_error=0:second_deadlock_stack=1:suppressions=$PWD/Tests/tsan_suppressions.txt \
  ctest --test-dir build/ci-linux-tsan -L nullrhi-headless --output-on-failure --no-tests=error

# Ten-minute FPS soak (opt-in registration, Release)
cmake -B build/linux-gcc-release -DSPARK_ENABLE_SOAK_TESTS=ON
ctest --test-dir build/linux-gcc-release -L '^soak$' --output-on-failure --no-tests=error
```

Use `-L '^soak$'`. A bare `-L soak` also matches the `nullrhi-soak` and
`server-soak` labels.

**Results.**

| Run | Result |
|---|---|
| ASan/UBSan/LSan, `nullrhi-headless`, before the fix | **7 of 12 failed.** Every test that runs the real host (lifecycle, all three shutdown scenarios, tick benchmark, soak smoke, FPS arena) exited 1 with a LeakSanitizer report of about 800 bytes in 10 allocations: the `ModuleManager`, its `LoadedModule` and lifecycle-record vectors, and the FPS module object. Headless POSIX teardown deliberately keeps the manager and module images alive until process exit (`ShutdownEngineAfterPreflight`), but it did so with a bare `release()`, which left them unreachable. The soak smoke also failed its RSS ceiling (719 MB/h). |
| Fix | `EngineRuntime::residentModuleManagers` now holds the deliberately process-lifetime manager (one entry per teardown), so it stays reachable. Behavior is unchanged: nothing is unloaded or freed. The two soak tests bound ASan's freed-memory quarantine to 1 MiB (`ENVIRONMENT_MODIFICATION ASAN_OPTIONS=string_append::quarantine_size_mb=1`). A 120 s ASan FPS soak with that bound fitted a slope of -687 MB/h, so the 719 MB/h was the 256 MiB default quarantine filling up, not engine growth. Leak detection stays on, with one caveat: LSan treats all heap reachable from the resident `ModuleManager` and the FPS module (module-owned state not freed at shutdown) as live, so it cannot report module-side leaks on Linux headless. That gap is tracked with the modules-kept-mapped-at-exit teardown item. The seeded-leak rows below used an unrooted shim and do not cover it. |
| ASan/UBSan/LSan, `nullrhi-headless`, after the fix | **12/12 passed**, no sanitizer report. |
| TSan, `nullrhi-headless` | **12/12 passed**, no ThreadSanitizer warning. The engine links `libtsan.so.2`. |
| `Soak_FPSHeadlessNullRHI` (Release, 600 s, 36000 ticks) | **Passed twice.** On the final code (after the fix above): 608 s, leak slope 0 B/h over 446 VmRSS samples (the provisional ceiling is 64 MiB/h), max heartbeat gap 0.20 s, peak RSS 32468 KiB, `SPARK_HEADLESS_NULLRHI_RESOURCES live=0` (a bridge/device teardown guard; see the scope note above). The earlier run, before the fix: slope -30.9 MB/h, peak RSS 32324 KiB, live=0. |
| Release `nullrhi-headless` after the fix | **12/12 passed.** The related Release suites (`-R "Lifecycle|RHIBridge|NullRHI|EngineRuntime|Shutdown"`, 16 CTest entries) also passed. |
| Seeded leak, LSan | An `LD_PRELOAD` shim (a thread that `malloc`s 64 KiB every 16 ms and drops the pointer), loaded into the ASan host running FPS for 120 frames, exited 1: LeakSanitizer reported `35913728 byte(s) leaked in 548 allocation(s)` from the shim's `leak_loop`. The same command without the shim exited 0 with no report. |
| Seeded leak, soak threshold | The same shim under `run_nullrhi_soak.py --duration 90` failed with `leak: RSS slope 14549534873 B/h` (Release) and `17749407897 B/h` (ASan, quarantine bounded; that run also exited 1 from LSan). Both are far above the 64 MiB/h ceiling. |
| Seeded NullRHI resource leak | `NullRHI_Lifetime_ShutdownReportsResourcesHeldPastTeardown` and `NullRHI_Lifetime_BridgeShutdownReportsHeadlessLeaks` hold resources past device and bridge shutdown and expect counts of 2 and 1. `HarnessTests.test_resources_held_past_device_shutdown_fail` proves that the soak harness fails on `live=3`. |

**Still missing (unchanged owners).** A NullRHI save/reload test; any Windows
package, clean-machine or no-display-host run; the hosted
`headless-windows-package` and `headless-windows-soak` jobs; soaks longer than
10 minutes (the one-hour `headless-soak-1h` scene is required for a PERF-100
`nullrhi.soak.*` result); and budgets. The Windows host does not yet print
`SPARK_HEADLESS_NULLRHI_RESOURCES`. The Linux headless host still keeps
modules mapped at exit instead of unloading them. The late-shutdown crash that
motivates this was not re-investigated.

## 7. Known broken / not fixed here (with owner)

1. **VisualScript assets are staged in the wrong place on Linux** (build lane).
   Module POST_BUILD copies use `$<TARGET_FILE_DIR:module>`. On Linux that is
   `lib/` (a library output), and the `.so` is then copied to `bin/`, so
   `Assets/Scripts/Generated` never sits next to the engine. The same pattern
   stages other modules' `Shaders/` and `Assets/` into `lib/`. Fix: stage to the
   runtime (`bin/`) directory.
2. **`SparkTests` link failure with CMake 3.28.3** (§3, build lane).
3. ~~**Engine OpenGL RHI falls back to NullRHI under Xvfb**~~ **Fixed.** The
   cause was the unconditional `SDL_VIDEO_X11_FORCE_EGL=1` on a host without
   `libEGL`, and window-creation failures were silently downgraded to NullRHI.
   See §6 for the fix and the regression test.
4. Engine log lines emitted after "Loading module" through `SimpleConsole` are
   not flushed to the `SparkConsole` child before shutdown. A failed module
   load's reason appears only with `-no-subprocess` or under a debugger
   (Core/Utils console lane; not investigated further).
5. The Windows `Process` detached launch also passes `bInheritHandles=TRUE`.
   Whether it has the same stdio-inheritance hang (#4) was **not verified**,
   because there is no Windows host here.
6. **Headless and server binaries still need GL and X11 at load time**
   (build/RHI lane). `SparkEngine`, `SparkServer` and `SparkGateway` link
   `libGL.so.1` and `libX11.so.6` directly (§6.1). `SparkEngine -headless` and
   the dedicated server therefore cannot start on a minimal host without the
   GL and X11 client libraries, even though they never open a window or a GL
   context. No display is needed, only the libraries. Fix: move the GL/X11
   dependency behind the windowed backends, or load it at runtime.

## 8. Untested (no evidence either way)

Clean-machine package, install, upgrade, rollback and uninstall; CPack and
installer output on Linux; the installed-tree RUNPATH/NEEDED closure on any
host but the one in §6.1; desktop integration; any real GPU or driver (Vulkan,
OpenGL hardware); Wayland; audio output; physical input devices; multiplayer
across hosts; Clang, GCC 14, Debug and sanitizer configurations (except the
`nullrhi-headless` label in §6.2), and Shipping beyond the §6.1
install-closure run (the full test suite ran on GCC 13 Release only); distributions other than Ubuntu 24.04; ARM64; running outside gVisor.

## 9. Bounded support statement

On **Ubuntu 24.04 x86-64 with GCC 13.3 in Release**, SparkEngine **builds**
(all product targets and 11 game modules). Its **test suite passes** (83/83
CTest entries, 7289 in-binary tests, 0 failures), but only when `SparkTests`
is relinked around the CMake 3.28 duplicate whole-archive defect. **Headless
runtime** loads 10 of 11 game modules end to end, and the eleventh
(`SparkGameVisualScript`) loads once its assets are staged. The **editor starts
on Mesa software OpenGL**. Linux engine rendering, packaging, installation,
GPUs, audio and input are **unverified**. Linux therefore remains
**experimental / not certified**, and must not be summarized as supported.

## Source & Freshness

Produced by the PLT-210 cloud lane on 2026-09-24 from `claude/cloud-plt-210`
(base `b5debbd43`, rebased onto `8738dff59`) on the host described in §1. The
counts come from CTest JUnit and `SparkTests-junit.xml` on that host; this is
not CI evidence and not same-SHA CI evidence. Re-run §2 to refresh. §6.2 was
added on 2026-09-25 by the HEAD-220 lane from local ASan, TSan and Release
runs on the same host; re-run its commands to refresh it.
