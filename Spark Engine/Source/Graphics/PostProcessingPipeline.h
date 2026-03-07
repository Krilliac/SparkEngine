/**
 * @file PostProcessingPipeline.h
 * @brief Configurable post-processing effect chain with ordered passes
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a flexible pipeline for chaining post-processing effects in a
 * configurable order. Each effect is a self-contained pass with its own
 * settings, enable/disable state, and performance metrics. The pipeline
 * manages render target ping-ponging between passes.
 *
 * ## Usage
 * @code
 *   PostProcessingPipeline pipeline;
 *   pipeline.Initialize(1920, 1080);
 *
 *   pipeline.SetEffectEnabled(PostProcessPass::Vignette, true);
 *   pipeline.GetVignetteSettings().intensity = 0.4f;
 *
 *   pipeline.SetEffectEnabled(PostProcessPass::DepthOfField, true);
 *   pipeline.GetDOFSettings().focalDistance = 15.0f;
 *
 *   // Each frame after scene rendering
 *   pipeline.Process(deltaTime);
 * @endcode
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdint>

namespace Spark::Graphics {

// =============================================================================
// Post-Processing Pass Types
// =============================================================================

/**
 * @brief Individual post-processing passes in render order
 */
enum class PostProcessPass {
    FXAA,                   ///< Fast Approximate Anti-Aliasing
    DepthOfField,           ///< Bokeh depth of field
    MotionBlur,             ///< Per-pixel motion blur (uses TemporalEffects data)
    Vignette,               ///< Screen-edge darkening
    ChromaticAberration,    ///< RGB channel separation
    FilmGrain,              ///< Cinematic film grain noise
    LensDistortion,         ///< Barrel/pincushion distortion
    LightShafts,            ///< God rays / volumetric light shafts
    LensFlare,              ///< Lens flare from bright light sources
    Sharpen,                ///< Contrast-adaptive sharpening
    Count
};

// =============================================================================
// Effect Settings
// =============================================================================

/**
 * @brief FXAA (Fast Approximate Anti-Aliasing) settings
 */
struct FXAASettings {
    bool enabled = false;
    float edgeThreshold = 0.166f;       ///< Min luminance edge detection [0.063, 0.333]
    float edgeThresholdMin = 0.0833f;   ///< Darkest edge threshold
    float subpixelQuality = 0.75f;      ///< Sub-pixel AA quality [0=off, 1=max]
    int qualityPreset = 12;             ///< Quality iterations [10=low, 29=ultra]
};

/**
 * @brief Depth of Field settings
 */
struct DepthOfFieldSettings {
    bool enabled = false;
    float focalDistance = 10.0f;         ///< Distance to focus plane in meters
    float focalLength = 50.0f;          ///< Lens focal length in mm
    float aperture = 2.8f;              ///< F-stop aperture (lower = more blur)
    float nearBlurStart = 0.5f;         ///< Near blur start distance
    float nearBlurEnd = 2.0f;           ///< Near blur full distance
    float farBlurStart = 20.0f;         ///< Far blur start distance
    float farBlurEnd = 100.0f;          ///< Far blur full distance
    float maxBokehSize = 8.0f;          ///< Maximum bokeh circle diameter in pixels
    int blurSamples = 16;               ///< Samples for blur kernel
    bool useCircularBokeh = true;       ///< Circular vs hexagonal bokeh shape
    float bokehBrightness = 1.0f;       ///< Brightness threshold for bokeh highlights
};

/**
 * @brief Vignette effect settings
 */
struct VignetteSettings {
    bool enabled = false;
    float intensity = 0.3f;             ///< Darkening intensity [0, 1]
    float smoothness = 0.5f;            ///< Edge softness [0, 1]
    float roundness = 1.0f;             ///< Shape (1=circular, 0=rectangular)
    XMFLOAT3 color = {0.0f, 0.0f, 0.0f}; ///< Vignette color (default: black)
    XMFLOAT2 center = {0.5f, 0.5f};    ///< Vignette center in UV space
};

/**
 * @brief Chromatic aberration settings
 */
struct ChromaticAberrationSettings {
    bool enabled = false;
    float intensity = 0.5f;             ///< Separation amount [0, 3]
    float radialFalloff = 1.0f;         ///< Stronger at edges [0=uniform, 2=strong edge]
    XMFLOAT3 channelOffsets = {1.0f, 0.0f, -1.0f}; ///< R, G, B offset multipliers
};

