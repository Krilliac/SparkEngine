/**
 * @file ProjectSettingsPanel.cpp
 * @brief Implementation of the project settings editor panel
 */

#include "ProjectSettingsPanel.h"
#include "../Core/EditorIcons.h"
#include "Core/EngineSettings.h"
#include "SparkUI/Widgets.h"
#include "Utils/LogMacros.h"

#include <array>
#include <cstddef>
#include <cstdio>
#include <string_view>
#include <utility>

#include <imgui.h>

namespace SparkEditor
{
    namespace
    {
        constexpr const char* kResetPopupId = "Reset project settings?##ProjectSettings";

        /**
         * @brief RAII Spark::Ui property grid for one settings section.
         *
         * When BeginPropertyGrid() declines (clipped window) the rows still render, stacked, because
         * PropertyLabel() falls back to the stacked layout without an open grid.
         */
        class ScopedPropertyGrid
        {
          public:
            explicit ScopedPropertyGrid(const char* id) : m_open(Spark::Ui::BeginPropertyGrid(id)) {}
            ~ScopedPropertyGrid()
            {
                if (m_open)
                {
                    Spark::Ui::EndPropertyGrid();
                }
            }
            ScopedPropertyGrid(const ScopedPropertyGrid&) = delete;
            ScopedPropertyGrid& operator=(const ScopedPropertyGrid&) = delete;

          private:
            bool m_open;
        };

        /**
         * @brief Starts one property-grid row: the visible part of @p label (before any "##") goes
         *        in the label column and Id() is the hidden id of the value-column control.
         *
         * Fixed buffers keep the per-frame row free of heap allocations; every label in this panel is
         * a short literal well under the capacity (longer text is truncated, never overflowed).
         */
        class GridRow
        {
          public:
            explicit GridRow(const char* label)
            {
                const std::string_view full(label);
                const std::string_view visible = full.substr(0, full.find("##"));
                std::snprintf(m_visible.data(), m_visible.size(), "%.*s", static_cast<int>(visible.size()),
                              visible.data());
                std::snprintf(m_id.data(), m_id.size(), "##%s", label);
                Spark::Ui::PropertyLabel(m_visible.data());
            }

            [[nodiscard]] const char* Id() const { return m_id.data(); }

          private:
            static constexpr std::size_t kCapacity = 128;
            std::array<char, kCapacity> m_visible{};
            std::array<char, kCapacity + 2> m_id{};
        };

        /// Property-grid forms of the ImGui value widgets: same arguments, label drawn in the grid.
        namespace Grid
        {
            template <typename... Args> bool DragFloat(const char* label, Args&&... args)
            {
                const GridRow row(label);
                return ImGui::DragFloat(row.Id(), std::forward<Args>(args)...);
            }

            template <typename... Args> bool DragInt(const char* label, Args&&... args)
            {
                const GridRow row(label);
                return ImGui::DragInt(row.Id(), std::forward<Args>(args)...);
            }

            template <typename... Args> bool SliderFloat(const char* label, Args&&... args)
            {
                const GridRow row(label);
                return ImGui::SliderFloat(row.Id(), std::forward<Args>(args)...);
            }

            template <typename... Args> bool SliderInt(const char* label, Args&&... args)
            {
                const GridRow row(label);
                return ImGui::SliderInt(row.Id(), std::forward<Args>(args)...);
            }

            bool Checkbox(const char* label, bool* value)
            {
                const GridRow row(label);
                return ImGui::Checkbox(row.Id(), value);
            }

            template <typename... Args> bool Combo(const char* label, Args&&... args)
            {
                const GridRow row(label);
                return ImGui::Combo(row.Id(), std::forward<Args>(args)...);
            }
        } // namespace Grid
    } // namespace

    ProjectSettingsPanel::ProjectSettingsPanel() : EditorPanel("Project Settings", "project_settings_panel") {}

