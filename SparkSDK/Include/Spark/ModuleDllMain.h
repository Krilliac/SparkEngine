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
} // namespace Spark::Detail

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

// EngineContext lives in the global namespace; this header declares its static
// SetInjected() setter. Module translation units have the engine source tree on
// their include path, so the quoted include resolves (mirrors the mid-file
// <imgui.h> include below).
#include "Core/EngineContext.h"

/**
 * @brief Host-EngineContext injection hook, called by ModuleManager after load.
 *
 * Windows module DLLs statically link SparkEngineLib, so the g_engineContext
 * global that backs EngineContext::Get() is a per-image copy that is null inside
 * the module — service-locator lookups there (e.g. NetworkManager) would otherwise
 * fall back to dead per-module singletons. This export points the module's
 * EngineContext::Get() at the host executable's live context. The pointer is stored
 * non-owning, so module teardown / FreeLibrary never frees the host context.
 */
#include "Physics/PhysicsSystem.h"

extern "C" __declspec(dllexport) void SparkModuleInjectEngineContext(void* ctx)
{
    EngineContext::SetInjected(static_cast<EngineContext*>(ctx));
    // Jolt keeps per-image globals (allocator fn pointers, Factory::sInstance,
    // RegisterTypes dispatch tables). This module image must register ITS
    // copies before any module-side shape creation or collision query — the
    // exe's registration does not reach this DLL. Idempotent, ~free.
    PhysicsSystem::EnsureImageRuntime();
}

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

/**
 * @brief Host-ImGui injection hook, called by ModuleManager after load.
 *
 * Windows module DLLs statically link their own copy of the ImGui object code,
 * so GImGui (the current-context global) and the allocator function pointers
 * are per-image. The engine executable owns the one real context (game-mode
 * overlay, see GameImGuiLayer); this export points the DLL's ImGui at it so
 * module OnImGui() calls record into the shared context. Allocators are
 * injected too so ImGui memory is always allocated and freed by the same
 * functions regardless of which image triggered the (de)allocation.
 *
 * Modules built without ImGui support keep the export as a no-op (the engine
 * probes it via GetProcAddress either way).
 */
extern "C" __declspec(dllexport) void SparkModuleInjectImGui(void* context, void* allocFn, void* freeFn, void* userData)
{
#ifdef SPARK_HAS_IMGUI
    ImGui::SetAllocatorFunctions(reinterpret_cast<ImGuiMemAllocFunc>(allocFn),
                                 reinterpret_cast<ImGuiMemFreeFunc>(freeFn), userData);
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(context));
#else
    (void)context;
    (void)allocFn;
    (void)freeFn;
    (void)userData;
#endif
}

#endif // _WIN32
