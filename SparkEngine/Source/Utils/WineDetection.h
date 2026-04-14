/**
 * @file WineDetection.h
 * @brief Detect whether a Windows build is running under Wine
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
 * @note On non-Windows builds the helpers are hard-coded to return false /
 * empty. They still compile so client code can `#include` this header
 * unconditionally.
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
     * @brief Write a one-line informational log about the Wine environment,
     *        or nothing when not running under Wine.
     *
     * Intended to be called once at engine startup, right after the Logger
     * is initialized. Safe to call on non-Windows builds (it's a no-op).
     */
    void LogWineEnvironmentIfApplicable();

} // namespace Spark