    bool ProjectSettingsPanel::Initialize()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Initializing Project Settings panel");
        return true;
    }

    void ProjectSettingsPanel::Update(float /*deltaTime*/) {}

    void ProjectSettingsPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            RenderToolbar();
            ImGui::Separator();

            // Tab bar for categories
            if (ImGui::BeginTabBar("SettingsTabs"))
            {
                if (ImGui::BeginTabItem(ICON_FA_DESKTOP " Graphics"))
                {
                    RenderGraphicsTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_CUBE " Rendering"))
                {
                    RenderRenderingTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_MAGIC " Post-Process"))
                {
                    RenderPostProcessingTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_VOLUME_UP " Audio"))
                {
                    RenderAudioTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_MOUSE_POINTER " Controls"))
                {
                    RenderControlsTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_ATOM " Physics"))
                {
                    RenderPhysicsTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_BRAIN " AI"))
                {
                    RenderAITab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_USER " Player"))
                {
                    RenderPlayerTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_GAMEPAD " Gameplay"))
                {
                    RenderGameplayTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_VIDEO " Camera"))
                {
                    RenderCameraTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_WIND " Network"))
                {
                    RenderNetworkTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_CODE " Scripting"))
                {
                    RenderScriptingTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_RUNNING " Animation"))
                {
                    RenderAnimationTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_PENCIL_ALT " Editor"))
                {
                    RenderEditorTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_USER " Accessibility"))
                {
                    RenderAccessibilityTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_GLOBE " VR"))
                {
                    RenderVRTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_BOMB " Destruction"))
                {
                    RenderDestructionDialogueTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_PUZZLE_PIECE " Modding"))
                {
                    RenderModdingLocalizationTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_SAVE " Save/Replay"))
                {
                    RenderSaveReplayTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_FIRE " Particles"))
                {
                    RenderParticlesDecalsTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_MEMORY " Memory"))
                {
                    RenderMemoryTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_GLOBE " Online"))
                {
                    RenderOnlineServicesTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_LIST " Logging"))
                {
                    RenderLoggingTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_INFO_CIRCLE " Project"))
                {
                    RenderProjectInfoTab();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        EndPanel();
    }

    void ProjectSettingsPanel::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Shutting down Project Settings panel");
    }

    // =========================================================================
    // Toolbar
    // =========================================================================

    void ProjectSettingsPanel::RenderToolbar()
    {
        auto& settings = EngineSettings::GetInstance();

        if (!Spark::Ui::BeginPanelToolbar("##ProjectSettingsToolbar"))
        {
            return;
        }

        if (Spark::Ui::PrimaryButton(ICON_FA_SAVE " Save"))
        {
            // Save() reports whether the atomic write actually landed; a read-only or
            // locked settings file must keep the dirty flag and surface the failure.
            if (settings.Save())
            {
                SPARK_LOG_INFO(Spark::LogCategory::Editor, "Project settings saved");
                m_saveError.clear();
                m_modified = false;
            }
            else
            {
                m_saveError = "Failed to save project settings (see log)";
                SPARK_LOG_ERROR(Spark::LogCategory::Editor, "%s", m_saveError.c_str());
            }
        }

        ImGui::SameLine();
        if (Spark::Ui::SecondaryButton(ICON_FA_UNDO " Reset to Defaults"))
        {
            ImGui::OpenPopup(kResetPopupId);
        }
        // Resetting overwrites every category in memory, so it asks first; Save still has to follow.
        if (Spark::Ui::ConfirmDestructive(kResetPopupId, "Reset project settings?",
                                          "Every settings category returns to its engine default. Nothing is written "
                                          "to disk until you press Save.",
                                          "Reset to Defaults"))
        {
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "Project settings reset to defaults");
            settings.ResetToDefaults();
            m_modified = true;
        }

        if (m_modified)
        {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            Spark::Ui::StatusText(Spark::Ui::Status::Warning, "Unsaved changes");
        }

        if (!m_saveError.empty())
        {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            Spark::Ui::StatusText(Spark::Ui::Status::Error, "%s", m_saveError.c_str());
        }

        Spark::Ui::EndPanelToolbar();
    }

    // =========================================================================
    // Graphics Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderGraphicsTab()
    {
        auto& gfx = EngineSettings::GetInstance().Graphics();
        const ScopedPropertyGrid grid("Graphics");

        if (Grid::DragInt("Window Width", &gfx.windowWidth, 1, 640, 7680))
        {
            SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Graphics: window width changed to %d", gfx.windowWidth);
            m_modified = true;
        }
        if (Grid::DragInt("Window Height", &gfx.windowHeight, 1, 480, 4320))
            m_modified = true;
        if (Grid::Checkbox("Fullscreen", &gfx.fullscreen))
        {
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "Graphics: fullscreen %s",
                           gfx.fullscreen ? "enabled" : "disabled");
            m_modified = true;
        }
        if (Grid::Checkbox("VSync", &gfx.vsync))
        {
            SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Graphics: VSync %s", gfx.vsync ? "enabled" : "disabled");
            m_modified = true;
        }

        const char* aaItems[] = {"Off", "2x MSAA", "4x MSAA", "8x MSAA"};
        int aaIndex = 0;
        if (gfx.antiAliasing == 2)
            aaIndex = 1;
        else if (gfx.antiAliasing == 4)
            aaIndex = 2;
        else if (gfx.antiAliasing >= 8)
            aaIndex = 3;
        if (Grid::Combo("Anti-Aliasing", &aaIndex, aaItems, 4))
        {
            const int aaMap[] = {1, 2, 4, 8};
            gfx.antiAliasing = aaMap[aaIndex];
            m_modified = true;
        }

        const char* shadowItems[] = {"Off", "Low", "Medium", "High"};
        if (Grid::Combo("Shadow Quality", &gfx.shadowQuality, shadowItems, 4))
            m_modified = true;

        if (Grid::SliderFloat("Render Scale", &gfx.renderScale, 0.25f, 2.0f, "%.2f"))
            m_modified = true;
        if (Grid::Checkbox("HDR", &gfx.hdr))
            m_modified = true;
    }

    // =========================================================================
    // Rendering Tab (Advanced)
    // =========================================================================

    void ProjectSettingsPanel::RenderRenderingTab()
    {
        auto& r = EngineSettings::GetInstance().Rendering();
        auto& game = EngineSettings::GetInstance().Game();

        if (Spark::Ui::SectionHeader("Pipeline"))
        {
            const ScopedPropertyGrid grid("Pipeline");
            const char* pathItems[] = {"Forward", "Deferred", "Forward+", "Clustered"};
            if (Grid::Combo("Render Path", &r.renderPath, pathItems, 4))
            {
                SPARK_LOG_INFO(Spark::LogCategory::Editor, "Rendering: render path changed to %s",
                               pathItems[r.renderPath]);
                m_modified = true;
            }

            const char* presetItems[] = {"Low", "Medium", "High", "Ultra", "Custom"};
            if (Grid::Combo("Quality Preset", &r.qualityPreset, presetItems, 5))
                m_modified = true;

            if (Grid::Checkbox("Show FPS", &game.showFPS))
                m_modified = true;
            if (Grid::Checkbox("Show Debug Info", &game.showDebugInfo))
                m_modified = true;
        }

        if (Spark::Ui::SectionHeader("Textures & Filtering", nullptr, false))
        {
            const ScopedPropertyGrid grid("Textures & Filtering");
            const char* texSizes[] = {"512", "1024", "2048", "4096", "8192"};
            int texIdx = 2;
            if (r.maxTextureSize <= 512)
                texIdx = 0;
            else if (r.maxTextureSize <= 1024)
                texIdx = 1;
            else if (r.maxTextureSize <= 2048)
                texIdx = 2;
            else if (r.maxTextureSize <= 4096)
                texIdx = 3;
            else
                texIdx = 4;
            if (Grid::Combo("Max Texture Size", &texIdx, texSizes, 5))
            {
                const int sizes[] = {512, 1024, 2048, 4096, 8192};
                r.maxTextureSize = sizes[texIdx];
                m_modified = true;
            }
            if (Grid::Checkbox("Anisotropic Filtering", &r.anisotropicFiltering))
                m_modified = true;
            if (r.anisotropicFiltering)
            {
                if (Grid::SliderInt("Anisotropy Level", &r.anisotropyLevel, 1, 16))
                    m_modified = true;
            }
        }

        if (Spark::Ui::SectionHeader("Shadows & Lighting", nullptr, false))
        {
            const ScopedPropertyGrid grid("Shadows & Lighting");
            if (Grid::Checkbox("Shadows", &r.shadows))
                m_modified = true;
            if (r.shadows)
            {
                const char* shadowSizes[] = {"512", "1024", "2048", "4096"};
                int sIdx = 2;
                if (r.shadowMapSize <= 512)
                    sIdx = 0;
                else if (r.shadowMapSize <= 1024)
                    sIdx = 1;
                else if (r.shadowMapSize <= 2048)
                    sIdx = 2;
                else
                    sIdx = 3;
                if (Grid::Combo("Shadow Map Size", &sIdx, shadowSizes, 4))
                {
                    const int sizes[] = {512, 1024, 2048, 4096};
                    r.shadowMapSize = sizes[sIdx];
                    m_modified = true;
                }
                if (Grid::SliderInt("Cascade Count", &r.cascadeCount, 1, 4))
                    m_modified = true;
            }
        }

        if (Spark::Ui::SectionHeader("Effects Toggles", nullptr, false))
        {
            const ScopedPropertyGrid grid("Effects Toggles");
            if (Grid::Checkbox("Bloom", &r.bloom))
                m_modified = true;
            if (Grid::Checkbox("SSAO", &r.ssao))
                m_modified = true;
            if (Grid::Checkbox("TAA", &r.taa))
                m_modified = true;
            if (Grid::Checkbox("Motion Blur", &r.motionBlur))
                m_modified = true;
        }

        if (Spark::Ui::SectionHeader("Culling & LOD", nullptr, false))
        {
            const ScopedPropertyGrid grid("Culling & LOD");
            if (Grid::Checkbox("Frustum Culling", &r.frustumCulling))
                m_modified = true;
            if (Grid::Checkbox("Occlusion Culling", &r.occlusionCulling))
                m_modified = true;
            if (Grid::Checkbox("Portal Culling", &r.portalCulling))
                m_modified = true;
            if (Grid::Checkbox("Level of Detail", &r.levelOfDetail))
                m_modified = true;
            if (Grid::DragInt("Max Draw Calls", &r.maxDrawCalls, 10, 100, 100000))
                m_modified = true;
        }

        if (Spark::Ui::SectionHeader("Dynamic Quality Scaler", nullptr, false))
        {
            const ScopedPropertyGrid grid("Dynamic Quality Scaler");
            auto& dq = EngineSettings::GetInstance().DynamicQuality();
            if (Grid::Checkbox("Dynamic Quality Enabled", &dq.enabled))
                m_modified = true;
            if (dq.enabled)
            {
                if (Grid::DragFloat("Target Frame Time", &dq.targetFrameTimeMs, 0.5f, 4.0f, 66.0f, "%.1f ms"))
                    m_modified = true;
                Spark::Ui::PropertyLabel("Render Scale Range");
                Spark::Ui::MonoText("%.2f - %.2f (step %.2f)", dq.minRenderScale, dq.maxRenderScale,
                                    dq.renderScaleStep);
                if (Grid::DragFloat("Min Render Scale", &dq.minRenderScale, 0.05f, 0.25f, dq.maxRenderScale))
                    m_modified = true;
                if (Grid::DragFloat("Max Render Scale", &dq.maxRenderScale, 0.05f, dq.minRenderScale, 2.0f))
                    m_modified = true;
                if (Grid::DragFloat("Shadow Scale Min", &dq.minShadowScale, 0.05f, 0.1f, dq.maxShadowScale))
                    m_modified = true;
                if (Grid::DragFloat("Shadow Scale Max", &dq.maxShadowScale, 0.05f, dq.minShadowScale, 1.0f))
                    m_modified = true;
            }
        }

        if (Spark::Ui::SectionHeader("Debug", nullptr, false))
        {
            const ScopedPropertyGrid grid("Debug");
            if (Grid::Checkbox("Wireframe Mode", &r.wireframeMode))
                m_modified = true;
            if (Grid::Checkbox("Debug Mode", &r.debugMode))
                m_modified = true;
            if (Grid::Checkbox("GPU Timing", &r.enableGPUTiming))
                m_modified = true;
        }
    }

    // =========================================================================
    // Post-Processing Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderPostProcessingTab()
    {
        auto& pp = EngineSettings::GetInstance().PostProcess();
        auto& ssao = EngineSettings::GetInstance().SSAO();
        auto& ssr = EngineSettings::GetInstance().SSR();
        auto& vol = EngineSettings::GetInstance().Volumetric();
        auto& taa = EngineSettings::GetInstance().TAA();
        auto& mb = EngineSettings::GetInstance().MotionBlur();

        if (Spark::Ui::SectionHeader("Bloom"))
        {
            const ScopedPropertyGrid grid("Bloom");
            if (Grid::Checkbox("Bloom Enabled", &pp.bloomEnabled))
                m_modified = true;
            if (pp.bloomEnabled)
            {
                if (Grid::DragFloat("Threshold", &pp.bloomThreshold, 0.05f, 0.0f, 10.0f))
                    m_modified = true;
                if (Grid::DragFloat("Intensity##Bloom", &pp.bloomIntensity, 0.05f, 0.0f, 10.0f))
                    m_modified = true;
                if (Grid::DragFloat("Radius", &pp.bloomRadius, 0.1f, 0.0f, 10.0f))
                    m_modified = true;
                if (Grid::SliderFloat("Soft Knee", &pp.bloomSoftKnee, 0.0f, 1.0f))
                    m_modified = true;
                if (Grid::SliderInt("Iterations", &pp.bloomIterations, 1, 16))
                    m_modified = true;
            }
        }

        if (Spark::Ui::SectionHeader("Tone Mapping", nullptr, false))
        {
            const ScopedPropertyGrid grid("Tone Mapping");
            const char* tmItems[] = {"None", "Reinhard", "ReinhardJodie", "Uncharted2", "ACES", "AgX", "FilmicALU"};
            if (Grid::Combo("Operator", &pp.toneMappingOperator, tmItems, 7))
                m_modified = true;
            if (Grid::DragFloat("Exposure", &pp.exposure, 0.05f, 0.01f, 20.0f))
                m_modified = true;
            if (Grid::DragFloat("Gamma", &pp.gamma, 0.05f, 0.5f, 5.0f))
                m_modified = true;
            if (Grid::DragFloat("White Point", &pp.whitePoint, 0.1f, 1.0f, 50.0f))
                m_modified = true;
        }

        if (Spark::Ui::SectionHeader("Color Grading", nullptr, false))
        {
            const ScopedPropertyGrid grid("Color Grading");
            if (Grid::Checkbox("Color Grading Enabled", &pp.colorGradingEnabled))
                m_modified = true;
            if (pp.colorGradingEnabled)
            {
                if (Grid::SliderFloat("Temperature", &pp.temperature, -1.0f, 1.0f))
                    m_modified = true;
                if (Grid::SliderFloat("Tint", &pp.tint, -1.0f, 1.0f))
                    m_modified = true;
                if (Grid::DragFloat("Contrast", &pp.contrast, 0.01f, 0.0f, 3.0f))
                    m_modified = true;
                if (Grid::DragFloat("Brightness", &pp.brightness, 0.01f, -1.0f, 1.0f))
                    m_modified = true;
                if (Grid::DragFloat("Saturation", &pp.saturation, 0.01f, 0.0f, 3.0f))
                    m_modified = true;
            }
        }

        if (Spark::Ui::SectionHeader("SSAO", nullptr, false))
        {
            const ScopedPropertyGrid grid("SSAO");
            if (Grid::Checkbox("SSAO Enabled", &ssao.enabled))
                m_modified = true;
            if (ssao.enabled)
            {
                if (Grid::DragFloat("Radius##SSAO", &ssao.radius, 0.05f, 0.01f, 5.0f))
                    m_modified = true;
                if (Grid::DragFloat("Intensity##SSAO", &ssao.intensity, 0.05f, 0.0f, 5.0f))
                    m_modified = true;
                if (Grid::SliderInt("Sample Count##SSAO", &ssao.sampleCount, 4, 64))
                    m_modified = true;
                if (Grid::DragFloat("Bias", &ssao.bias, 0.005f, 0.0f, 0.5f, "%.3f"))
                    m_modified = true;
                if (Grid::Checkbox("Blur##SSAO", &ssao.blur))
                    m_modified = true;
            }
        }

        if (Spark::Ui::SectionHeader("Screen-Space Reflections", nullptr, false))
        {
            const ScopedPropertyGrid grid("Screen-Space Reflections");
            if (Grid::Checkbox("SSR Enabled", &ssr.enabled))
                m_modified = true;
            if (ssr.enabled)
            {
                if (Grid::DragFloat("Max Distance##SSR", &ssr.maxDistance, 1.0f, 1.0f, 500.0f))
                    m_modified = true;
                if (Grid::SliderInt("Max Steps##SSR", &ssr.maxSteps, 8, 256))
                    m_modified = true;
                if (Grid::DragFloat("Thickness##SSR", &ssr.thickness, 0.05f, 0.01f, 5.0f))
                    m_modified = true;
                if (Grid::DragFloat("Fade Start", &ssr.fadeStart, 1.0f, 0.0f, ssr.fadeEnd))
                    m_modified = true;
                if (Grid::DragFloat("Fade End", &ssr.fadeEnd, 1.0f, ssr.fadeStart, 500.0f))
                    m_modified = true;
            }
        }

        if (Spark::Ui::SectionHeader("Volumetric Lighting", nullptr, false))
        {
            const ScopedPropertyGrid grid("Volumetric Lighting");
            if (Grid::Checkbox("Volumetric Enabled", &vol.enabled))
                m_modified = true;
            if (vol.enabled)
            {
                if (Grid::SliderInt("Sample Count##Vol", &vol.sampleCount, 8, 128))
                    m_modified = true;
                if (Grid::DragFloat("Scattering", &vol.scattering, 0.01f, 0.0f, 1.0f))
                    m_modified = true;
                if (Grid::DragFloat("Extinction", &vol.extinction, 0.001f, 0.0f, 0.1f, "%.4f"))
                    m_modified = true;
                if (Grid::SliderFloat("Anisotropy##Vol", &vol.anisotropy, -1.0f, 1.0f))
                    m_modified = true;
            }
        }

        if (Spark::Ui::SectionHeader("Temporal Anti-Aliasing", nullptr, false))
        {
            const ScopedPropertyGrid grid("Temporal Anti-Aliasing");
            if (Grid::Checkbox("TAA Enabled", &taa.enabled))
                m_modified = true;
            if (taa.enabled)
            {
                const char* qualItems[] = {"Low", "Medium", "High", "Ultra"};
                if (Grid::Combo("Quality##TAA", &taa.quality, qualItems, 4))
                    m_modified = true;
                const char* jitterItems[] = {"Halton 2,3", "Blue Noise", "Uniform 8x", "Interleaved Gradient"};
                if (Grid::Combo("Jitter Pattern", &taa.jitterPattern, jitterItems, 4))
                    m_modified = true;
                if (Grid::SliderFloat("History Blend", &taa.historyBlendFactor, 0.0f, 1.0f))
                    m_modified = true;
                if (Grid::SliderFloat("Sharpness##TAA", &taa.sharpness, 0.0f, 1.0f))
                    m_modified = true;
                if (Grid::SliderFloat("Ghosting Rejection", &taa.ghostingRejectionStrength, 0.0f, 1.0f))
                    m_modified = true;
                if (Grid::SliderFloat("Flicker Reduction", &taa.flickerReduction, 0.0f, 1.0f))
                    m_modified = true;
                if (Grid::Checkbox("Use Motion Vectors", &taa.useMotionVectors))
                    m_modified = true;
                if (Grid::Checkbox("Use YCoCg", &taa.useYCoCg))
                    m_modified = true;
            }
        }

        if (Spark::Ui::SectionHeader("Motion Blur", nullptr, false))
        {
            const ScopedPropertyGrid grid("Motion Blur");
            if (Grid::Checkbox("Motion Blur Enabled", &mb.enabled))
                m_modified = true;
            if (mb.enabled)
            {
                const char* mbTypes[] = {"Camera Only", "Per-Object", "Combined"};
                if (Grid::Combo("Type##MB", &mb.type, mbTypes, 3))
                    m_modified = true;
                if (Grid::SliderFloat("Intensity##MB", &mb.intensity, 0.0f, 2.0f))
                    m_modified = true;
                if (Grid::SliderInt("Sample Count##MB", &mb.sampleCount, 2, 32))
                    m_modified = true;
                if (Grid::DragFloat("Max Blur Radius", &mb.maxBlurRadius, 1.0f, 1.0f, 128.0f))
                    m_modified = true;
                if (Grid::DragFloat("Velocity Scale", &mb.velocityScale, 0.1f, 0.0f, 10.0f))
                    m_modified = true;
            }
        }
    }

    // =========================================================================
    // Audio Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderAudioTab()
    {
        auto& audio = EngineSettings::GetInstance().Audio();
        auto& ext = EngineSettings::GetInstance().AudioExtended();

        if (Spark::Ui::SectionHeader("Volume"))
        {
            const ScopedPropertyGrid grid("Volume");
            if (Grid::SliderFloat("Master Volume", &audio.masterVolume, 0.0f, 1.0f))
                m_modified = true;
            if (Grid::SliderFloat("SFX Volume", &audio.sfxVolume, 0.0f, 1.0f))
                m_modified = true;
            if (Grid::SliderFloat("Music Volume", &audio.musicVolume, 0.0f, 1.0f))
                m_modified = true;
            if (Grid::SliderFloat("Voice Volume", &audio.voiceVolume, 0.0f, 1.0f))
                m_modified = true;
            if (Grid::Checkbox("Mute on Focus Loss", &audio.muteOnFocusLoss))
                m_modified = true;
        }

        if (Spark::Ui::SectionHeader("3D Audio & Effects", nullptr, false))
        {
            const ScopedPropertyGrid grid("3D Audio & Effects");
            if (Grid::Checkbox("Enable 3D Audio", &ext.enable3D))
                m_modified = true;
            if (Grid::Checkbox("Enable Reverb", &ext.enableReverb))
                m_modified = true;
            if (Grid::Checkbox("Enable EAX", &ext.enableEAX))
                m_modified = true;
            if (Grid::DragFloat("Doppler Scale", &ext.dopplerScale, 0.1f, 0.0f, 10.0f))
                m_modified = true;
            if (Grid::DragFloat("Distance Scale", &ext.distanceScale, 0.1f, 0.1f, 100.0f))
                m_modified = true;
            if (Grid::DragInt("Max Sources", &ext.maxSources, 1, 8, 256))
                m_modified = true;
        }
    }

    // =========================================================================
    // Controls Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderControlsTab()
    {
        auto& ctrl = EngineSettings::GetInstance().Controls();
        auto& game = EngineSettings::GetInstance().Game();

        if (Spark::Ui::SectionHeader("Mouse"))
        {
            const ScopedPropertyGrid grid("Mouse");
            if (Grid::DragFloat("Sensitivity", &ctrl.mouseSensitivity, 0.05f, 0.05f, 10.0f))
                m_modified = true;
            if (Grid::Checkbox("Invert Mouse Y", &ctrl.invertMouseY))
                m_modified = true;
            if (Grid::DragFloat("Dead Zone", &ctrl.mouseDeadZone, 0.01f, 0.0f, 0.5f))
                m_modified = true;
            if (Grid::Checkbox("Raw Mouse Input", &ctrl.rawMouseInput))
                m_modified = true;
            if (Grid::Checkbox("Mouse Acceleration", &ctrl.mouseAcceleration))
                m_modified = true;
        }

        if (Spark::Ui::SectionHeader("Controller", nullptr, false))
        {
            const ScopedPropertyGrid grid("Controller");
            if (Grid::DragFloat("Left Stick Dead Zone", &ctrl.controllerDeadZoneLeft, 0.01f, 0.0f, 0.5f))
                m_modified = true;
            if (Grid::DragFloat("Right Stick Dead Zone", &ctrl.controllerDeadZoneRight, 0.01f, 0.0f, 0.5f))
                m_modified = true;
            if (Grid::DragFloat("Controller Sensitivity", &ctrl.controllerSensitivity, 0.1f, 0.1f, 10.0f))
                m_modified = true;
            if (Grid::Checkbox("Controller Vibration", &ctrl.controllerVibration))
                m_modified = true;
            if (Grid::Checkbox("Invert Controller Y", &ctrl.invertControllerY))
                m_modified = true;
        }

        if (Spark::Ui::SectionHeader("Game Display", nullptr, false))
        {
            const ScopedPropertyGrid grid("Game Display");
            const char* diffItems[] = {"Easy", "Normal", "Hard", "Nightmare"};
            int diffIdx = 1;
            if (game.difficulty == "Easy")
                diffIdx = 0;
            else if (game.difficulty == "Hard")
                diffIdx = 2;
            else if (game.difficulty == "Nightmare")
                diffIdx = 3;
            if (Grid::Combo("Difficulty", &diffIdx, diffItems, 4))
            {
                const char* diffNames[] = {"Easy", "Normal", "Hard", "Nightmare"};
                game.difficulty = diffNames[diffIdx];
                m_modified = true;
            }
            if (Grid::DragFloat("Field of View", &game.fieldOfView, 1.0f, 30.0f, 150.0f, "%.0f"))
                m_modified = true;
        }
    }

    // =========================================================================
    // Physics Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderPhysicsTab()
    {
        auto& phys = EngineSettings::GetInstance().Physics();

        Spark::Ui::SubHeader("Gravity");
        {
            const ScopedPropertyGrid grid("Gravity");
            if (Grid::DragFloat("Gravity X", &phys.gravityX, 0.1f, -100.0f, 100.0f))
                m_modified = true;
            if (Grid::DragFloat("Gravity Y", &phys.gravityY, 0.1f, -100.0f, 100.0f))
                m_modified = true;
            if (Grid::DragFloat("Gravity Z", &phys.gravityZ, 0.1f, -100.0f, 100.0f))
                m_modified = true;
        }

        Spark::Ui::SubHeader("Simulation");
        const ScopedPropertyGrid grid("Simulation");
        if (Grid::DragFloat("Fixed Timestep", &phys.fixedTimestep, 0.001f, 0.001f, 0.1f, "%.4f"))
            m_modified = true;
        if (Grid::DragInt("Max Sub-steps", &phys.maxSubSteps, 1, 1, 16))
            m_modified = true;
        if (Grid::DragFloat("Default Friction", &phys.defaultFriction, 0.01f, 0.0f, 1.0f))
            m_modified = true;
        if (Grid::DragFloat("Default Restitution", &phys.defaultRestitution, 0.01f, 0.0f, 1.0f))
            m_modified = true;
        if (Grid::Checkbox("Debug Draw", &phys.debugDraw))
            m_modified = true;
    }

    // =========================================================================
    // AI Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderAITab()
    {
        auto& ai = EngineSettings::GetInstance().AI();
        const ScopedPropertyGrid grid("AI");

        if (Grid::DragFloat("Detection Range", &ai.detectionRange, 0.5f, 1.0f, 200.0f))
            m_modified = true;
        if (Grid::DragFloat("Attack Range", &ai.attackRange, 0.5f, 1.0f, 100.0f))
            m_modified = true;
        if (Grid::DragFloat("Melee Range", &ai.meleeRange, 0.1f, 0.5f, 10.0f))
            m_modified = true;
        if (Grid::DragFloat("Move Speed", &ai.moveSpeed, 0.1f, 0.1f, 50.0f))
            m_modified = true;
        if (Grid::DragFloat("Turn Speed", &ai.turnSpeed, 1.0f, 10.0f, 720.0f, "%.0f deg/s"))
            m_modified = true;
        if (Grid::SliderFloat("Accuracy", &ai.accuracy, 0.0f, 1.0f))
            m_modified = true;
        if (Grid::DragFloat("Reaction Time", &ai.reactionTime, 0.01f, 0.0f, 5.0f, "%.2f s"))
            m_modified = true;
        if (Grid::DragFloat("Cover Search Radius", &ai.coverSearchRadius, 0.5f, 1.0f, 100.0f))
            m_modified = true;
        if (Grid::Checkbox("Can Strafe", &ai.canStrafe))
            m_modified = true;
        if (Grid::Checkbox("Can Sprint", &ai.canSprint))
            m_modified = true;
        if (Grid::Checkbox("Can Use Cover", &ai.canUseCover))
            m_modified = true;
    }

    // =========================================================================
    // Player Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderPlayerTab()
    {
        auto& p = EngineSettings::GetInstance().Player();

        if (Spark::Ui::SectionHeader("Health & Defense"))
        {
            const ScopedPropertyGrid grid("Health & Defense");
            if (Grid::DragFloat("Max Health", &p.maxHealth, 1.0f, 1.0f, 1000.0f))
                m_modified = true;
            if (Grid::DragFloat("Max Armor", &p.maxArmor, 1.0f, 0.0f, 1000.0f))
                m_modified = true;
            if (Grid::DragFloat("Max Shield", &p.maxShield, 1.0f, 0.0f, 500.0f))
                m_modified = true;
            if (Grid::DragFloat("Shield Recharge Rate", &p.shieldRechargeRate, 0.5f, 0.0f, 100.0f))
                m_modified = true;
            if (Grid::DragFloat("Shield Recharge Delay", &p.shieldRechargeDelay, 0.1f, 0.0f, 30.0f, "%.1f s"))
                m_modified = true;
        }

        if (Spark::Ui::SectionHeader("Movement"))
        {
            const ScopedPropertyGrid grid("Movement");
            if (Grid::DragFloat("Move Speed##Player", &p.moveSpeed, 0.1f, 0.1f, 50.0f))
                m_modified = true;
            if (Grid::DragFloat("Jump Height", &p.jumpHeight, 0.1f, 0.0f, 20.0f))
                m_modified = true;
            if (Grid::DragFloat("Gravity Force", &p.gravityForce, 0.5f, 1.0f, 100.0f))
                m_modified = true;
            if (Grid::DragFloat("Friction##Player", &p.friction, 0.01f, 0.0f, 1.0f))
                m_modified = true;
            if (Grid::DragFloat("Sprint Multiplier", &p.sprintMultiplier, 0.1f, 1.0f, 5.0f))
                m_modified = true;
            if (Grid::DragFloat("Crouch Multiplier", &p.crouchMultiplier, 0.05f, 0.1f, 1.0f))
                m_modified = true;
            if (Grid::DragFloat("ADS Speed Multiplier", &p.adsSpeedMultiplier, 0.05f, 0.1f, 1.0f))
                m_modified = true;
        }

        if (Spark::Ui::SectionHeader("Energy", nullptr, false))
        {
            const ScopedPropertyGrid grid("Energy");
            if (Grid::DragFloat("Max Energy", &p.maxEnergy, 1.0f, 0.0f, 500.0f))
                m_modified = true;
            if (Grid::DragFloat("Energy Regen Rate", &p.energyRegenRate, 0.5f, 0.0f, 100.0f))
                m_modified = true;
        }
    }

    // =========================================================================
    // Gameplay Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderGameplayTab()
    {
        auto& gm = EngineSettings::GetInstance().GameMode();

        if (Spark::Ui::SectionHeader("Scoring"))
        {
            const ScopedPropertyGrid grid("Scoring");
            if (Grid::DragInt("Score Limit", &gm.scoreLimit, 1, 0, 1000))
                m_modified = true;
            if (Grid::DragInt("Kill Points", &gm.killPoints, 1, 0, 500))
                m_modified = true;
            if (Grid::DragInt("Assist Points", &gm.assistPoints, 1, 0, 500))
                m_modified = true;
            if (Grid::DragInt("Objective Points", &gm.objectivePoints, 1, 0, 1000))
                m_modified = true;
            if (Grid::DragInt("Headshot Bonus", &gm.headshotBonus, 1, 0, 200))
                m_modified = true;
            if (Grid::DragInt("Death Penalty", &gm.deathPenalty, 1, 0, 200))
                m_modified = true;
        }

        if (Spark::Ui::SectionHeader("Rules"))
        {
            const ScopedPropertyGrid grid("Rules");
            if (Grid::DragInt("Round Limit", &gm.roundLimit, 1, 0, 100))
                m_modified = true;
            if (Grid::DragFloat("Time Limit", &gm.timeLimit, 1.0f, 0.0f, 3600.0f, "%.0f s"))
                m_modified = true;
            if (Grid::DragFloat("Respawn Delay", &gm.respawnDelay, 0.1f, 0.0f, 30.0f, "%.1f s"))
                m_modified = true;
            if (Grid::Checkbox("Auto Respawn", &gm.autoRespawn))
                m_modified = true;
            if (Grid::Checkbox("Friendly Fire", &gm.friendlyFire))
                m_modified = true;
            if (Grid::Checkbox("Headshots", &gm.headshots))
                m_modified = true;
        }

        if (Spark::Ui::SectionHeader("Multipliers", nullptr, false))
        {
            const ScopedPropertyGrid grid("Multipliers");
            if (Grid::DragFloat("Damage Multiplier", &gm.damageMultiplier, 0.1f, 0.1f, 10.0f))
                m_modified = true;
            if (Grid::DragFloat("Health Multiplier", &gm.healthMultiplier, 0.1f, 0.1f, 10.0f))
                m_modified = true;
            if (Grid::DragFloat("Speed Multiplier", &gm.speedMultiplier, 0.1f, 0.1f, 10.0f))
                m_modified = true;
            if (Grid::DragFloat("Headshot Multiplier", &gm.headshotMultiplier, 0.1f, 1.0f, 10.0f))
                m_modified = true;
        }
    }

    // =========================================================================
    // Camera Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderCameraTab()
    {
        auto& cam = EngineSettings::GetInstance().Camera();
        const ScopedPropertyGrid grid("Camera");

        if (Grid::DragFloat("Move Speed", &cam.moveSpeed, 0.1f, 0.1f, 100.0f))
            m_modified = true;
        if (Grid::DragFloat("Rotation Speed", &cam.rotationSpeed, 0.1f, 0.1f, 20.0f))
            m_modified = true;
        if (Grid::DragFloat("Default FOV", &cam.defaultFov, 1.0f, 30.0f, 150.0f, "%.0f"))
            m_modified = true;
        if (Grid::DragFloat("Zoomed FOV", &cam.zoomedFov, 1.0f, 10.0f, 90.0f, "%.0f"))
            m_modified = true;
        if (Grid::Checkbox("Smooth Movement", &cam.smoothMovement))
            m_modified = true;
        if (Grid::DragFloat("Near Plane", &cam.nearPlane, 0.01f, 0.001f, 10.0f, "%.3f"))
            m_modified = true;
        if (Grid::DragFloat("Far Plane", &cam.farPlane, 10.0f, 10.0f, 100000.0f, "%.0f"))
            m_modified = true;
    }

    // =========================================================================
    // Editor Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderEditorTab()
    {
        auto& ed = EngineSettings::GetInstance().Editor();

        if (Spark::Ui::SectionHeader("Grid & Snapping"))
        {
            const ScopedPropertyGrid grid("Grid & Snapping");
            if (Grid::DragFloat("Grid Size", &ed.gridSize, 0.1f, 0.1f, 100.0f))
                m_modified = true;
            if (Grid::Checkbox("Snap to Grid", &ed.snapToGrid))
                m_modified = true;
            if (Grid::Checkbox("Show Grid", &ed.showGrid))
                m_modified = true;
            if (Grid::DragFloat("Rotation Snap", &ed.rotationSnap, 1.0f, 0.0f, 90.0f, "%.0f deg"))
                m_modified = true;
            if (Grid::DragFloat("Scale Snap", &ed.scaleSnap, 0.05f, 0.0f, 1.0f))
                m_modified = true;
        }

        if (Spark::Ui::SectionHeader("Gizmo & Visualization"))
        {
            const ScopedPropertyGrid grid("Gizmo & Visualization");
            if (Grid::DragFloat("Gizmo Scale", &ed.gizmoScale, 0.1f, 0.1f, 10.0f))
                m_modified = true;
            if (Grid::Checkbox("Show Gizmo Labels", &ed.showGizmoLabels))
                m_modified = true;
            if (Grid::Checkbox("Wireframe Overlay", &ed.showWireframeOverlay))
                m_modified = true;
            if (Grid::Checkbox("Show Bounds", &ed.showBounds))
                m_modified = true;
            if (Grid::Checkbox("Show Colliders", &ed.showColliders))
                m_modified = true;
            if (Grid::Checkbox("Show NavMesh", &ed.showNavMesh))
                m_modified = true;
            if (Grid::Checkbox("Show Light Radius", &ed.showLightRadius))
                m_modified = true;
        }

        if (Spark::Ui::SectionHeader("Autosave & History", nullptr, false))
        {
            const ScopedPropertyGrid grid("Autosave & History");
            if (Grid::Checkbox("Autosave Enabled", &ed.autosaveEnabled))
                m_modified = true;
            if (Grid::DragFloat("Autosave Interval", &ed.autosaveIntervalSeconds, 10.0f, 30.0f, 3600.0f, "%.0f s"))
                m_modified = true;
            if (Grid::DragInt("Undo History Size", &ed.undoHistorySize, 1, 10, 1000))
                m_modified = true;
            if (Grid::DragInt("Recent Files Max", &ed.recentFilesMax, 1, 1, 50))
                m_modified = true;
        }

        if (Spark::Ui::SectionHeader("Navigation", nullptr, false))
        {
            const ScopedPropertyGrid grid("Navigation");
            if (Grid::DragFloat("Camera Speed Multiplier", &ed.cameraSpeedMultiplier, 0.1f, 0.1f, 10.0f))
                m_modified = true;
        }

        if (Spark::Ui::SectionHeader("Collaboration", nullptr, false))
        {
            const ScopedPropertyGrid grid("Collaboration");
            if (Grid::Checkbox("Enable Collaboration", &ed.enableCollaboration))
                m_modified = true;
            Spark::Ui::StatusText(Spark::Ui::Status::Warning, "Requires restart");
        }
    }

    // =========================================================================
    // Network Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderNetworkTab()
    {
        auto& net = EngineSettings::GetInstance().Network();

        if (Spark::Ui::SectionHeader("Server"))
        {
            const ScopedPropertyGrid grid("Server");
            if (Grid::DragInt("Server Port", &net.serverPort, 1, 1024, 65535))
            {
                SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Network: server port changed to %d", net.serverPort);
                m_modified = true;
            }
            if (Grid::DragInt("Max Clients", &net.maxClients, 1, 1, 128))
                m_modified = true;
            if (Grid::DragFloat("Connection Timeout", &net.connectionTimeout, 0.5f, 1.0f, 60.0f, "%.1f s"))
                m_modified = true;
            if (Grid::DragFloat("Heartbeat Interval", &net.heartbeatInterval, 0.1f, 0.1f, 10.0f, "%.1f s"))
                m_modified = true;
        }

        if (Spark::Ui::SectionHeader("Replication", nullptr, false))
        {
            const ScopedPropertyGrid grid("Replication");
            if (Grid::DragFloat("Replication Rate", &net.replicationRate, 1.0f, 1.0f, 120.0f, "%.0f Hz"))
                m_modified = true;
            if (Grid::DragFloat("Reliable Retransmit Base", &net.reliableRetransmitBase, 0.05f, 0.1f, 5.0f, "%.2f s"))
                m_modified = true;
            if (Grid::DragInt("Max Reliable Retries", &net.maxReliableRetries, 1, 1, 20))
                m_modified = true;
        }

        if (Spark::Ui::SectionHeader("Buffers & Features", nullptr, false))
        {
            const ScopedPropertyGrid grid("Buffers & Features");
            if (Grid::DragInt("Send Buffer Size", &net.sendBufferSize, 1024, 4096, 1048576))
                m_modified = true;
            if (Grid::DragInt("Receive Buffer Size", &net.receiveBufferSize, 1024, 4096, 1048576))
                m_modified = true;
            if (Grid::Checkbox("Enable Compression", &net.enableCompression))
                m_modified = true;
        }

        if (Spark::Ui::SectionHeader("Lag Simulation (Dev Only)", nullptr, false))
        {
            const ScopedPropertyGrid grid("Lag Simulation (Dev Only)");
            Spark::Ui::StatusText(Spark::Ui::Status::Warning, "For development/testing only");
            if (Grid::DragFloat("Simulated Latency", &net.simulatedLatencyMs, 1.0f, 0.0f, 500.0f, "%.0f ms"))
                m_modified = true;
            if (Grid::SliderFloat("Simulated Packet Loss", &net.simulatedPacketLoss, 0.0f, 1.0f, "%.2f"))
                m_modified = true;
            if (Grid::DragFloat("Simulated Jitter", &net.simulatedJitterMs, 0.5f, 0.0f, 100.0f, "%.0f ms"))
                m_modified = true;
        }
    }

    // =========================================================================
    // Scripting Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderScriptingTab()
    {
        auto& s = EngineSettings::GetInstance().Scripting();

        if (Spark::Ui::SectionHeader("Hot Reload"))
        {
            const ScopedPropertyGrid grid("Hot Reload");
            if (Grid::Checkbox("Hot Reload Enabled", &s.hotReloadEnabled))
                m_modified = true;
            if (s.hotReloadEnabled)
            {
                if (Grid::DragFloat("Poll Interval", &s.hotReloadPollInterval, 0.1f, 0.1f, 10.0f, "%.1f s"))
                    m_modified = true;
            }
        }

        if (Spark::Ui::SectionHeader("Execution"))
        {
            const ScopedPropertyGrid grid("Execution");
            if (Grid::DragInt("Context Pool Size", &s.contextPoolSize, 1, 1, 64))
                m_modified = true;
            if (Grid::DragFloat("Execution Timeout", &s.executionTimeoutMs, 10.0f, 10.0f, 10000.0f, "%.0f ms"))
                m_modified = true;
            if (Grid::DragInt("Max Call Stack Depth", &s.maxCallStackDepth, 1, 8, 256))
                m_modified = true;
            if (Grid::DragInt("Max Script Memory", &s.maxScriptMemoryMB, 1, 8, 512, "%d MB"))
                m_modified = true;
        }

        if (Spark::Ui::SectionHeader("Debug", nullptr, false))
        {
            const ScopedPropertyGrid grid("Debug");
            if (Grid::Checkbox("Generate Debug Info", &s.generateDebugInfo))
                m_modified = true;
            if (Grid::Checkbox("Enable Profiler", &s.enableProfiler))
                m_modified = true;
        }
    }

    // =========================================================================
    // Animation Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderAnimationTab()
    {
        auto& a = EngineSettings::GetInstance().Animation();

        if (Spark::Ui::SectionHeader("Blending & Playback"))
        {
            const ScopedPropertyGrid grid("Blending & Playback");
            if (Grid::DragFloat("Default Blend Time", &a.defaultBlendTime, 0.01f, 0.0f, 2.0f, "%.2f s"))
                m_modified = true;
            if (Grid::DragInt("Max Active Montages", &a.maxActiveMontages, 1, 1, 16))
                m_modified = true;
            if (Grid::Checkbox("Enable Root Motion", &a.enableRootMotion))
                m_modified = true;
            if (Grid::Checkbox("Enable Animation Events", &a.enableAnimationEvents))
                m_modified = true;
        }

        if (Spark::Ui::SectionHeader("Inverse Kinematics", nullptr, false))
        {
            const ScopedPropertyGrid grid("Inverse Kinematics");
            if (Grid::DragInt("IK Solver Iterations", &a.ikSolverIterations, 1, 1, 50))
                m_modified = true;
            if (Grid::DragFloat("IK Tolerance", &a.ikTolerance, 0.0001f, 0.0001f, 0.1f, "%.4f"))
                m_modified = true;
        }

        if (Spark::Ui::SectionHeader("Quality & LOD", nullptr, false))
        {
            const ScopedPropertyGrid grid("Quality & LOD");
            const char* compItems[] = {"None", "Low", "Medium", "High"};
            if (Grid::Combo("Compression Quality", &a.compressionQuality, compItems, 4))
                m_modified = true;
            if (Grid::DragFloat("LOD Distance Multiplier", &a.lodDistanceMultiplier, 0.1f, 0.1f, 5.0f))
                m_modified = true;
        }
    }

    // =========================================================================
    // Logging Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderLoggingTab()
    {
        auto& log = EngineSettings::GetInstance().Logging();

        const char* levelItems[] = {"Trace", "Debug", "Info", "Warn", "Error", "Fatal", "Off"};
        auto levelCombo = [&](const char* label, std::string& level, bool allowEmpty = false)
        {
            int idx = 2; // Info default
            if (level == "Trace")
                idx = 0;
            else if (level == "Debug")
                idx = 1;
            else if (level == "Info")
                idx = 2;
            else if (level == "Warn")
                idx = 3;
            else if (level == "Error")
                idx = 4;
            else if (level == "Fatal")
                idx = 5;
            else if (level == "Off")
                idx = 6;
            else if (allowEmpty && level.empty())
                idx = -1;

            if (allowEmpty)
            {
                const char* extItems[] = {"(Use Global)", "Trace", "Debug", "Info", "Warn", "Error", "Fatal", "Off"};
                int extIdx = idx + 1;
                if (idx == -1)
                    extIdx = 0;
                if (Grid::Combo(label, &extIdx, extItems, 8))
                {
                    if (extIdx == 0)
                        level = "";
                    else
                        level = extItems[extIdx];
                    m_modified = true;
                }
            }
            else
            {
                if (Grid::Combo(label, &idx, levelItems, 7))
                {
                    level = levelItems[idx];
                    m_modified = true;
                }
            }
        };

        if (Spark::Ui::SectionHeader("Global"))
        {
            const ScopedPropertyGrid grid("Global");
            levelCombo("Global Level", log.globalLevel);
            levelCombo("Stack Trace Level", log.stackTraceLevel);
        }

        if (Spark::Ui::SectionHeader("Per-Category Overrides", nullptr, false))
        {
            ImGui::PushTextWrapPos(0.0f);
            Spark::Ui::SecondaryText("Set per-category levels, or leave as '(Use Global)' to inherit.");
            ImGui::PopTextWrapPos();
            const ScopedPropertyGrid grid("Per-Category Overrides");
            levelCombo("Core", log.coreLevel, true);
            levelCombo("Graphics##Log", log.graphicsLevel, true);
            levelCombo("Physics##Log", log.physicsLevel, true);
            levelCombo("Audio##Log", log.audioLevel, true);
            levelCombo("AI##Log", log.aiLevel, true);
            levelCombo("Animation##Log", log.animationLevel, true);
            levelCombo("ECS", log.ecsLevel, true);
            levelCombo("Network##Log", log.networkLevel, true);
            levelCombo("Input", log.inputLevel, true);
            levelCombo("Scripting##Log", log.scriptingLevel, true);
            levelCombo("Scene", log.sceneLevel, true);
            levelCombo("Save", log.saveLevel, true);
            levelCombo("Cinematic", log.cinematicLevel, true);
            levelCombo("Procedural", log.proceduralLevel, true);
            levelCombo("Editor##Log", log.editorLevel, true);
            levelCombo("Game##Log", log.gameLevel, true);
        }
    }

    // =========================================================================
    // Accessibility Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderAccessibilityTab()
    {
        auto& a = EngineSettings::GetInstance().Accessibility();

        if (Spark::Ui::SectionHeader("Vision"))
        {
            const ScopedPropertyGrid grid("Vision");
            const char* cbItems[] = {"Off", "Protanopia", "Deuteranopia", "Tritanopia"};
            if (Grid::Combo("Colorblind Mode", &a.colorblindMode, cbItems, 4))
                m_modified = true;
            if (a.colorblindMode > 0)
            {
                if (Grid::SliderFloat("Colorblind Strength", &a.colorblindStrength, 0.0f, 1.0f))
                    m_modified = true;
            }
            if (Grid::Checkbox("High Contrast", &a.highContrast))
                m_modified = true;
            if (Grid::Checkbox("Large Text", &a.largeText))
                m_modified = true;
        }

        if (Spark::Ui::SectionHeader("Audio Accessibility", nullptr, false))
        {
            const ScopedPropertyGrid grid("Audio Accessibility");
            if (Grid::Checkbox("Closed Captions", &a.closedCaptions))
                m_modified = true;
            if (Grid::Checkbox("Screen Reader", &a.screenReader))
                m_modified = true;
            if (a.screenReader)
            {
                if (Grid::DragFloat("TTS Rate", &a.textToSpeechRate, 0.1f, 0.5f, 3.0f))
                    m_modified = true;
            }
        }

        if (Spark::Ui::SectionHeader("Motor Accessibility", nullptr, false))
        {
            const ScopedPropertyGrid grid("Motor Accessibility");
            if (Grid::Checkbox("Reduce Motion", &a.reduceMotion))
                m_modified = true;
            if (Grid::DragFloat("Hold Time Multiplier", &a.holdTimeMultiplier, 0.1f, 0.5f, 5.0f))
                m_modified = true;
            if (Grid::Checkbox("Toggle Aim", &a.toggleAim))
                m_modified = true;
            if (Grid::Checkbox("Toggle Sprint", &a.toggleSprint))
                m_modified = true;
            if (Grid::Checkbox("Toggle Crouch", &a.toggleCrouch))
                m_modified = true;
            if (Grid::Checkbox("Auto-Aim", &a.autoAim))
                m_modified = true;
            if (a.autoAim)
            {
                if (Grid::SliderFloat("Auto-Aim Strength", &a.autoAimStrength, 0.0f, 1.0f))
                    m_modified = true;
            }
        }
    }

    // =========================================================================
    // VR Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderVRTab()
    {
        auto& vr = EngineSettings::GetInstance().VR();

        {
            const ScopedPropertyGrid grid("VR");
            if (Grid::Checkbox("Enable VR", &vr.enabled))
                m_modified = true;
        }
        if (vr.enabled)
        {
            Spark::Ui::StatusText(Spark::Ui::Status::Warning, "VR mode changes require restart");

            if (Spark::Ui::SectionHeader("Display"))
            {
                const ScopedPropertyGrid grid("Display");
                if (Grid::DragInt("Render Target Width", &vr.renderTargetWidth, 1, 640, 4096))
                    m_modified = true;
                if (Grid::DragInt("Render Target Height", &vr.renderTargetHeight, 1, 640, 4096))
                    m_modified = true;
                if (Grid::SliderFloat("Render Scale##VR", &vr.renderScale, 0.5f, 2.0f))
                    m_modified = true;
                if (Grid::DragFloat("IPD", &vr.ipd, 0.001f, 0.050f, 0.080f, "%.3f m"))
                    m_modified = true;
                if (Grid::Checkbox("Reprojection", &vr.reprojection))
                    m_modified = true;
            }

            if (Spark::Ui::SectionHeader("Tracking", nullptr, false))
            {
                const ScopedPropertyGrid grid("Tracking");
                const char* trackItems[] = {"Seated", "Room Scale"};
                if (Grid::Combo("Tracking Space", &vr.trackingSpace, trackItems, 2))
                    m_modified = true;
                if (Grid::Checkbox("Head Tracking", &vr.headTrackingEnabled))
                    m_modified = true;
                if (Grid::Checkbox("Controller Tracking", &vr.controllerTrackingEnabled))
                    m_modified = true;
            }

            if (Spark::Ui::SectionHeader("Comfort", nullptr, false))
            {
                const ScopedPropertyGrid grid("Comfort");
                const char* comfortItems[] = {"Off", "Vignette", "Snap Turn"};
                if (Grid::Combo("Comfort Mode", &vr.comfortMode, comfortItems, 3))
                    m_modified = true;
                if (vr.comfortMode == 2)
                {
                    if (Grid::DragFloat("Snap Turn Angle", &vr.snapTurnAngle, 5.0f, 15.0f, 90.0f, "%.0f deg"))
                        m_modified = true;
                }
                if (Grid::SliderFloat("Haptic Amplitude", &vr.hapticAmplitude, 0.0f, 1.0f))
                    m_modified = true;
                if (Grid::DragFloat("Haptic Duration", &vr.hapticDuration, 0.01f, 0.01f, 1.0f, "%.2f s"))
                    m_modified = true;
            }
        }
    }

    // =========================================================================
    // Destruction & Dialogue Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderDestructionDialogueTab()
    {
        auto& d = EngineSettings::GetInstance().Destruction();
        auto& dl = EngineSettings::GetInstance().Dialogue();

        if (Spark::Ui::SectionHeader("Destruction"))
        {
            const ScopedPropertyGrid grid("Destruction");
            if (Grid::DragFloat("Debris Lifetime", &d.debrisLifetime, 0.5f, 0.0f, 60.0f, "%.1f s"))
                m_modified = true;
            if (Grid::DragFloat("Damage Threshold", &d.damageThreshold, 1.0f, 0.0f, 500.0f))
                m_modified = true;
            if (Grid::DragFloat("Damage Multiplier##Dest", &d.damageMultiplier, 0.1f, 0.0f, 10.0f))
                m_modified = true;
            if (Grid::DragInt("Max Damage Stages", &d.maxDamageStages, 1, 1, 10))
                m_modified = true;
            if (Grid::DragFloat("Scatter Force", &d.scatterForce, 0.5f, 0.0f, 50.0f))
                m_modified = true;
            if (Grid::DragInt("Max Debris Pieces", &d.maxDebrisPieces, 1, 1, 500))
                m_modified = true;
            if (Grid::Checkbox("Physics Debris", &d.enablePhysicsDebris))
                m_modified = true;
        }

        if (Spark::Ui::SectionHeader("Dialogue & DRS", nullptr, false))
        {
            const ScopedPropertyGrid grid("Dialogue & DRS");
            if (Grid::DragFloat("Default Cooldown", &dl.defaultCooldown, 0.5f, 0.0f, 60.0f, "%.1f s"))
                m_modified = true;
            if (Grid::DragInt("Default Priority", &dl.defaultPriority, 1, 0, 100))
                m_modified = true;
            if (Grid::DragInt("Max Rules Per Signal", &dl.maxRulesPerSignal, 1, 1, 64))
                m_modified = true;
            if (Grid::DragFloat("Typing Speed", &dl.typingSpeed, 1.0f, 5.0f, 200.0f, "%.0f chars/s"))
                m_modified = true;
            if (Grid::Checkbox("Enable Barks", &dl.enableBarks))
                m_modified = true;
            if (dl.enableBarks)
            {
                if (Grid::DragFloat("Bark Range", &dl.barkRange, 0.5f, 1.0f, 100.0f))
                    m_modified = true;
                if (Grid::DragFloat("Bark Cooldown", &dl.barkCooldown, 0.5f, 0.0f, 60.0f, "%.1f s"))
                    m_modified = true;
            }
        }
    }

    // =========================================================================
    // Modding & Localization Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderModdingLocalizationTab()
    {
        auto& mod = EngineSettings::GetInstance().Modding();
        auto& loc = EngineSettings::GetInstance().Localization();

        if (Spark::Ui::SectionHeader("Modding"))
        {
            const ScopedPropertyGrid grid("Modding");
            if (Grid::Checkbox("Enable Modding", &mod.enableModding))
                m_modified = true;
            Spark::Ui::StatusText(Spark::Ui::Status::Warning, "Requires restart");
            if (mod.enableModding)
            {
                if (Grid::Checkbox("Allow Script Mods", &mod.allowScriptMods))
                    m_modified = true;
                if (Grid::Checkbox("Allow Asset Overrides", &mod.allowAssetOverrides))
                    m_modified = true;
                if (Grid::Checkbox("Sandbox Mods", &mod.sandboxMods))
                    m_modified = true;
                // Neither script setting is enforced yet; say so instead of implying a sandbox.
                Spark::Ui::PropertyLabel("Script Mods");
                Spark::Ui::UnavailableFeature("Script mod loading",
                                              "Script mods are always refused: no sandboxed mod-script loader "
                                              "exists yet.");
                if (Grid::DragInt("Max Loaded Mods", &mod.maxLoadedMods, 1, 1, 128))
                    m_modified = true;
            }
        }

        if (Spark::Ui::SectionHeader("Localization", nullptr, false))
        {
            const ScopedPropertyGrid grid("Localization");
            if (Grid::Checkbox("Auto-Detect Language", &loc.autoDetectLanguage))
                m_modified = true;
            if (Grid::Checkbox("Load All Languages", &loc.loadAllLanguages))
                m_modified = true;
        }
    }

    // =========================================================================
    // Save & Replay Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderSaveReplayTab()
    {
        auto& sv = EngineSettings::GetInstance().SaveSystem();
        auto& rp = EngineSettings::GetInstance().Replay();

        if (Spark::Ui::SectionHeader("Save System"))
        {
            const ScopedPropertyGrid grid("Save System");
            if (Grid::DragInt("Max Auto-Save Slots", &sv.maxAutoSaveSlots, 1, 1, 10))
                m_modified = true;
            if (Grid::DragFloat("Auto-Save Interval", &sv.autoSaveInterval, 10.0f, 0.0f, 3600.0f, "%.0f s"))
                m_modified = true;
            if (Grid::DragInt("Max Manual Saves", &sv.maxManualSaves, 1, 1, 1000))
                m_modified = true;
            if (Grid::Checkbox("Compress Saves", &sv.compressSaves))
                m_modified = true;
            if (Grid::Checkbox("Backup on Save", &sv.backupOnSave))
                m_modified = true;
            if (Grid::Checkbox("Enable Cloud Saves", &sv.enableCloudSaves))
                m_modified = true;
        }

        if (Spark::Ui::SectionHeader("Replay", nullptr, false))
        {
            const ScopedPropertyGrid grid("Replay");
            if (Grid::Checkbox("Auto-Record", &rp.autoRecord))
                m_modified = true;
            if (Grid::DragFloat("Record Interval", &rp.recordInterval, 0.01f, 0.01f, 1.0f, "%.3f s"))
                m_modified = true;
            if (Grid::DragInt("Max Frame Count", &rp.maxFrameCount, 1000, 1000, 10000000))
                m_modified = true;
            if (Grid::DragInt("Max Entity Count", &rp.maxEntityCount, 1000, 100, 1000000))
                m_modified = true;
        }
    }

    // =========================================================================
    // Particles & Decals Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderParticlesDecalsTab()
    {
        auto& p = EngineSettings::GetInstance().Particles();
        auto& d = EngineSettings::GetInstance().Decals();

        if (Spark::Ui::SectionHeader("Particles"))
        {
            const ScopedPropertyGrid grid("Particles");
            if (Grid::DragInt("Max Particles", &p.maxParticles, 100, 100, 100000))
                m_modified = true;
            if (Grid::DragInt("Max Emitters", &p.maxEmitters, 1, 1, 1024))
                m_modified = true;
            if (Grid::DragFloat("Simulation Rate", &p.simulationRate, 1.0f, 15.0f, 120.0f, "%.0f Hz"))
                m_modified = true;
            if (Grid::DragFloat("LOD Distance Mult##Part", &p.lodDistanceMultiplier, 0.1f, 0.1f, 5.0f))
                m_modified = true;
            if (Grid::DragFloat("Global Scale##Part", &p.globalScale, 0.1f, 0.1f, 5.0f))
                m_modified = true;
            if (Grid::Checkbox("Soft Particles", &p.softParticles))
                m_modified = true;
            if (Grid::Checkbox("GPU Particles", &p.gpuParticles))
                m_modified = true;
        }

        if (Spark::Ui::SectionHeader("Decals", nullptr, false))
        {
            const ScopedPropertyGrid grid("Decals");
            if (Grid::Checkbox("Enable Decals", &d.enableDecals))
                m_modified = true;
            if (d.enableDecals)
            {
                if (Grid::DragInt("Max Decals", &d.maxDecals, 1, 1, 1024))
                    m_modified = true;
                if (Grid::DragFloat("Default Lifetime", &d.defaultLifetime, 1.0f, 0.0f, 300.0f, "%.0f s"))
                    m_modified = true;
                if (Grid::DragFloat("Fade Time", &d.fadeTime, 0.1f, 0.0f, 10.0f, "%.1f s"))
                    m_modified = true;
            }
        }
    }

    // =========================================================================
    // Memory Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderMemoryTab()
    {
        auto& mem = EngineSettings::GetInstance().Memory();

        if (Spark::Ui::SectionHeader("Streaming Budgets"))
        {
            const ScopedPropertyGrid grid("Streaming Budgets");
            if (Grid::DragInt("Texture Budget", &mem.textureStreamingBudgetMB, 32, 64, 4096, "%d MB"))
                m_modified = true;
            if (Grid::DragInt("Mesh Budget", &mem.meshStreamingBudgetMB, 32, 32, 2048, "%d MB"))
                m_modified = true;
            if (Grid::DragInt("Audio Budget", &mem.audioStreamingBudgetMB, 16, 16, 1024, "%d MB"))
                m_modified = true;
            if (Grid::DragInt("Shader Cache", &mem.shaderCacheSizeMB, 16, 16, 512, "%d MB"))
                m_modified = true;
        }

        if (Spark::Ui::SectionHeader("Garbage Collection", nullptr, false))
        {
            const ScopedPropertyGrid grid("Garbage Collection");
            if (Grid::DragFloat("GC Interval", &mem.gcInterval, 5.0f, 5.0f, 600.0f, "%.0f s"))
                m_modified = true;
            if (Grid::SliderFloat("GC Aggressiveness", &mem.gcAggressiveness, 0.0f, 1.0f))
                m_modified = true;
        }

        if (Spark::Ui::SectionHeader("Debug", nullptr, false))
        {
            const ScopedPropertyGrid grid("Debug");
            if (Grid::Checkbox("Memory Tracking", &mem.enableMemoryTracking))
                m_modified = true;
            Spark::Ui::StatusText(Spark::Ui::Status::Warning, "Requires restart");
        }
    }

    // =========================================================================
    // Online Services Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderOnlineServicesTab()
    {
        auto& os = EngineSettings::GetInstance().OnlineServices();

        {
            const ScopedPropertyGrid grid("Online Services");
            if (Grid::Checkbox("Enable Online Services", &os.enableOnlineServices))
                m_modified = true;
            Spark::Ui::StatusText(Spark::Ui::Status::Warning, "Requires restart");

            if (os.enableOnlineServices)
            {
                const char* backendItems[] = {"Null", "Steam", "Epic", "Custom"};
                if (Grid::Combo("Platform Backend", &os.platformBackend, backendItems, 4))
                    m_modified = true;
                if (Grid::DragFloat("Session Timeout", &os.sessionTimeout, 10.0f, 30.0f, 3600.0f, "%.0f s"))
                    m_modified = true;
                if (Grid::DragInt("Max Search Results", &os.maxSessionSearchResults, 1, 5, 200))
                    m_modified = true;
            }
        }

        if (os.enableOnlineServices)
        {
            if (Spark::Ui::SectionHeader("Voice Chat", nullptr, false))
            {
                const ScopedPropertyGrid grid("Voice Chat");
                if (Grid::Checkbox("Enable Voice Chat", &os.enableVoiceChat))
                    m_modified = true;
                if (os.enableVoiceChat)
                {
                    if (Grid::SliderFloat("Voice Volume", &os.voiceChatVolume, 0.0f, 1.0f))
                        m_modified = true;
                    if (Grid::Checkbox("Push to Talk", &os.pushToTalk))
                        m_modified = true;
                }
            }
        }
    }

    // =========================================================================
    // Project Info Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderProjectInfoTab()
    {
        {
            const ScopedPropertyGrid grid("Project");
            Spark::Ui::PropertyLabel("Engine");
            ImGui::TextUnformatted("SparkEngine");
            Spark::Ui::PropertyLabel("Settings File");
            Spark::Ui::MonoText("%s", EngineSettings::GetInstance().GetFilePath().c_str());
        }
        ImGui::Separator();

        ImGui::PushTextWrapPos(0.0f);
        Spark::Ui::SecondaryText("Project settings are loaded from settings.ini at startup and saved on demand. "
                                 "Changes made here take effect immediately for most subsystems.");
        ImGui::PopTextWrapPos();
    }

} // namespace SparkEditor
