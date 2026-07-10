/**
 * @file GameImGuiLayer.cpp
 * @brief Game-mode ImGui overlay implementation (exe-only, Windows + DX11).
 */
#include "GameImGuiLayer.h"

#if defined(SPARK_PLATFORM_WINDOWS)

#include "ModuleManager.h"
#include "../Utils/LogMacros.h"
#include "../Utils/SparkConsole.h"

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif

namespace Spark::GameImGui
{

#ifdef SPARK_HAS_IMGUI

    namespace
    {
        bool g_initialized = false;
        bool g_inOverlay = false;
    } // namespace

    bool Init(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* context)
    {
        if (g_initialized)
            return true;
        if (!hwnd || !device || !context)
            return false;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.IniFilename = nullptr; // game HUD: don't persist window layout
        ImGui::StyleColorsDark();

        if (!ImGui_ImplWin32_Init(hwnd))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "GameImGui: ImGui_ImplWin32_Init failed");
            ImGui::DestroyContext();
            return false;
        }
        if (!ImGui_ImplDX11_Init(device, context))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "GameImGui: ImGui_ImplDX11_Init failed");
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            return false;
        }

        g_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Core, "GameImGui: initialized (game-mode HUD overlay active)");
        return true;
    }

    void Shutdown()
    {
        if (!g_initialized)
            return;
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_initialized = false;
    }

    bool IsInitialized()
    {
        return g_initialized;
    }

    void RenderOverlay(ModuleManager* modules)
    {
        if (!g_initialized || g_inOverlay)
            return;
        g_inOverlay = true;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (modules)
            modules->ImGuiAll();

        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // One-shot diagnostics: confirm the overlay runs and whether module UI
        // actually submitted geometry (0 vertices => injection/HUD gating issue).
        static bool s_loggedOnce = false;
        if (!s_loggedOnce)
        {
            s_loggedOnce = true;
            const ImDrawData* dd = ImGui::GetDrawData();
            Spark::SimpleConsole::GetInstance().LogInfo(
                "GameImGui overlay first frame: " + std::to_string(dd ? dd->TotalVtxCount : -1) + " verts, " +
                std::to_string(dd ? dd->CmdListsCount : -1) + " cmd lists");
        }

        g_inOverlay = false;
    }

    void GetInjectionData(void** outContext, void** outAllocFn, void** outFreeFn, void** outUserData)
    {
        *outContext = nullptr;
        *outAllocFn = nullptr;
        *outFreeFn = nullptr;
        *outUserData = nullptr;
        if (!g_initialized)
            return;

        ImGuiMemAllocFunc allocFn = nullptr;
        ImGuiMemFreeFunc freeFn = nullptr;
        void* userData = nullptr;
        ImGui::GetAllocatorFunctions(&allocFn, &freeFn, &userData);

        *outContext = ImGui::GetCurrentContext();
        *outAllocFn = reinterpret_cast<void*>(allocFn);
        *outFreeFn = reinterpret_cast<void*>(freeFn);
        *outUserData = userData;
    }

    bool HandleWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (!g_initialized)
            return false;
        return ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam) != 0;
    }

    bool WantsMouse()
    {
        return g_initialized && ImGui::GetIO().WantCaptureMouse;
    }

    bool WantsKeyboard()
    {
        return g_initialized && ImGui::GetIO().WantCaptureKeyboard;
    }

#else // !SPARK_HAS_IMGUI — stubs so the exe links when ImGui is absent

    bool Init(HWND, ID3D11Device*, ID3D11DeviceContext*)
    {
        return false;
    }
    void Shutdown() {}
    bool IsInitialized()
    {
        return false;
    }
    void RenderOverlay(ModuleManager*) {}
    void GetInjectionData(void** outContext, void** outAllocFn, void** outFreeFn, void** outUserData)
    {
        *outContext = nullptr;
        *outAllocFn = nullptr;
        *outFreeFn = nullptr;
        *outUserData = nullptr;
    }
    bool HandleWndProc(HWND, UINT, WPARAM, LPARAM)
    {
        return false;
    }
    bool WantsMouse()
    {
        return false;
    }
    bool WantsKeyboard()
    {
        return false;
    }

#endif // SPARK_HAS_IMGUI

} // namespace Spark::GameImGui

#endif // SPARK_PLATFORM_WINDOWS
