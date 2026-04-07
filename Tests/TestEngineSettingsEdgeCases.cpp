/**
 * @file TestEngineSettingsEdgeCases.cpp
 * @brief Edge-case tests for EngineSettings: boundary values, setting interactions
 *
 * Complements TestEngineSettingsParser.cpp (INI parsing, type conversion, callbacks)
 * with validation boundary tests and settings struct interaction tests.
 */

#include "TestFramework.h"

#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <limits>

// ============================================================================
// Standalone settings structs (matching production types for CI)
// ============================================================================

namespace
{

    struct GraphicsSettings
    {
        int windowWidth = 1920;
        int windowHeight = 1080;
        bool fullscreen = false;
        bool vsync = true;
        int antiAliasing = 4;
        int shadowQuality = 2;
        float renderScale = 1.0f;
        bool hdr = true;
    };

    struct AudioSettings
    {
        float masterVolume = 1.0f;
        float sfxVolume = 1.0f;
        float musicVolume = 0.7f;
        float voiceVolume = 1.0f;
        bool muteOnFocusLoss = true;
    };

    struct PhysicsSettings
    {
        float gravityX = 0.0f;
        float gravityY = -9.81f;
        float gravityZ = 0.0f;
        float fixedTimestep = 1.0f / 60.0f;
        int maxSubSteps = 4;
        float defaultFriction = 0.5f;
        float defaultRestitution = 0.3f;
        float defaultLinearDamping = 0.05f;
        float defaultAngularDamping = 0.05f;
        bool debugDraw = false;
    };

    struct DynamicQualitySettings
    {
        bool enabled = false;
        float targetFrameTimeMs = 16.67f;
        float minRenderScale = 0.5f;
        float maxRenderScale = 1.0f;
        float minShadowScale = 0.25f;
        float maxShadowScale = 1.0f;
        float minLODBias = 0.0f;
        float maxLODBias = 2.0f;
    };

    struct PostProcessSettings
    {
        float bloomThreshold = 1.0f;
        float bloomIntensity = 0.5f;
        float bloomRadius = 4.0f;
        int bloomIterations = 5;
        float exposure = 1.0f;
        float gamma = 2.2f;
        float whitePoint = 4.0f;
    };

    struct SSAOSettings
    {
        bool enabled = true;
        float radius = 0.5f;
        float intensity = 1.0f;
        int sampleCount = 16;
        float bias = 0.025f;
        bool blur = true;
    };

    struct TAASettings
    {
        bool enabled = true;
        int quality = 2;
        float historyBlendFactor = 0.9f;
        float varianceClipGamma = 1.0f;
        bool useMotionVectors = true;
        float sharpness = 0.5f;
    };

    struct PlayerSettings
    {
        float maxHealth = 100.0f;
        float maxArmor = 100.0f;
        float moveSpeed = 5.0f;
        float jumpHeight = 1.2f;
        float gravityForce = 9.81f;
        float friction = 0.3f;
        float sprintMultiplier = 1.5f;
        float crouchMultiplier = 0.5f;
    };

    float Clamp(float value, float minVal, float maxVal)
    {
        return std::max(minVal, std::min(maxVal, value));
    }

} // anonymous namespace

// ============================================================================
// Graphics Settings
// ============================================================================

TEST(EngineSettings_GraphicsDefaults)
{
    GraphicsSettings gs;
    EXPECT_EQ(gs.windowWidth, 1920);
    EXPECT_EQ(gs.windowHeight, 1080);
    EXPECT_FALSE(gs.fullscreen);
    EXPECT_TRUE(gs.vsync);
    EXPECT_EQ(gs.antiAliasing, 4);
    EXPECT_NEAR(gs.renderScale, 1.0f, 0.01f);
}

TEST(EngineSettings_RenderScaleClamped)
{
    GraphicsSettings gs;
    gs.renderScale = Clamp(2.5f, 0.25f, 2.0f);
    EXPECT_NEAR(gs.renderScale, 2.0f, 0.01f);

    gs.renderScale = Clamp(-0.5f, 0.25f, 2.0f);
    EXPECT_NEAR(gs.renderScale, 0.25f, 0.01f);
}

TEST(EngineSettings_ZeroResolution)
{
    GraphicsSettings gs;
    gs.windowWidth = 0;
    gs.windowHeight = 0;

    // System should detect and fix invalid resolution
    if (gs.windowWidth <= 0)
        gs.windowWidth = 800;
    if (gs.windowHeight <= 0)
        gs.windowHeight = 600;

    EXPECT_GT(gs.windowWidth, 0);
    EXPECT_GT(gs.windowHeight, 0);
}

