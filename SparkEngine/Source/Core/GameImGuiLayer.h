/**
 * @file GameImGuiLayer.h
 * @brief Game-mode (non-editor) ImGui overlay for the SparkEngine executable.
 *
 * The engine runtime historically only drove ImGui inside SparkEditor, so
 * IModule::OnImGui() was never called in plain game mode and module HUDs were
 * invisible. This layer owns a Dear ImGui context + Win32/DX11 backends in the
 * ENGINE EXECUTABLE and renders module UI right before Present via the
 * GraphicsEngine pre-present hook.
 *
 * Cross-DLL contract: game module DLLs statically link their own copy of the
 * ImGui object code, so the exe's context/allocators are injected into each
 * module through its exported SparkModuleInjectImGui() (see ModuleDllMain.h
 * and ModuleManager::LoadModule) — identical mechanism to the console
 * injection. Without the injection the module's ImGui calls would hit a null
 * DLL-private GImGui and crash.
 *
 * Compiled only into the SparkEngine executable when SPARK_HAS_IMGUI is
 * defined (see SPARK_ENGINE_ENTRY_POINTS in the root CMakeLists).
 */
#pragma once

#include "Platform.h"

#if defined(SPARK_PLATFORM_WINDOWS)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

struct ID3D11Device;
struct ID3D11DeviceContext;
class ModuleManager;

namespace Spark::GameImGui
{
    /// Create the ImGui context and init the Win32 + DX11 backends. No-op if
    /// already initialized. Returns false when ImGui support is compiled out.
    bool Init(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* context);

    /// Destroy backends + context. Safe to call when not initialized.
    void Shutdown();

    bool IsInitialized();

    /// Full overlay pass: backend NewFrame -> modules' OnImGui -> Render.
    /// Called from the GraphicsEngine pre-present hook (exe code).
    void RenderOverlay(ModuleManager* modules);

    /// Injection payload for module DLLs (exe ImGui context + allocators).
    void GetInjectionData(void** outContext, void** outAllocFn, void** outFreeFn, void** outUserData);

    /// Forward a window message to the ImGui Win32 backend.
    /// Returns true if ImGui consumed the message.
    bool HandleWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
} // namespace Spark::GameImGui

#endif // SPARK_PLATFORM_WINDOWS
