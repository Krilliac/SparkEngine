/**
 * @file ModuleDllMain.h
 * @brief Standard DllMain entry point for Spark module DLLs.
 *
 * Include this header in exactly one .cpp file per module DLL (typically the
 * same file that invokes SPARK_IMPLEMENT_MODULE). On non-Windows platforms
 * this header is a no-op.
 *
 * Rationale: every game module DLL on Windows needs a DllMain that calls
 * DisableThreadLibraryCalls to suppress per-thread DLL_THREAD_ATTACH
 * notifications. Rather than duplicating the same 15-line boilerplate in
 * every module's Main.cpp, this header defines the canonical version once.
 *
 * Usage:
 * @code
 *   // MyGameModule/Main.cpp
 *   #include "MyGameModule.h"
 *   #include <Spark/ModuleDllMain.h>
 *
 *   SPARK_IMPLEMENT_MODULE(MyGameModule)
 * @endcode
 *
 * If this header is included in more than one translation unit within the
 * same DLL, the linker will report a duplicate DllMain symbol — the right
 * failure mode, since DllMain must be unique per module.
 */

#pragma once

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

extern "C" BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        ::DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}

namespace Spark
{
    class SimpleConsole;
}

namespace Spark::Detail
{
    // Defined in SparkConsole.cpp (statically linked into this DLL).
    void InjectConsoleInstance(SimpleConsole* instance);
}

/**
 * @brief Host-console injection hook, called by ModuleManager after load.
 *
 * Windows module DLLs statically link SparkEngineLib and would otherwise
 * register their console commands into a DLL-private SimpleConsole copy that
 * the engine never reads (module commands were silently dead on Windows).
 */
extern "C" __declspec(dllexport) void SparkModuleInjectConsole(void* hostConsole)
{
    Spark::Detail::InjectConsoleInstance(static_cast<Spark::SimpleConsole*>(hostConsole));
}

#endif // _WIN32