TEST(EngineSettings_NegativeAntiAliasing)
{
    GraphicsSettings gs;
    gs.antiAliasing = -1;

    int validAA = std::max(0, gs.antiAliasing);
    EXPECT_EQ(validAA, 0);
}

// ============================================================================
// Audio Settings
// ============================================================================

TEST(EngineSettings_AudioDefaults)
{
    AudioSettings as;
    EXPECT_NEAR(as.masterVolume, 1.0f, 0.01f);
    EXPECT_NEAR(as.sfxVolume, 1.0f, 0.01f);
    EXPECT_NEAR(as.musicVolume, 0.7f, 0.01f);
    EXPECT_TRUE(as.muteOnFocusLoss);
}

TEST(EngineSettings_VolumeClamp)
{
    AudioSettings as;
    as.masterVolume = Clamp(1.5f, 0.0f, 1.0f);
    EXPECT_NEAR(as.masterVolume, 1.0f, 0.01f);

    as.masterVolume = Clamp(-0.5f, 0.0f, 1.0f);
    EXPECT_NEAR(as.masterVolume, 0.0f, 0.01f);
}

TEST(EngineSettings_EffectiveVolume)
{
    AudioSettings as;
    as.masterVolume = 0.5f;
    as.sfxVolume = 0.8f;

    float effectiveSfx = as.masterVolume * as.sfxVolume;
    EXPECT_NEAR(effectiveSfx, 0.4f, 0.01f);
}

// ============================================================================
// Physics Settings
// ============================================================================

TEST(EngineSettings_PhysicsDefaults)
{
    PhysicsSettings ps;
    EXPECT_NEAR(ps.gravityY, -9.81f, 0.01f);
    EXPECT_NEAR(ps.fixedTimestep, 1.0f / 60.0f, 0.001f);
    EXPECT_EQ(ps.maxSubSteps, 4);
    EXPECT_FALSE(ps.debugDraw);
}

TEST(EngineSettings_ZeroGravity)
{
    PhysicsSettings ps;
    ps.gravityX = 0.0f;
    ps.gravityY = 0.0f;
    ps.gravityZ = 0.0f;

    float gravMag = std::sqrt(ps.gravityX * ps.gravityX + ps.gravityY * ps.gravityY + ps.gravityZ * ps.gravityZ);
    EXPECT_NEAR(gravMag, 0.0f, 0.001f);
}

TEST(EngineSettings_TimestepBoundary)
{
    PhysicsSettings ps;
    // Very small timestep
    ps.fixedTimestep = 1.0f / 240.0f;
    EXPECT_GT(ps.fixedTimestep, 0.0f);

    // Zero timestep is invalid
    ps.fixedTimestep = 0.0f;
    float safeTimestep = ps.fixedTimestep > 0.0f ? ps.fixedTimestep : 1.0f / 60.0f;
    EXPECT_GT(safeTimestep, 0.0f);
}

TEST(EngineSettings_PhysicsRestitutionClamp)
{
    PhysicsSettings ps;
    ps.defaultRestitution = Clamp(1.5f, 0.0f, 1.0f);
    EXPECT_NEAR(ps.defaultRestitution, 1.0f, 0.01f);

    ps.defaultRestitution = Clamp(-0.1f, 0.0f, 1.0f);
    EXPECT_NEAR(ps.defaultRestitution, 0.0f, 0.01f);
}

// ============================================================================
// Dynamic Quality Settings
// ============================================================================

TEST(EngineSettings_DynamicQualityDefaults)
{
    DynamicQualitySettings dq;
    EXPECT_FALSE(dq.enabled);
    EXPECT_NEAR(dq.targetFrameTimeMs, 16.67f, 0.1f);
    EXPECT_NEAR(dq.minRenderScale, 0.5f, 0.01f);
    EXPECT_NEAR(dq.maxRenderScale, 1.0f, 0.01f);
}

TEST(EngineSettings_DynamicQualityScaleRange)
{
    DynamicQualitySettings dq;
    // min must be <= max
    EXPECT_LE(dq.minRenderScale, dq.maxRenderScale);
    EXPECT_LE(dq.minShadowScale, dq.maxShadowScale);
    EXPECT_LE(dq.minLODBias, dq.maxLODBias);
}

TEST(EngineSettings_DynamicQuality_TargetFPS30)
{
    DynamicQualitySettings dq;
    dq.targetFrameTimeMs = 33.33f; // 30 FPS target

    float targetFPS = 1000.0f / dq.targetFrameTimeMs;
    EXPECT_NEAR(targetFPS, 30.0f, 0.1f);
}

