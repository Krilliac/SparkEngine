/**
 * @file GameImGuiLayer.cpp
 * @brief Game-mode ImGui overlay implementation (exe-only, Windows + DX11).
 *
 * Styling: the overlay installs the Spark UI "Runtime" tokens (Ember, HUD metrics) on its context,
 * loads the Spark font faces from <exe>/EditorAssets/Fonts, and publishes a SparkUiRuntimeBlock
 * through ImGuiIO::UserData so injected game modules can read the theme. The executable stays
 * DPI-unaware: the only scale is EngineSettings uiScale. Every SparkUI include and call lives
 * inside SPARK_HAS_IMGUI because the Linux executable has no ImGui.
 */
#include "GameImGuiLayer.h"

#if defined(SPARK_PLATFORM_WINDOWS)

#include "ModuleManager.h"
#include "../Utils/LogMacros.h"

#ifdef SPARK_HAS_IMGUI
#include "EngineSettings.h"
#include "RuntimePackage.h"

#include <SparkUI/Fonts.h>
#include <SparkUI/Runtime.h>
#include <SparkUI/Tokens.h>
#include <SparkUI/Widgets.h>
#include <Spark/UiTheme.h>

#include <cstddef>

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
        void (*g_auxPanel)() = nullptr;

        /// Opacity of the overlay window background (SurfaceBase) so the 3D scene shows through HUD panels.
        constexpr float kOverlayWindowAlpha = 0.94f;

        /// EngineSettings values that shape the overlay theme; compared every frame to pick up live changes.
        struct OverlaySettings
        {
            float uiScale = 1.0f;
            bool highContrast = false;
            bool reduceMotion = false;

            bool operator==(const OverlaySettings&) const = default;
        };

        /// Settings applied to the context so far (main/render thread only).
        OverlaySettings g_appliedSettings;

        /// Theme snapshot published to game modules through ImGuiIO::UserData; owned by the executable and valid
        /// for as long as the context exists (UserData is cleared before the context is destroyed).
        Spark::Ui::Sdk::SparkUiRuntimeBlock g_runtimeBlock;

        OverlaySettings ReadOverlaySettings()
        {
            const EngineSettings& settings = EngineSettings::GetInstance();
            return {settings.UI().uiScale, settings.Accessibility().highContrast,
                    settings.Accessibility().reduceMotion};
        }

        /// Ember tokens with the in-game (HUD) metrics profile; the window background is translucent except in
        /// High Contrast, which stays opaque.
        Spark::Ui::Tokens BuildOverlayTokens(bool highContrast)
        {
            namespace Ui = Spark::Ui;
            Ui::Tokens tokens = Ui::GenerateTokens(Ui::BuiltinSeed(Ui::PaletteId::Ember),
                                                   highContrast ? Ui::Appearance::HighContrast : Ui::Appearance::Dark,
                                                   Ui::Density::Standard);
            // The HUD profile replaces the spacing only; High Contrast keeps its 3 px focus ring.
            const float focusRing = tokens.metrics.focusRing;
            tokens.metrics = Ui::MetricsFor(Ui::Density::Standard, Ui::MetricsProfile::Hud);
            tokens.metrics.focusRing = focusRing;
            if (!highContrast)
            {
                tokens.color[static_cast<std::size_t>(Ui::Role::SurfaceBase)].a = kOverlayWindowAlpha;
            }
            return tokens;
        }

        /// Refresh the module-facing block from the active tokens and point ImGuiIO::UserData at it.
        void PublishRuntimeBlock()
        {
            Spark::Ui::FillSdkRuntimeBlock(g_runtimeBlock);
            ImGui::GetIO().UserData = &g_runtimeBlock;
        }

        /// Re-apply the theme when EngineSettings changed since the last frame. Runs before NewFrame so the
        /// scale never changes mid-frame; the font atlas is never reloaded (dynamic fonts follow the scale).
        void SyncOverlayTheme()
        {
            const OverlaySettings wanted = ReadOverlaySettings();
            if (wanted == g_appliedSettings)
            {
                return;
            }
            if (wanted.highContrast != g_appliedSettings.highContrast)
            {
                Spark::Ui::SetTokens(BuildOverlayTokens(wanted.highContrast));
            }
            Spark::Ui::RequestScale(1.0f, wanted.uiScale);
            Spark::Ui::BeginFrameScale();
            Spark::Ui::SetReduceMotion(wanted.reduceMotion);
            g_appliedSettings = wanted;
            PublishRuntimeBlock();
        }
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

        // Theme, fonts and the module-facing block are all in place before the backends create their device
        // objects and before ModuleManager::SetImGuiInjection hands the context to module DLLs.
        g_appliedSettings = ReadOverlaySettings();
        Spark::Ui::Install(ImGui::GetCurrentContext(), BuildOverlayTokens(g_appliedSettings.highContrast), 1.0f,
                           g_appliedSettings.uiScale, g_appliedSettings.reduceMotion);
        Spark::Ui::InstallFocusRing(ImGui::GetCurrentContext());
        const Spark::Ui::FontSet fonts = Spark::Ui::LoadFontSet(
            *io.Fonts, Spark::Ui::FindFontDirectory(Spark::RuntimePackage::GetExecutableDirectory()),
            Spark::Ui::ActiveTokens().type.body);
        io.FontDefault = fonts.body;
        if (!fonts.diagnostic.empty())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "GameImGui: font substitutions: %s", fonts.diagnostic.c_str());
        }
        PublishRuntimeBlock();

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
        ImGui::GetIO().UserData = nullptr;
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

        SyncOverlayTheme();
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (modules)
            modules->ImGuiAll();

        if (g_auxPanel)
            g_auxPanel();

        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // One-shot diagnostics: confirm the overlay runs and whether module UI
        // actually submitted geometry (0 vertices => injection/HUD gating issue).
        // Routed through the Logger so it lands in Logs/SparkEngine_*.log (where
        // release checks grep for it) as well as the console via ConsoleSink.
        static bool s_loggedOnce = false;
        if (!s_loggedOnce)
        {
            s_loggedOnce = true;
            const ImDrawData* dd = ImGui::GetDrawData();
            SPARK_LOG_INFO(Spark::LogCategory::Core, "GameImGui overlay first frame: %d verts, %d cmd lists",
                           dd ? dd->TotalVtxCount : -1, dd ? dd->CmdListsCount : -1);
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

    void SetAuxPanel(void (*panelFn)())
    {
        g_auxPanel = panelFn;
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
    void SetAuxPanel(void (*)()) {}

#endif // SPARK_HAS_IMGUI

} // namespace Spark::GameImGui

#endif // SPARK_PLATFORM_WINDOWS