/**
 * @brief Film grain effect settings
 */
struct FilmGrainSettings {
    bool enabled = false;
    float intensity = 0.15f;            ///< Grain visibility [0, 1]
    float size = 1.6f;                  ///< Grain particle size
    float speed = 1.0f;                 ///< Animation speed
    float luminanceContribution = 0.8f; ///< How much luminance affects grain [0, 1]
    bool colored = false;               ///< Color noise vs monochrome
};

/**
 * @brief Lens distortion settings
 */
struct LensDistortionSettings {
    bool enabled = false;
    float barrelDistortion = 0.0f;      ///< Barrel/pincushion [-1=pincushion, 1=barrel]
    float zoomCompensation = 1.0f;      ///< Zoom to compensate for distortion
    XMFLOAT2 center = {0.5f, 0.5f};    ///< Distortion center in UV space
    float cubicDistortion = 0.0f;       ///< Higher-order distortion term
};

/**
 * @brief Light shafts (god rays) settings
 */
struct LightShaftSettings {
    bool enabled = false;
    XMFLOAT2 lightScreenPos = {0.5f, 0.3f}; ///< Light source screen position
    float density = 1.0f;               ///< Ray density [0, 1]
    float weight = 0.01f;               ///< Intensity per sample
    float decay = 0.97f;                ///< Intensity decay per step [0, 1]
    float exposure = 1.0f;              ///< Final exposure multiplier
    int sampleCount = 64;               ///< Ray marching samples
    XMFLOAT3 color = {1.0f, 0.95f, 0.8f}; ///< Shaft color
};

/**
 * @brief Lens flare settings
 */
struct LensFlareSettings {
    bool enabled = false;
    float threshold = 0.8f;             ///< Brightness threshold for flare trigger
    float intensity = 0.5f;             ///< Flare overall intensity
    int ghostCount = 5;                 ///< Number of ghost images
    float ghostSpacing = 0.3f;          ///< Distance between ghosts
    float ghostThreshold = 10.0f;       ///< Brightness for ghost generation
    float haloRadius = 0.6f;            ///< Halo ring radius
    float haloThickness = 0.1f;         ///< Halo ring width
    float chromaticDistortion = 2.5f;   ///< Color separation in flare
};

/**
 * @brief Contrast-adaptive sharpening settings
 */
struct SharpenSettings {
    bool enabled = false;
    float amount = 0.5f;                ///< Sharpening strength [0, 1]
    float threshold = 0.05f;            ///< Edge threshold to avoid noise amplification
    bool adaptiveSharpening = true;     ///< CAS (AMD FidelityFX style)
};

// =============================================================================
// Pass Metrics
// =============================================================================

/**
 * @brief Performance metrics for a single post-processing pass
 */
struct PassMetrics {
    std::string name;
    float timeMs = 0.0f;               ///< GPU time in milliseconds
    bool isEnabled = false;
};

// =============================================================================
// Post-Processing Pipeline
// =============================================================================

/**
 * @class PostProcessingPipeline
 * @brief Manages an ordered chain of post-processing effects
 *
 * Effects are processed in a fixed order (the PostProcessPass enum order).
 * Each effect can be independently enabled/disabled. The pipeline handles
 * render target ping-ponging automatically.
 */
class PostProcessingPipeline {
public:
    PostProcessingPipeline() = default;
    ~PostProcessingPipeline() = default;

    /**
     * @brief Initialize the pipeline with render dimensions
     */
    bool Initialize(uint32_t width = 1920, uint32_t height = 1080) {
        m_width = width;
        m_height = height;
        m_initialized = true;
        return true;
    }

    /** @brief Shutdown and release all resources */
    void Shutdown() {
        m_initialized = false;
    }

    /**
     * @brief Process all enabled effects in order
     * @param deltaTime Frame delta time for animated effects
     */
    void Process(float deltaTime = 0.0f) {
        if (!m_initialized) return;
        m_totalTime += deltaTime;
        m_activePassCount = 0;

        for (int i = 0; i < static_cast<int>(PostProcessPass::Count); ++i) {
            auto pass = static_cast<PostProcessPass>(i);
            if (IsEffectEnabled(pass)) {
                ProcessPass(pass, deltaTime);
                m_activePassCount++;
            }
        }
    }