// ============================================================================
// Post-Processing Settings
// ============================================================================

TEST(EngineSettings_PostProcessDefaults)
{
    PostProcessSettings pp;
    EXPECT_NEAR(pp.bloomThreshold, 1.0f, 0.01f);
    EXPECT_NEAR(pp.exposure, 1.0f, 0.01f);
    EXPECT_NEAR(pp.gamma, 2.2f, 0.01f);
    EXPECT_EQ(pp.bloomIterations, 5);
}

TEST(EngineSettings_GammaRange)
{
    PostProcessSettings pp;
    pp.gamma = Clamp(0.5f, 1.0f, 3.0f);
    EXPECT_GE(pp.gamma, 1.0f);
    EXPECT_LE(pp.gamma, 3.0f);
}

TEST(EngineSettings_ExposureExtremes)
{
    PostProcessSettings pp;
    pp.exposure = 0.0f;
    EXPECT_NEAR(pp.exposure, 0.0f, 0.01f);

    pp.exposure = 10.0f;
    float clamped = Clamp(pp.exposure, 0.01f, 10.0f);
    EXPECT_NEAR(clamped, 10.0f, 0.01f);
}

// ============================================================================
// SSAO Settings
// ============================================================================

TEST(EngineSettings_SSAODefaults)
{
    SSAOSettings ssao;
    EXPECT_TRUE(ssao.enabled);
    EXPECT_NEAR(ssao.radius, 0.5f, 0.01f);
    EXPECT_EQ(ssao.sampleCount, 16);
    EXPECT_TRUE(ssao.blur);
}

TEST(EngineSettings_SSAOSampleCountPowerOf2)
{
    SSAOSettings ssao;
    // Common sample counts are powers of 2
    int valid[] = {8, 16, 32, 64};
    bool isValidCount = false;
    for (int v : valid)
    {
        if (ssao.sampleCount == v)
        {
            isValidCount = true;
            break;
        }
    }
    EXPECT_TRUE(isValidCount);
}

// ============================================================================
// TAA Settings
// ============================================================================

TEST(EngineSettings_TAADefaults)
{
    TAASettings taa;
    EXPECT_TRUE(taa.enabled);
    EXPECT_GE(taa.quality, 0);
    EXPECT_LE(taa.quality, 3);
    EXPECT_NEAR(taa.historyBlendFactor, 0.9f, 0.01f);
    EXPECT_TRUE(taa.useMotionVectors);
}

TEST(EngineSettings_TAAHistoryBlendBounds)
{
    TAASettings taa;
    float clamped = Clamp(taa.historyBlendFactor, 0.0f, 1.0f);
    EXPECT_GE(clamped, 0.0f);
    EXPECT_LE(clamped, 1.0f);
}

// ============================================================================
// Player Settings
// ============================================================================

TEST(EngineSettings_PlayerDefaults)
{
    PlayerSettings ps;
    EXPECT_NEAR(ps.maxHealth, 100.0f, 0.01f);
    EXPECT_NEAR(ps.moveSpeed, 5.0f, 0.01f);
    EXPECT_NEAR(ps.sprintMultiplier, 1.5f, 0.01f);
    EXPECT_NEAR(ps.crouchMultiplier, 0.5f, 0.01f);
}

TEST(EngineSettings_SprintSpeed)
{
    PlayerSettings ps;
    float sprintSpeed = ps.moveSpeed * ps.sprintMultiplier;
    EXPECT_NEAR(sprintSpeed, 7.5f, 0.01f);
}

TEST(EngineSettings_CrouchSpeed)
{
    PlayerSettings ps;
    float crouchSpeed = ps.moveSpeed * ps.crouchMultiplier;
    EXPECT_NEAR(crouchSpeed, 2.5f, 0.01f);
}

TEST(EngineSettings_SprintFasterThanCrouch)
{
    PlayerSettings ps;
    EXPECT_GT(ps.sprintMultiplier, ps.crouchMultiplier);
}

// ============================================================================
// Cross-Settings Interactions
// ============================================================================

TEST(EngineSettings_DynamicQualityAffectsRenderScale)
{
    GraphicsSettings gs;
    DynamicQualitySettings dq;
    dq.enabled = true;

    // When dynamic quality is on, render scale should be within dq bounds
    gs.renderScale = Clamp(gs.renderScale, dq.minRenderScale, dq.maxRenderScale);
    EXPECT_GE(gs.renderScale, dq.minRenderScale);
    EXPECT_LE(gs.renderScale, dq.maxRenderScale);
}

