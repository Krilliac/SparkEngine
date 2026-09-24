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
| Base commit | `b5debbd43` (`claude/stable-v1-release`) |
| Evidence head | `claude/cloud-plt-210`. The fixes are commits `5a823b2`, `9e41a4c`, `8cb50f1` and `3e5da14`, on top of the base. The "after" run below was built from `3e5da14`; this document was added in the next commit. |
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
the base source before this run). "After" means head `3e5da14`.

| Suite | Before | After |
|---|---|---|
| CTest entries | **83 / 83 passed**, 0 failed, 0 not run (65.6 s) | **83 / 83 passed**, 0 failed, 0 not run (63.2 s) |
| `SparkEngineTests` (in-binary tests, JUnit) | 7277 run, **0 failed, 9 skipped** | 7289 run (+12 new PLT-210 tests), **0 failed, 9 skipped** |
| `SparkEngineLoadTests` (load lane) | 22 / 22 | 22 / 22 |

The 9 skips are the same before and after. All are declared skips:
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
* **Engine rendering on Linux was not demonstrated.** Without a backend
  override, the SDL2 path chose `NullRHIDevice` because of the gVisor
  detection. With `SPARK_RHI_BACKEND=opengl` under Xvfb, the RHI still logged
  "No graphics backend available — falling back to NullRHIDevice", with no
  reason given. Owner: RHI lane (`Graphics/RHI/**`, forbidden to this lane);
  the cause is unknown.
* Audio: the OpenAL Soft backend initialized. There was no audio device and
  no audible output was verified.

## 7. Known broken / not fixed here (with owner)

1. **VisualScript assets are staged in the wrong place on Linux** (build lane).
   Module POST_BUILD copies use `$<TARGET_FILE_DIR:module>`. On Linux that is
   `lib/` (a library output), and the `.so` is then copied to `bin/`, so
   `Assets/Scripts/Generated` never sits next to the engine. The same pattern
   stages other modules' `Shaders/` and `Assets/` into `lib/`. Fix: stage to the
   runtime (`bin/`) directory.
2. **`SparkTests` link failure with CMake 3.28.3** (§3, build lane).
3. **Engine OpenGL RHI falls back to NullRHI under Xvfb** (§6, RHI lane,
   unexplained).
4. Engine log lines emitted after "Loading module" through `SimpleConsole` are
   not flushed to the `SparkConsole` child before shutdown. A failed module
   load's reason appears only with `-no-subprocess` or under a debugger
   (Core/Utils console lane; not investigated further).
5. The Windows `Process` detached launch also passes `bInheritHandles=TRUE`.
   Whether it has the same stdio-inheritance hang (#4) was **not verified**,
   because there is no Windows host here.

## 8. Untested (no evidence either way)

Clean-machine package, install, upgrade, rollback and uninstall; CPack and
installer output on Linux; RPATH/`$ORIGIN` of an installed tree; desktop
integration; any real GPU or driver (Vulkan, OpenGL hardware); Wayland; audio
output; physical input devices; multiplayer across hosts; Clang, GCC 14,
Debug, sanitizer and Shipping configurations (only GCC 13 Release was run);
distributions other than Ubuntu 24.04; ARM64; running outside gVisor.

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
(base `b5debbd43`, fixes through `3e5da14`) on the host described in §1. The
counts come from CTest JUnit and `SparkTests-junit.xml` on that host; this is
not CI evidence and not same-SHA CI evidence. Re-run §2 to refresh.