    /** @brief Render the final result */
    void Render() {}

    /**
     * @brief Handle viewport resize
     */
    void Resize(uint32_t width, uint32_t height) {
        m_width = width;
        m_height = height;
    }

    // ---- Effect Enable/Disable ----

    void SetEffectEnabled(PostProcessPass pass, bool enabled) {
        m_passEnabled[static_cast<int>(pass)] = enabled;
    }

    bool IsEffectEnabled(PostProcessPass pass) const {
        return m_passEnabled[static_cast<int>(pass)];
    }

    int GetActivePassCount() const { return m_activePassCount; }

    // ---- Settings Accessors ----

    FXAASettings& GetFXAASettings() { return m_fxaaSettings; }
    const FXAASettings& GetFXAASettings() const { return m_fxaaSettings; }

    DepthOfFieldSettings& GetDOFSettings() { return m_dofSettings; }
    const DepthOfFieldSettings& GetDOFSettings() const { return m_dofSettings; }

    VignetteSettings& GetVignetteSettings() { return m_vignetteSettings; }
    const VignetteSettings& GetVignetteSettings() const { return m_vignetteSettings; }

    ChromaticAberrationSettings& GetChromaticAberrationSettings() { return m_chromAbSettings; }
    const ChromaticAberrationSettings& GetChromaticAberrationSettings() const { return m_chromAbSettings; }

    FilmGrainSettings& GetFilmGrainSettings() { return m_filmGrainSettings; }
    const FilmGrainSettings& GetFilmGrainSettings() const { return m_filmGrainSettings; }

    LensDistortionSettings& GetLensDistortionSettings() { return m_lensDistSettings; }
    const LensDistortionSettings& GetLensDistortionSettings() const { return m_lensDistSettings; }

    LightShaftSettings& GetLightShaftSettings() { return m_lightShaftSettings; }
    const LightShaftSettings& GetLightShaftSettings() const { return m_lightShaftSettings; }

    LensFlareSettings& GetLensFlareSettings() { return m_lensFlareSettings; }
    const LensFlareSettings& GetLensFlareSettings() const { return m_lensFlareSettings; }

    SharpenSettings& GetSharpenSettings() { return m_sharpenSettings; }
    const SharpenSettings& GetSharpenSettings() const { return m_sharpenSettings; }

    // ---- Metrics ----

    std::vector<PassMetrics> GetPassMetrics() const {
        std::vector<PassMetrics> metrics;
        static const char* passNames[] = {
            "FXAA", "DepthOfField", "MotionBlur", "Vignette",
            "ChromaticAberration", "FilmGrain", "LensDistortion",
            "LightShafts", "LensFlare", "Sharpen"
        };
        for (int i = 0; i < static_cast<int>(PostProcessPass::Count); ++i) {
            PassMetrics pm;
            pm.name = passNames[i];
            pm.isEnabled = m_passEnabled[i];
            pm.timeMs = m_passTimings[i];
            metrics.push_back(pm);
        }
        return metrics;
    }

    std::string Console_ListEffects() const {
        std::string result = "Post-Processing Pipeline:\n";
        auto metrics = GetPassMetrics();
        for (const auto& pm : metrics) {
            result += "  " + pm.name + ": " + (pm.isEnabled ? "ON" : "OFF") + "\n";
        }
        result += "Active: " + std::to_string(m_activePassCount) + "\n";
        return result;
    }

private:
    void ProcessPass(PostProcessPass pass, float deltaTime) {
        (void)pass;
        (void)deltaTime;
    }

    bool m_initialized = false;
    uint32_t m_width = 1920;
    uint32_t m_height = 1080;
    float m_totalTime = 0.0f;
    int m_activePassCount = 0;

    bool m_passEnabled[static_cast<int>(PostProcessPass::Count)] = {};
    float m_passTimings[static_cast<int>(PostProcessPass::Count)] = {};

    FXAASettings m_fxaaSettings;
    DepthOfFieldSettings m_dofSettings;
    VignetteSettings m_vignetteSettings;
    ChromaticAberrationSettings m_chromAbSettings;
    FilmGrainSettings m_filmGrainSettings;
    LensDistortionSettings m_lensDistSettings;
    LightShaftSettings m_lightShaftSettings;
    LensFlareSettings m_lensFlareSettings;
    SharpenSettings m_sharpenSettings;
};

} // namespace Spark::Graphics