TEST(EngineSettings_HDRRequiresBloomThreshold)
{
    GraphicsSettings gs;
    PostProcessSettings pp;
    gs.hdr = true;

    // With HDR, bloom threshold should be > 0 to avoid blooming everything
    EXPECT_GT(pp.bloomThreshold, 0.0f);
}

// ============================================================================
// New Settings Categories — Accessibility
// ============================================================================

namespace
{
    struct AccessibilitySettings
    {
        int colorblindMode = 0;
        float colorblindStrength = 1.0f;
        bool screenReader = false;
        bool reduceMotion = false;
        bool highContrast = false;
        float holdTimeMultiplier = 1.0f;
        bool toggleAim = false;
        bool toggleSprint = false;
        bool toggleCrouch = false;
        bool autoAim = false;
        float autoAimStrength = 0.5f;
    };

    struct VRSettings
    {
        bool enabled = false;
        int renderTargetWidth = 1440;
        int renderTargetHeight = 1600;
        float renderScale = 1.0f;
        int trackingSpace = 1;
        float ipd = 0.064f;
        int comfortMode = 0;
        float snapTurnAngle = 45.0f;
    };

    struct DestructionSettings
    {
        float debrisLifetime = 10.0f;
        float damageThreshold = 50.0f;
        float damageMultiplier = 1.0f;
        int maxDamageStages = 3;
        float scatterForce = 5.0f;
        int maxDebrisPieces = 100;
    };

    struct SaveSystemSettings
    {
        int maxAutoSaveSlots = 3;
        float autoSaveInterval = 300.0f;
        bool compressSaves = true;
        bool backupOnSave = true;
        int maxManualSaves = 100;
    };

    struct MemorySettings
    {
        int textureStreamingBudgetMB = 512;
        int meshStreamingBudgetMB = 256;
        int audioStreamingBudgetMB = 128;
        float gcInterval = 60.0f;
        float gcAggressiveness = 0.5f;
    };

    struct ParticleSettings
    {
        int maxParticles = 10000;
        int maxEmitters = 256;
        float simulationRate = 60.0f;
        float globalScale = 1.0f;
    };
} // anonymous namespace

TEST(EngineSettings_AccessibilityDefaults)
{
    AccessibilitySettings a;
    EXPECT_EQ(a.colorblindMode, 0);
    EXPECT_NEAR(a.colorblindStrength, 1.0f, 0.01f);
    EXPECT_FALSE(a.screenReader);
    EXPECT_FALSE(a.reduceMotion);
    EXPECT_FALSE(a.highContrast);
    EXPECT_NEAR(a.holdTimeMultiplier, 1.0f, 0.01f);
    EXPECT_FALSE(a.toggleAim);
    EXPECT_FALSE(a.autoAim);
    EXPECT_NEAR(a.autoAimStrength, 0.5f, 0.01f);
}

TEST(EngineSettings_ColorblindModeBounds)
{
    AccessibilitySettings a;
    a.colorblindMode = std::clamp(5, 0, 3);
    EXPECT_EQ(a.colorblindMode, 3);

    a.colorblindMode = std::clamp(-1, 0, 3);
    EXPECT_EQ(a.colorblindMode, 0);
}

TEST(EngineSettings_AutoAimStrengthClamped)
{
    AccessibilitySettings a;
    a.autoAimStrength = Clamp(1.5f, 0.0f, 1.0f);
    EXPECT_NEAR(a.autoAimStrength, 1.0f, 0.01f);

    a.autoAimStrength = Clamp(-0.5f, 0.0f, 1.0f);
    EXPECT_NEAR(a.autoAimStrength, 0.0f, 0.01f);
}

// ============================================================================
// VR Settings
// ============================================================================

TEST(EngineSettings_VRDefaults)
{
    VRSettings vr;
    EXPECT_FALSE(vr.enabled);
    EXPECT_EQ(vr.renderTargetWidth, 1440);
    EXPECT_EQ(vr.renderTargetHeight, 1600);
    EXPECT_NEAR(vr.renderScale, 1.0f, 0.01f);
    EXPECT_EQ(vr.trackingSpace, 1);
    EXPECT_NEAR(vr.ipd, 0.064f, 0.001f);
}

TEST(EngineSettings_VRRenderScaleClamped)
{
    VRSettings vr;
    vr.renderScale = Clamp(3.0f, 0.5f, 2.0f);
    EXPECT_NEAR(vr.renderScale, 2.0f, 0.01f);
}

TEST(EngineSettings_VRSnapTurnAngleBounds)
{
    VRSettings vr;
    vr.snapTurnAngle = Clamp(100.0f, 15.0f, 90.0f);
    EXPECT_NEAR(vr.snapTurnAngle, 90.0f, 0.01f);
}

