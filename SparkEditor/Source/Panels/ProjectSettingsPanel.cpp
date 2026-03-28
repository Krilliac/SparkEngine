/**
 * @file ProjectSettingsPanel.cpp
 * @brief Implementation of the project settings editor panel
 */

#include "ProjectSettingsPanel.h"
#include "../Core/EditorIcons.h"
#include "Core/EngineSettings.h"
#include <imgui.h>
#include <iostream>

namespace SparkEditor
{

    ProjectSettingsPanel::ProjectSettingsPanel() : EditorPanel("Project Settings", "project_settings_panel") {}

    bool ProjectSettingsPanel::Initialize()
    {
        std::cout << "Initializing Project Settings panel\n";
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
                if (ImGui::BeginTabItem(ICON_FA_CUBES " Rendering"))
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
                if (ImGui::BeginTabItem(ICON_FA_WIFI " Network"))
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
        std::cout << "Shutting down Project Settings panel\n";
    }

    // =========================================================================
    // Toolbar
    // =========================================================================

    void ProjectSettingsPanel::RenderToolbar()
    {
        auto& settings = EngineSettings::GetInstance();

        if (ImGui::Button(ICON_FA_SAVE " Save"))
        {
            settings.Save();
            m_modified = false;
        }

        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_UNDO " Reset to Defaults"))
        {
            settings.ResetToDefaults();
            m_modified = true;
        }

        if (m_modified)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, 0.8f, 0, 1), ICON_FA_EXCLAMATION_TRIANGLE " Unsaved changes");
        }
    }

    // =========================================================================
    // Graphics Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderGraphicsTab()
    {
        auto& gfx = EngineSettings::GetInstance().Graphics();

        if (ImGui::DragInt("Window Width", &gfx.windowWidth, 1, 640, 7680))
            m_modified = true;
        if (ImGui::DragInt("Window Height", &gfx.windowHeight, 1, 480, 4320))
            m_modified = true;
        if (ImGui::Checkbox("Fullscreen", &gfx.fullscreen))
            m_modified = true;
        if (ImGui::Checkbox("VSync", &gfx.vsync))
            m_modified = true;

        const char* aaItems[] = {"Off", "2x MSAA", "4x MSAA", "8x MSAA"};
        int aaIndex = 0;
        if (gfx.antiAliasing == 2)
            aaIndex = 1;
        else if (gfx.antiAliasing == 4)
            aaIndex = 2;
        else if (gfx.antiAliasing >= 8)
            aaIndex = 3;
        if (ImGui::Combo("Anti-Aliasing", &aaIndex, aaItems, 4))
        {
            const int aaMap[] = {1, 2, 4, 8};
            gfx.antiAliasing = aaMap[aaIndex];
            m_modified = true;
        }

        const char* shadowItems[] = {"Off", "Low", "Medium", "High"};
        if (ImGui::Combo("Shadow Quality", &gfx.shadowQuality, shadowItems, 4))
            m_modified = true;

        if (ImGui::SliderFloat("Render Scale", &gfx.renderScale, 0.25f, 2.0f, "%.2f"))
            m_modified = true;
        if (ImGui::Checkbox("HDR", &gfx.hdr))
            m_modified = true;
    }

    // =========================================================================
    // Rendering Tab (Advanced)
    // =========================================================================

    void ProjectSettingsPanel::RenderRenderingTab()
    {
        auto& r = EngineSettings::GetInstance().Rendering();
        auto& game = EngineSettings::GetInstance().Game();

        if (ImGui::CollapsingHeader("Pipeline", ImGuiTreeNodeFlags_DefaultOpen))
        {
            const char* pathItems[] = {"Forward", "Deferred", "Forward+", "Clustered"};
            if (ImGui::Combo("Render Path", &r.renderPath, pathItems, 4))
                m_modified = true;

            const char* presetItems[] = {"Low", "Medium", "High", "Ultra", "Custom"};
            if (ImGui::Combo("Quality Preset", &r.qualityPreset, presetItems, 5))
                m_modified = true;

            if (ImGui::Checkbox("Show FPS", &game.showFPS))
                m_modified = true;
            if (ImGui::Checkbox("Show Debug Info", &game.showDebugInfo))
                m_modified = true;
        }

        if (ImGui::CollapsingHeader("Textures & Filtering"))
        {
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
            if (ImGui::Combo("Max Texture Size", &texIdx, texSizes, 5))
            {
                const int sizes[] = {512, 1024, 2048, 4096, 8192};
                r.maxTextureSize = sizes[texIdx];
                m_modified = true;
            }
            if (ImGui::Checkbox("Anisotropic Filtering", &r.anisotropicFiltering))
                m_modified = true;
            if (r.anisotropicFiltering)
            {
                if (ImGui::SliderInt("Anisotropy Level", &r.anisotropyLevel, 1, 16))
                    m_modified = true;
            }
        }

        if (ImGui::CollapsingHeader("Shadows & Lighting"))
        {
            if (ImGui::Checkbox("Shadows", &r.shadows))
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
                if (ImGui::Combo("Shadow Map Size", &sIdx, shadowSizes, 4))
                {
                    const int sizes[] = {512, 1024, 2048, 4096};
                    r.shadowMapSize = sizes[sIdx];
                    m_modified = true;
                }
                if (ImGui::SliderInt("Cascade Count", &r.cascadeCount, 1, 4))
                    m_modified = true;
            }
        }

        if (ImGui::CollapsingHeader("Effects Toggles"))
        {
            if (ImGui::Checkbox("Bloom", &r.bloom))
                m_modified = true;
            if (ImGui::Checkbox("SSAO", &r.ssao))
                m_modified = true;
            if (ImGui::Checkbox("TAA", &r.taa))
                m_modified = true;
            if (ImGui::Checkbox("Motion Blur", &r.motionBlur))
                m_modified = true;
        }

        if (ImGui::CollapsingHeader("Culling & LOD"))
        {
            if (ImGui::Checkbox("Frustum Culling", &r.frustumCulling))
                m_modified = true;
            if (ImGui::Checkbox("Occlusion Culling", &r.occlusionCulling))
                m_modified = true;
            if (ImGui::Checkbox("Level of Detail", &r.levelOfDetail))
                m_modified = true;
            if (ImGui::DragInt("Max Draw Calls", &r.maxDrawCalls, 10, 100, 100000))
                m_modified = true;
        }

        if (ImGui::CollapsingHeader("Dynamic Quality Scaler"))
        {
            auto& dq = EngineSettings::GetInstance().DynamicQuality();
            if (ImGui::Checkbox("Dynamic Quality Enabled", &dq.enabled))
                m_modified = true;
            if (dq.enabled)
            {
                if (ImGui::DragFloat("Target Frame Time", &dq.targetFrameTimeMs, 0.5f, 4.0f, 66.0f, "%.1f ms"))
                    m_modified = true;
                ImGui::Text("Render Scale: %.2f - %.2f (step %.2f)", dq.minRenderScale, dq.maxRenderScale,
                            dq.renderScaleStep);
                if (ImGui::DragFloat("Min Render Scale", &dq.minRenderScale, 0.05f, 0.25f, dq.maxRenderScale))
                    m_modified = true;
                if (ImGui::DragFloat("Max Render Scale", &dq.maxRenderScale, 0.05f, dq.minRenderScale, 2.0f))
                    m_modified = true;
                if (ImGui::DragFloat("Shadow Scale Min", &dq.minShadowScale, 0.05f, 0.1f, dq.maxShadowScale))
                    m_modified = true;
                if (ImGui::DragFloat("Shadow Scale Max", &dq.maxShadowScale, 0.05f, dq.minShadowScale, 1.0f))
                    m_modified = true;
            }
        }

        if (ImGui::CollapsingHeader("Debug"))
        {
            if (ImGui::Checkbox("Wireframe Mode", &r.wireframeMode))
                m_modified = true;
            if (ImGui::Checkbox("Debug Mode", &r.debugMode))
                m_modified = true;
            if (ImGui::Checkbox("GPU Timing", &r.enableGPUTiming))
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

        if (ImGui::CollapsingHeader("Bloom", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (ImGui::Checkbox("Bloom Enabled", &pp.bloomEnabled))
                m_modified = true;
            if (pp.bloomEnabled)
            {
                if (ImGui::DragFloat("Threshold", &pp.bloomThreshold, 0.05f, 0.0f, 10.0f))
                    m_modified = true;
                if (ImGui::DragFloat("Intensity##Bloom", &pp.bloomIntensity, 0.05f, 0.0f, 10.0f))
                    m_modified = true;
                if (ImGui::DragFloat("Radius", &pp.bloomRadius, 0.1f, 0.0f, 10.0f))
                    m_modified = true;
                if (ImGui::SliderFloat("Soft Knee", &pp.bloomSoftKnee, 0.0f, 1.0f))
                    m_modified = true;
                if (ImGui::SliderInt("Iterations", &pp.bloomIterations, 1, 16))
                    m_modified = true;
            }
        }

        if (ImGui::CollapsingHeader("Tone Mapping"))
        {
            const char* tmItems[] = {"None", "Reinhard", "ReinhardJodie", "Uncharted2", "ACES", "AgX", "FilmicALU"};
            if (ImGui::Combo("Operator", &pp.toneMappingOperator, tmItems, 7))
                m_modified = true;
            if (ImGui::DragFloat("Exposure", &pp.exposure, 0.05f, 0.01f, 20.0f))
                m_modified = true;
            if (ImGui::DragFloat("Gamma", &pp.gamma, 0.05f, 0.5f, 5.0f))
                m_modified = true;
            if (ImGui::DragFloat("White Point", &pp.whitePoint, 0.1f, 1.0f, 50.0f))
                m_modified = true;
        }

        if (ImGui::CollapsingHeader("Color Grading"))
        {
            if (ImGui::Checkbox("Color Grading Enabled", &pp.colorGradingEnabled))
                m_modified = true;
            if (pp.colorGradingEnabled)
            {
                if (ImGui::SliderFloat("Temperature", &pp.temperature, -1.0f, 1.0f))
                    m_modified = true;
                if (ImGui::SliderFloat("Tint", &pp.tint, -1.0f, 1.0f))
                    m_modified = true;
                if (ImGui::DragFloat("Contrast", &pp.contrast, 0.01f, 0.0f, 3.0f))
                    m_modified = true;
                if (ImGui::DragFloat("Brightness", &pp.brightness, 0.01f, -1.0f, 1.0f))
                    m_modified = true;
                if (ImGui::DragFloat("Saturation", &pp.saturation, 0.01f, 0.0f, 3.0f))
                    m_modified = true;
            }
        }

        if (ImGui::CollapsingHeader("SSAO"))
        {
            if (ImGui::Checkbox("SSAO Enabled", &ssao.enabled))
                m_modified = true;
            if (ssao.enabled)
            {
                if (ImGui::DragFloat("Radius##SSAO", &ssao.radius, 0.05f, 0.01f, 5.0f))
                    m_modified = true;
                if (ImGui::DragFloat("Intensity##SSAO", &ssao.intensity, 0.05f, 0.0f, 5.0f))
                    m_modified = true;
                if (ImGui::SliderInt("Sample Count##SSAO", &ssao.sampleCount, 4, 64))
                    m_modified = true;
                if (ImGui::DragFloat("Bias", &ssao.bias, 0.005f, 0.0f, 0.5f, "%.3f"))
                    m_modified = true;
                if (ImGui::Checkbox("Blur##SSAO", &ssao.blur))
                    m_modified = true;
            }
        }

        if (ImGui::CollapsingHeader("Screen-Space Reflections"))
        {
            if (ImGui::Checkbox("SSR Enabled", &ssr.enabled))
                m_modified = true;
            if (ssr.enabled)
            {
                if (ImGui::DragFloat("Max Distance##SSR", &ssr.maxDistance, 1.0f, 1.0f, 500.0f))
                    m_modified = true;
                if (ImGui::SliderInt("Max Steps##SSR", &ssr.maxSteps, 8, 256))
                    m_modified = true;
                if (ImGui::DragFloat("Thickness##SSR", &ssr.thickness, 0.05f, 0.01f, 5.0f))
                    m_modified = true;
                if (ImGui::DragFloat("Fade Start", &ssr.fadeStart, 1.0f, 0.0f, ssr.fadeEnd))
                    m_modified = true;
                if (ImGui::DragFloat("Fade End", &ssr.fadeEnd, 1.0f, ssr.fadeStart, 500.0f))
                    m_modified = true;
            }
        }

        if (ImGui::CollapsingHeader("Volumetric Lighting"))
        {
            if (ImGui::Checkbox("Volumetric Enabled", &vol.enabled))
                m_modified = true;
            if (vol.enabled)
            {
                if (ImGui::SliderInt("Sample Count##Vol", &vol.sampleCount, 8, 128))
                    m_modified = true;
                if (ImGui::DragFloat("Scattering", &vol.scattering, 0.01f, 0.0f, 1.0f))
                    m_modified = true;
                if (ImGui::DragFloat("Extinction", &vol.extinction, 0.001f, 0.0f, 0.1f, "%.4f"))
                    m_modified = true;
                if (ImGui::SliderFloat("Anisotropy##Vol", &vol.anisotropy, -1.0f, 1.0f))
                    m_modified = true;
            }
        }

        if (ImGui::CollapsingHeader("Temporal Anti-Aliasing"))
        {
            if (ImGui::Checkbox("TAA Enabled", &taa.enabled))
                m_modified = true;
            if (taa.enabled)
            {
                const char* qualItems[] = {"Low", "Medium", "High", "Ultra"};
                if (ImGui::Combo("Quality##TAA", &taa.quality, qualItems, 4))
                    m_modified = true;
                const char* jitterItems[] = {"Halton 2,3", "Blue Noise", "Uniform 8x", "Interleaved Gradient"};
                if (ImGui::Combo("Jitter Pattern", &taa.jitterPattern, jitterItems, 4))
                    m_modified = true;
                if (ImGui::SliderFloat("History Blend", &taa.historyBlendFactor, 0.0f, 1.0f))
                    m_modified = true;
                if (ImGui::SliderFloat("Sharpness##TAA", &taa.sharpness, 0.0f, 1.0f))
                    m_modified = true;
                if (ImGui::SliderFloat("Ghosting Rejection", &taa.ghostingRejectionStrength, 0.0f, 1.0f))
                    m_modified = true;
                if (ImGui::SliderFloat("Flicker Reduction", &taa.flickerReduction, 0.0f, 1.0f))
                    m_modified = true;
                if (ImGui::Checkbox("Use Motion Vectors", &taa.useMotionVectors))
                    m_modified = true;
                if (ImGui::Checkbox("Use YCoCg", &taa.useYCoCg))
                    m_modified = true;
            }
        }

        if (ImGui::CollapsingHeader("Motion Blur"))
        {
            if (ImGui::Checkbox("Motion Blur Enabled", &mb.enabled))
                m_modified = true;
            if (mb.enabled)
            {
                const char* mbTypes[] = {"Camera Only", "Per-Object", "Combined"};
                if (ImGui::Combo("Type##MB", &mb.type, mbTypes, 3))
                    m_modified = true;
                if (ImGui::SliderFloat("Intensity##MB", &mb.intensity, 0.0f, 2.0f))
                    m_modified = true;
                if (ImGui::SliderInt("Sample Count##MB", &mb.sampleCount, 2, 32))
                    m_modified = true;
                if (ImGui::DragFloat("Max Blur Radius", &mb.maxBlurRadius, 1.0f, 1.0f, 128.0f))
                    m_modified = true;
                if (ImGui::DragFloat("Velocity Scale", &mb.velocityScale, 0.1f, 0.0f, 10.0f))
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

        if (ImGui::CollapsingHeader("Volume", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (ImGui::SliderFloat("Master Volume", &audio.masterVolume, 0.0f, 1.0f))
                m_modified = true;
            if (ImGui::SliderFloat("SFX Volume", &audio.sfxVolume, 0.0f, 1.0f))
                m_modified = true;
            if (ImGui::SliderFloat("Music Volume", &audio.musicVolume, 0.0f, 1.0f))
                m_modified = true;
            if (ImGui::SliderFloat("Voice Volume", &audio.voiceVolume, 0.0f, 1.0f))
                m_modified = true;
            if (ImGui::Checkbox("Mute on Focus Loss", &audio.muteOnFocusLoss))
                m_modified = true;
        }

        if (ImGui::CollapsingHeader("3D Audio & Effects"))
        {
            if (ImGui::Checkbox("Enable 3D Audio", &ext.enable3D))
                m_modified = true;
            if (ImGui::Checkbox("Enable Reverb", &ext.enableReverb))
                m_modified = true;
            if (ImGui::Checkbox("Enable EAX", &ext.enableEAX))
                m_modified = true;
            if (ImGui::DragFloat("Doppler Scale", &ext.dopplerScale, 0.1f, 0.0f, 10.0f))
                m_modified = true;
            if (ImGui::DragFloat("Distance Scale", &ext.distanceScale, 0.1f, 0.1f, 100.0f))
                m_modified = true;
            if (ImGui::DragInt("Max Sources", &ext.maxSources, 1, 8, 256))
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

        if (ImGui::CollapsingHeader("Mouse", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (ImGui::DragFloat("Sensitivity", &ctrl.mouseSensitivity, 0.05f, 0.05f, 10.0f))
                m_modified = true;
            if (ImGui::Checkbox("Invert Mouse Y", &ctrl.invertMouseY))
                m_modified = true;
            if (ImGui::DragFloat("Dead Zone", &ctrl.mouseDeadZone, 0.01f, 0.0f, 0.5f))
                m_modified = true;
            if (ImGui::Checkbox("Raw Mouse Input", &ctrl.rawMouseInput))
                m_modified = true;
            if (ImGui::Checkbox("Mouse Acceleration", &ctrl.mouseAcceleration))
                m_modified = true;
        }

        if (ImGui::CollapsingHeader("Game Display"))
        {
            const char* diffItems[] = {"Easy", "Normal", "Hard", "Nightmare"};
            int diffIdx = 1;
            if (game.difficulty == "Easy")
                diffIdx = 0;
            else if (game.difficulty == "Hard")
                diffIdx = 2;
            else if (game.difficulty == "Nightmare")
                diffIdx = 3;
            if (ImGui::Combo("Difficulty", &diffIdx, diffItems, 4))
            {
                const char* diffNames[] = {"Easy", "Normal", "Hard", "Nightmare"};
                game.difficulty = diffNames[diffIdx];
                m_modified = true;
            }
            if (ImGui::DragFloat("Field of View", &game.fieldOfView, 1.0f, 30.0f, 150.0f, "%.0f"))
                m_modified = true;
        }
    }

    // =========================================================================
    // Physics Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderPhysicsTab()
    {
        auto& phys = EngineSettings::GetInstance().Physics();

        ImGui::Text("Gravity");
        if (ImGui::DragFloat("Gravity X", &phys.gravityX, 0.1f, -100.0f, 100.0f))
            m_modified = true;
        if (ImGui::DragFloat("Gravity Y", &phys.gravityY, 0.1f, -100.0f, 100.0f))
            m_modified = true;
        if (ImGui::DragFloat("Gravity Z", &phys.gravityZ, 0.1f, -100.0f, 100.0f))
            m_modified = true;

        ImGui::Separator();
        if (ImGui::DragFloat("Fixed Timestep", &phys.fixedTimestep, 0.001f, 0.001f, 0.1f, "%.4f"))
            m_modified = true;
        if (ImGui::DragInt("Max Sub-steps", &phys.maxSubSteps, 1, 1, 16))
            m_modified = true;
        if (ImGui::DragFloat("Default Friction", &phys.defaultFriction, 0.01f, 0.0f, 1.0f))
            m_modified = true;
        if (ImGui::DragFloat("Default Restitution", &phys.defaultRestitution, 0.01f, 0.0f, 1.0f))
            m_modified = true;
        if (ImGui::Checkbox("Debug Draw", &phys.debugDraw))
            m_modified = true;
    }

    // =========================================================================
    // AI Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderAITab()
    {
        auto& ai = EngineSettings::GetInstance().AI();

        if (ImGui::DragFloat("Detection Range", &ai.detectionRange, 0.5f, 1.0f, 200.0f))
            m_modified = true;
        if (ImGui::DragFloat("Attack Range", &ai.attackRange, 0.5f, 1.0f, 100.0f))
            m_modified = true;
        if (ImGui::DragFloat("Melee Range", &ai.meleeRange, 0.1f, 0.5f, 10.0f))
            m_modified = true;
        if (ImGui::DragFloat("Move Speed", &ai.moveSpeed, 0.1f, 0.1f, 50.0f))
            m_modified = true;
        if (ImGui::DragFloat("Turn Speed", &ai.turnSpeed, 1.0f, 10.0f, 720.0f, "%.0f deg/s"))
            m_modified = true;
        if (ImGui::SliderFloat("Accuracy", &ai.accuracy, 0.0f, 1.0f))
            m_modified = true;
        if (ImGui::DragFloat("Reaction Time", &ai.reactionTime, 0.01f, 0.0f, 5.0f, "%.2f s"))
            m_modified = true;
        if (ImGui::DragFloat("Cover Search Radius", &ai.coverSearchRadius, 0.5f, 1.0f, 100.0f))
            m_modified = true;
        if (ImGui::Checkbox("Can Strafe", &ai.canStrafe))
            m_modified = true;
        if (ImGui::Checkbox("Can Sprint", &ai.canSprint))
            m_modified = true;
        if (ImGui::Checkbox("Can Use Cover", &ai.canUseCover))
            m_modified = true;
    }

    // =========================================================================
    // Player Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderPlayerTab()
    {
        auto& p = EngineSettings::GetInstance().Player();

        if (ImGui::CollapsingHeader("Health & Defense", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (ImGui::DragFloat("Max Health", &p.maxHealth, 1.0f, 1.0f, 1000.0f))
                m_modified = true;
            if (ImGui::DragFloat("Max Armor", &p.maxArmor, 1.0f, 0.0f, 1000.0f))
                m_modified = true;
            if (ImGui::DragFloat("Max Shield", &p.maxShield, 1.0f, 0.0f, 500.0f))
                m_modified = true;
            if (ImGui::DragFloat("Shield Recharge Rate", &p.shieldRechargeRate, 0.5f, 0.0f, 100.0f))
                m_modified = true;
            if (ImGui::DragFloat("Shield Recharge Delay", &p.shieldRechargeDelay, 0.1f, 0.0f, 30.0f, "%.1f s"))
                m_modified = true;
        }

        if (ImGui::CollapsingHeader("Movement", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (ImGui::DragFloat("Move Speed##Player", &p.moveSpeed, 0.1f, 0.1f, 50.0f))
                m_modified = true;
            if (ImGui::DragFloat("Jump Height", &p.jumpHeight, 0.1f, 0.0f, 20.0f))
                m_modified = true;
            if (ImGui::DragFloat("Gravity Force", &p.gravityForce, 0.5f, 1.0f, 100.0f))
                m_modified = true;
            if (ImGui::DragFloat("Friction##Player", &p.friction, 0.01f, 0.0f, 1.0f))
                m_modified = true;
            if (ImGui::DragFloat("Sprint Multiplier", &p.sprintMultiplier, 0.1f, 1.0f, 5.0f))
                m_modified = true;
            if (ImGui::DragFloat("Crouch Multiplier", &p.crouchMultiplier, 0.05f, 0.1f, 1.0f))
                m_modified = true;
            if (ImGui::DragFloat("ADS Speed Multiplier", &p.adsSpeedMultiplier, 0.05f, 0.1f, 1.0f))
                m_modified = true;
        }

        if (ImGui::CollapsingHeader("Energy"))
        {
            if (ImGui::DragFloat("Max Energy", &p.maxEnergy, 1.0f, 0.0f, 500.0f))
                m_modified = true;
            if (ImGui::DragFloat("Energy Regen Rate", &p.energyRegenRate, 0.5f, 0.0f, 100.0f))
                m_modified = true;
        }
    }

    // =========================================================================
    // Gameplay Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderGameplayTab()
    {
        auto& gm = EngineSettings::GetInstance().GameMode();

        if (ImGui::CollapsingHeader("Scoring", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (ImGui::DragInt("Score Limit", &gm.scoreLimit, 1, 0, 1000))
                m_modified = true;
            if (ImGui::DragInt("Kill Points", &gm.killPoints, 1, 0, 500))
                m_modified = true;
            if (ImGui::DragInt("Assist Points", &gm.assistPoints, 1, 0, 500))
                m_modified = true;
            if (ImGui::DragInt("Objective Points", &gm.objectivePoints, 1, 0, 1000))
                m_modified = true;
            if (ImGui::DragInt("Headshot Bonus", &gm.headshotBonus, 1, 0, 200))
                m_modified = true;
            if (ImGui::DragInt("Death Penalty", &gm.deathPenalty, 1, 0, 200))
                m_modified = true;
        }

        if (ImGui::CollapsingHeader("Rules", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (ImGui::DragInt("Round Limit", &gm.roundLimit, 1, 0, 100))
                m_modified = true;
            if (ImGui::DragFloat("Time Limit", &gm.timeLimit, 1.0f, 0.0f, 3600.0f, "%.0f s"))
                m_modified = true;
            if (ImGui::DragFloat("Respawn Delay", &gm.respawnDelay, 0.1f, 0.0f, 30.0f, "%.1f s"))
                m_modified = true;
            if (ImGui::Checkbox("Auto Respawn", &gm.autoRespawn))
                m_modified = true;
            if (ImGui::Checkbox("Friendly Fire", &gm.friendlyFire))
                m_modified = true;
            if (ImGui::Checkbox("Headshots", &gm.headshots))
                m_modified = true;
        }

        if (ImGui::CollapsingHeader("Multipliers"))
        {
            if (ImGui::DragFloat("Damage Multiplier", &gm.damageMultiplier, 0.1f, 0.1f, 10.0f))
                m_modified = true;
            if (ImGui::DragFloat("Health Multiplier", &gm.healthMultiplier, 0.1f, 0.1f, 10.0f))
                m_modified = true;
            if (ImGui::DragFloat("Speed Multiplier", &gm.speedMultiplier, 0.1f, 0.1f, 10.0f))
                m_modified = true;
            if (ImGui::DragFloat("Headshot Multiplier", &gm.headshotMultiplier, 0.1f, 1.0f, 10.0f))
                m_modified = true;
        }
    }

    // =========================================================================
    // Camera Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderCameraTab()
    {
        auto& cam = EngineSettings::GetInstance().Camera();

        if (ImGui::DragFloat("Move Speed", &cam.moveSpeed, 0.1f, 0.1f, 100.0f))
            m_modified = true;
        if (ImGui::DragFloat("Rotation Speed", &cam.rotationSpeed, 0.1f, 0.1f, 20.0f))
            m_modified = true;
        if (ImGui::DragFloat("Default FOV", &cam.defaultFov, 1.0f, 30.0f, 150.0f, "%.0f"))
            m_modified = true;
        if (ImGui::DragFloat("Zoomed FOV", &cam.zoomedFov, 1.0f, 10.0f, 90.0f, "%.0f"))
            m_modified = true;
        if (ImGui::Checkbox("Smooth Movement", &cam.smoothMovement))
            m_modified = true;
        if (ImGui::DragFloat("Near Plane", &cam.nearPlane, 0.01f, 0.001f, 10.0f, "%.3f"))
            m_modified = true;
        if (ImGui::DragFloat("Far Plane", &cam.farPlane, 10.0f, 10.0f, 100000.0f, "%.0f"))
            m_modified = true;
    }

    // =========================================================================
    // Editor Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderEditorTab()
    {
        auto& ed = EngineSettings::GetInstance().Editor();

        if (ImGui::DragFloat("Grid Size", &ed.gridSize, 0.1f, 0.1f, 100.0f))
            m_modified = true;
        if (ImGui::Checkbox("Snap to Grid", &ed.snapToGrid))
            m_modified = true;
        if (ImGui::Checkbox("Show Grid", &ed.showGrid))
            m_modified = true;
        if (ImGui::DragFloat("Gizmo Scale", &ed.gizmoScale, 0.1f, 0.1f, 10.0f))
            m_modified = true;
        if (ImGui::Checkbox("Autosave Enabled", &ed.autosaveEnabled))
            m_modified = true;
        if (ImGui::DragFloat("Autosave Interval", &ed.autosaveIntervalSeconds, 10.0f, 30.0f, 3600.0f, "%.0f s"))
            m_modified = true;
        if (ImGui::DragInt("Undo History Size", &ed.undoHistorySize, 1, 10, 1000))
            m_modified = true;
    }

    // =========================================================================
    // Network Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderNetworkTab()
    {
        auto& net = EngineSettings::GetInstance().Network();

        if (ImGui::CollapsingHeader("Server", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (ImGui::DragInt("Server Port", &net.serverPort, 1, 1024, 65535))
                m_modified = true;
            if (ImGui::DragInt("Max Clients", &net.maxClients, 1, 1, 128))
                m_modified = true;
            if (ImGui::DragFloat("Connection Timeout", &net.connectionTimeout, 0.5f, 1.0f, 60.0f, "%.1f s"))
                m_modified = true;
            if (ImGui::DragFloat("Heartbeat Interval", &net.heartbeatInterval, 0.1f, 0.1f, 10.0f, "%.1f s"))
                m_modified = true;
        }

        if (ImGui::CollapsingHeader("Replication"))
        {
            if (ImGui::DragFloat("Replication Rate", &net.replicationRate, 1.0f, 1.0f, 120.0f, "%.0f Hz"))
                m_modified = true;
            if (ImGui::DragFloat("Reliable Retransmit Base", &net.reliableRetransmitBase, 0.05f, 0.1f, 5.0f, "%.2f s"))
                m_modified = true;
            if (ImGui::DragInt("Max Reliable Retries", &net.maxReliableRetries, 1, 1, 20))
                m_modified = true;
        }

        if (ImGui::CollapsingHeader("Buffers & Features"))
        {
            if (ImGui::DragInt("Send Buffer Size", &net.sendBufferSize, 1024, 4096, 1048576))
                m_modified = true;
            if (ImGui::DragInt("Receive Buffer Size", &net.receiveBufferSize, 1024, 4096, 1048576))
                m_modified = true;
            if (ImGui::Checkbox("Enable Compression", &net.enableCompression))
                m_modified = true;
            if (ImGui::Checkbox("Enable Encryption", &net.enableEncryption))
                m_modified = true;
        }

        if (ImGui::CollapsingHeader("Lag Simulation (Dev Only)"))
        {
            ImGui::TextColored(ImVec4(1, 0.6f, 0, 1), "For development/testing only");
            if (ImGui::DragFloat("Simulated Latency", &net.simulatedLatencyMs, 1.0f, 0.0f, 500.0f, "%.0f ms"))
                m_modified = true;
            if (ImGui::SliderFloat("Simulated Packet Loss", &net.simulatedPacketLoss, 0.0f, 1.0f, "%.2f"))
                m_modified = true;
            if (ImGui::DragFloat("Simulated Jitter", &net.simulatedJitterMs, 0.5f, 0.0f, 100.0f, "%.0f ms"))
                m_modified = true;
        }
    }

    // =========================================================================
    // Scripting Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderScriptingTab()
    {
        auto& s = EngineSettings::GetInstance().Scripting();

        if (ImGui::CollapsingHeader("Hot Reload", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (ImGui::Checkbox("Hot Reload Enabled", &s.hotReloadEnabled))
                m_modified = true;
            if (s.hotReloadEnabled)
            {
                if (ImGui::DragFloat("Poll Interval", &s.hotReloadPollInterval, 0.1f, 0.1f, 10.0f, "%.1f s"))
                    m_modified = true;
            }
        }

        if (ImGui::CollapsingHeader("Execution", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (ImGui::DragInt("Context Pool Size", &s.contextPoolSize, 1, 1, 64))
                m_modified = true;
            if (ImGui::DragFloat("Execution Timeout", &s.executionTimeoutMs, 10.0f, 10.0f, 10000.0f, "%.0f ms"))
                m_modified = true;
            if (ImGui::DragInt("Max Call Stack Depth", &s.maxCallStackDepth, 1, 8, 256))
                m_modified = true;
            if (ImGui::DragInt("Max Script Memory", &s.maxScriptMemoryMB, 1, 8, 512, "%d MB"))
                m_modified = true;
        }

        if (ImGui::CollapsingHeader("Debug"))
        {
            if (ImGui::Checkbox("Generate Debug Info", &s.generateDebugInfo))
                m_modified = true;
            if (ImGui::Checkbox("Enable Profiler", &s.enableProfiler))
                m_modified = true;
        }
    }

    // =========================================================================
    // Animation Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderAnimationTab()
    {
        auto& a = EngineSettings::GetInstance().Animation();

        if (ImGui::CollapsingHeader("Blending & Playback", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (ImGui::DragFloat("Default Blend Time", &a.defaultBlendTime, 0.01f, 0.0f, 2.0f, "%.2f s"))
                m_modified = true;
            if (ImGui::DragInt("Max Active Montages", &a.maxActiveMontages, 1, 1, 16))
                m_modified = true;
            if (ImGui::Checkbox("Enable Root Motion", &a.enableRootMotion))
                m_modified = true;
            if (ImGui::Checkbox("Enable Animation Events", &a.enableAnimationEvents))
                m_modified = true;
        }

        if (ImGui::CollapsingHeader("Inverse Kinematics"))
        {
            if (ImGui::DragInt("IK Solver Iterations", &a.ikSolverIterations, 1, 1, 50))
                m_modified = true;
            if (ImGui::DragFloat("IK Tolerance", &a.ikTolerance, 0.0001f, 0.0001f, 0.1f, "%.4f"))
                m_modified = true;
        }

        if (ImGui::CollapsingHeader("Quality & LOD"))
        {
            const char* compItems[] = {"None", "Low", "Medium", "High"};
            if (ImGui::Combo("Compression Quality", &a.compressionQuality, compItems, 4))
                m_modified = true;
            if (ImGui::DragFloat("LOD Distance Multiplier", &a.lodDistanceMultiplier, 0.1f, 0.1f, 5.0f))
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
                if (ImGui::Combo(label, &extIdx, extItems, 8))
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
                if (ImGui::Combo(label, &idx, levelItems, 7))
                {
                    level = levelItems[idx];
                    m_modified = true;
                }
            }
        };

        if (ImGui::CollapsingHeader("Global", ImGuiTreeNodeFlags_DefaultOpen))
        {
            levelCombo("Global Level", log.globalLevel);
            levelCombo("Stack Trace Level", log.stackTraceLevel);
        }

        if (ImGui::CollapsingHeader("Per-Category Overrides"))
        {
            ImGui::TextWrapped("Set per-category levels, or leave as '(Use Global)' to inherit.");
            ImGui::Separator();
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
    // Project Info Tab
    // =========================================================================

    void ProjectSettingsPanel::RenderProjectInfoTab()
    {
        ImGui::Text("Engine: SparkEngine");
        ImGui::Text("Settings File: %s", EngineSettings::GetInstance().GetFilePath().c_str());
        ImGui::Separator();

        ImGui::TextWrapped("Project settings are loaded from settings.ini at startup and saved on demand. "
                           "Changes made here take effect immediately for most subsystems.");
    }

} // namespace SparkEditor
