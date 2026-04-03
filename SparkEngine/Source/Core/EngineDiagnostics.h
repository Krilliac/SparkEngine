/**
 * @file EngineDiagnostics.h
 * @brief Runtime diagnostic commands that exercise live engine subsystems
 *
 * Provides console commands (diag_all, diag_physics, diag_weather, etc.)
 * that perform real operations on engine systems and report pass/fail results.
 * Unlike unit tests, these run inside the live engine and validate that
 * systems are initialized, wired, and functioning correctly at runtime.
 */

#pragma once

#include <string>
#include <vector>

namespace Spark
{

    class SimpleConsole;

    /// @brief Result of a single diagnostic check.
    struct DiagResult
    {
        std::string subsystem;
        std::string testName;
        bool passed = false;
        std::string detail;
    };

    /// @brief Summary of a full diagnostic run.
    struct DiagReport
    {
        std::vector<DiagResult> results;
        int passed = 0;
        int failed = 0;

        void Add(const std::string& subsystem, const std::string& name, bool ok, const std::string& detail = "")
        {
            results.push_back({subsystem, name, ok, detail});
            if (ok)
                ++passed;
            else
                ++failed;
        }

        std::string Format() const;
    };

    // --- Core subsystem diagnostics (EngineDiagnostics.cpp) ---
    void DiagPhysics(DiagReport& report);
    void DiagWeather(DiagReport& report);
    void DiagTimeOfDay(DiagReport& report);
    void DiagECS(DiagReport& report);
    void DiagEventBus(DiagReport& report);
    void DiagSaveSystem(DiagReport& report);
    void DiagNetworking(DiagReport& report);
    void DiagAbilities(DiagReport& report);
    void DiagDialogue(DiagReport& report);
    void DiagCoroutines(DiagReport& report);
    void DiagConditions(DiagReport& report);
    void DiagInstances(DiagReport& report);
    void DiagTweens(DiagReport& report);
    void DiagFileCache(DiagReport& report);
    void DiagVFS(DiagReport& report);
    void DiagMemory(DiagReport& report);

    // --- Extended subsystem diagnostics (EngineDiagnosticsExtended.cpp) ---
    void DiagRHI(DiagReport& report);
    void DiagJobSystem(DiagReport& report);
    void DiagAISystems(DiagReport& report);
    void DiagDebugSystems(DiagReport& report);
    void DiagRenderingSystems(DiagReport& report);
    void DiagStreaming(DiagReport& report);
    void DiagDestruction(DiagReport& report);
    void DiagFreezeSystem(DiagReport& report);
    void DiagScripting(DiagReport& report);
    void DiagCpuDebugger(DiagReport& report);
    void DiagCacheDebugger(DiagReport& report);
    void DiagIODebugger(DiagReport& report);
    void DiagThreadDebugger(DiagReport& report);
    void DiagWatchdog(DiagReport& report);
    void DiagHitchDetector(DiagReport& report);
    void DiagAssetStall(DiagReport& report);
    void DiagNetworkHealth(DiagReport& report);
    void DiagGPUResourceLeak(DiagReport& report);

    /// @brief Run ALL diagnostics (core + extended) into a single report.
    void DiagRunAll(DiagReport& report);

    /// @brief Register all diag_* console commands (core + extended).
    void RegisterDiagnosticCommands(SimpleConsole& console);

} // namespace Spark
