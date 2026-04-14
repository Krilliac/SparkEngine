/**
 * @file WineDetection.h
 * @brief Detect whether a Windows build is running under Wine (and whether
 *        the host kernel is gVisor's runsc user-mode kernel)
 *
 * SparkEngine on Linux targets MinGW (via `cmake --preset linux-mingw-release`)
 * for end-to-end Windows code testing without a Windows host. At runtime the
 * resulting .exe can be run either on native Windows (MSVC or MinGW output)
 * or under Wine. A handful of subsystems need to know which environment
 * they're in — D3D11 feature probing, XInput/XAudio2 callbacks, crash-handler
 * behavior, and anything that interacts with GPU drivers (DXVK vs native
 * d3d11.dll) — so SparkEngine exposes `IsRunningUnderWine()` for the whole
 * engine to call.
 *
 * Detection is via `ntdll.dll` exporting `wine_get_version`. This is the
 * standard public hook Wine provides specifically for apps that want to
 * detect or version-check the Wine runtime. On native Windows `wine_get_version`
 * does not exist so `GetProcAddress` returns NULL and `IsRunningUnderWine()`
 * returns false.
 *
 * In addition, `IsRunningUnderGvisor()` probes for gVisor's `runsc` user-mode
 * kernel. gVisor interferes with Wine's signal handling (the ucontext
 * `trap_no` field is not populated, causing Wine's segv_handler to loop with
 * "Got unexpected trap 0"). See
 * `.claude/knowledge/wine-gvisor-incompatibility.md` and
 * `.claude/knowledge/wine-role-and-fallback-tiers-2026-04-14.md`. Subsystems
 * that care (the RHI factory when auto-picking a backend, the crash handler,
 * the Wine launcher probe) can query this at startup and select a safer
 * fallback tier automatically.
 *
 * @note On non-Windows builds `IsRunningUnderWine()` / `GetWineVersion()` are
 * hard-coded to return false / empty. `IsRunningUnderGvisor()` still probes
 * `/proc` on Linux builds, because the engine's native Linux launcher (used
 * by the tier-4 fallback in the Wine ladder) also benefits from knowing it's
 * inside runsc. They still compile on all platforms so client code can
 * `#include` this header unconditionally.
 */

#pragma once

#include <string>

namespace Spark
{

    /**
     * @brief True if the process is running under Wine (any version).
     *
     * Result is cached after the first call; subsequent calls are a
     * pointer dereference. Thread-safe (magic statics).
     *
     * @return true when running on Wine, false on native Windows or
     *         non-Windows platforms.
     */
    bool IsRunningUnderWine();

    /**
     * @brief The Wine version string reported by `wine_get_version`, or an
     *        empty string when not running under Wine.
     *
     * Example: `"9.0"`, `"10.0-rc3"`, `"10.2 (Staging)"`. Useful for log
     * banners and compatibility workarounds.
     */
    const std::string& GetWineVersion();

    /**
     * @brief True if the process is running inside gVisor's `runsc` user-mode
     *        kernel.
     *
     * Detection logic (in order, first match wins):
     *   1. On Linux / MinGW-under-Wine hosts: read `/proc/1/comm` — gVisor's
     *      PID 1 is `runsc` (or `runsc-sandbox`).
     *   2. Fallback: scan `/proc/self/maps` for the string `runsc`, which
     *      appears in the vDSO / stack entry names under gVisor.
     *   3. Environment override: `SPARK_FORCE_GVISOR=1` forces true,
     *      `SPARK_FORCE_GVISOR=0` forces false. Useful for tests and for
     *      machines where the probe is incorrect.
     *
     * Result is cached after the first call; subsequent calls are a pointer
     * dereference. Thread-safe (magic statics).
     *
     * @return true when running under gVisor, false otherwise (including
     *         native Windows — gVisor does not host Windows syscalls).
     */
    bool IsRunningUnderGvisor();

    /**
     * @brief Write a one-line informational log about the Wine environment,
     *        or nothing when not running under Wine. Also logs a warning if
     *        the process is running inside gVisor, because Wine's signal
     *        handling is known-incompatible with `runsc` and subsystems
     *        should fall back to tier-2/tier-3/tier-4 of the software
     *        rendering ladder.
     *
     * Intended to be called once at engine startup, right after the Logger
     * is initialized. Safe to call on non-Windows builds (the Wine branch is
     * a no-op; the gVisor branch still runs on native Linux).
     */
    void LogWineEnvironmentIfApplicable();

} // namespace Spark