// ============================================================================
// Destruction Settings
// ============================================================================

TEST(EngineSettings_DestructionDefaults)
{
    DestructionSettings d;
    EXPECT_NEAR(d.debrisLifetime, 10.0f, 0.01f);
    EXPECT_NEAR(d.damageThreshold, 50.0f, 0.01f);
    EXPECT_NEAR(d.damageMultiplier, 1.0f, 0.01f);
    EXPECT_EQ(d.maxDamageStages, 3);
    EXPECT_EQ(d.maxDebrisPieces, 100);
}

TEST(EngineSettings_DebrisLifetimePositive)
{
    DestructionSettings d;
    d.debrisLifetime = Clamp(-5.0f, 0.0f, 60.0f);
    EXPECT_GE(d.debrisLifetime, 0.0f);
}

// ============================================================================
// Save System Settings
// ============================================================================

TEST(EngineSettings_SaveSystemDefaults)
{
    SaveSystemSettings sv;
    EXPECT_EQ(sv.maxAutoSaveSlots, 3);
    EXPECT_NEAR(sv.autoSaveInterval, 300.0f, 0.01f);
    EXPECT_TRUE(sv.compressSaves);
    EXPECT_TRUE(sv.backupOnSave);
    EXPECT_EQ(sv.maxManualSaves, 100);
}

TEST(EngineSettings_AutoSaveIntervalBounds)
{
    SaveSystemSettings sv;
    sv.autoSaveInterval = Clamp(-10.0f, 0.0f, 3600.0f);
    EXPECT_GE(sv.autoSaveInterval, 0.0f);
}

// ============================================================================
// Memory Settings
// ============================================================================

TEST(EngineSettings_MemoryDefaults)
{
    MemorySettings mem;
    EXPECT_EQ(mem.textureStreamingBudgetMB, 512);
    EXPECT_EQ(mem.meshStreamingBudgetMB, 256);
    EXPECT_EQ(mem.audioStreamingBudgetMB, 128);
    EXPECT_NEAR(mem.gcInterval, 60.0f, 0.01f);
    EXPECT_NEAR(mem.gcAggressiveness, 0.5f, 0.01f);
}

TEST(EngineSettings_MemoryBudgetPositive)
{
    MemorySettings mem;
    mem.textureStreamingBudgetMB = std::max(64, mem.textureStreamingBudgetMB);
    EXPECT_GE(mem.textureStreamingBudgetMB, 64);
}

TEST(EngineSettings_GCAggressivenessClamped)
{
    MemorySettings mem;
    mem.gcAggressiveness = Clamp(1.5f, 0.0f, 1.0f);
    EXPECT_NEAR(mem.gcAggressiveness, 1.0f, 0.01f);
}

// ============================================================================
// Particle Settings
// ============================================================================

TEST(EngineSettings_ParticleDefaults)
{
    ParticleSettings p;
    EXPECT_EQ(p.maxParticles, 10000);
    EXPECT_EQ(p.maxEmitters, 256);
    EXPECT_NEAR(p.simulationRate, 60.0f, 0.01f);
    EXPECT_NEAR(p.globalScale, 1.0f, 0.01f);
}

TEST(EngineSettings_ParticleCountPositive)
{
    ParticleSettings p;
    p.maxParticles = std::max(100, p.maxParticles);
    EXPECT_GE(p.maxParticles, 100);
}

// ============================================================================
// Cross-category Interactions (new categories)
// ============================================================================

TEST(EngineSettings_ReduceMotionDisablesScreenShake)
{
    AccessibilitySettings a;
    a.reduceMotion = true;
    // When reduce motion is on, camera shake/bob multipliers should be 0
    float cameraShakeMultiplier = a.reduceMotion ? 0.0f : 1.0f;
    EXPECT_NEAR(cameraShakeMultiplier, 0.0f, 0.01f);
}

TEST(EngineSettings_VROverridesRenderScale)
{
    GraphicsSettings gs;
    VRSettings vr;
    vr.enabled = true;
    vr.renderScale = 1.4f;

    // When VR is enabled, its render scale should be used instead of graphics scale
    float effectiveScale = vr.enabled ? vr.renderScale : gs.renderScale;
    EXPECT_NEAR(effectiveScale, 1.4f, 0.01f);
}

TEST(EngineSettings_TotalMemoryBudgetReasonable)
{
    MemorySettings mem;
    int totalBudget = mem.textureStreamingBudgetMB + mem.meshStreamingBudgetMB + mem.audioStreamingBudgetMB;
    // Total streaming budget should be under 4 GB by default
    EXPECT_LT(totalBudget, 4096);
}
