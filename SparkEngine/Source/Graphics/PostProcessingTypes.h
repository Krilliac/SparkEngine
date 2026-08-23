/**
 * @file PostProcessingTypes.h
 * @brief Type definitions, enums, and settings structs for the post-processing pipeline
 * @author Spark Engine Team
 * @date 2025
 *
 * Contains all public data types used by PostProcessingPipeline: pass enums,
 * per-effect settings structs, and performance metric types. Separated from
 * the pipeline class to allow lightweight inclusion by code that only needs
 * the type definitions (e.g., editor panels, serialization).
 */

#pragma once

#include "../Core/Platform.h"
#include "GTAOEffect.h"   // Provides GTAOSettings for the GTAO post-process pass
#include "SSAOTemporal.h" // Provides SSAOTemporalSettings for the SSAOTemporal post-process pass

// DirectXMath types provided by Platform.h (stubs on Linux).

#include <string>
#include <cstdint>

namespace Spark::Graphics
{

    // =============================================================================
    // Post-Processing Pass Types
    // =============================================================================

    /**
 * @brief Individual post-processing passes in render order
 *
 * GTAO is slotted first so ambient occlusion modulates scene lighting at
 * HDR resolution, before Bloom extracts highlights. SSAOTemporal immediately
 * follows GTAO and applies a variance-clipped spatial/temporal denoise to
 * the AO output before downstream passes (Bloom, exposure, tonemap...) see
 * the AO-darkened scene.
 */
    enum class PostProcessPass
    {
        GTAO,                ///< Ground Truth Ambient Occlusion (horizon-based screen-space AO)
        SSAOTemporal,        ///< Variance-clipped denoiser for the AO term (runs immediately after GTAO)
        Bloom,               ///< HDR bloom (threshold extract + blur + composite)
        AutoExposure,        ///< Luminance-based automatic exposure adaptation
        Tonemapping,         ///< HDR-to-LDR tonemapping (ACES, Filmic, Neutral)
        ColorGrading,        ///< Lift/Gamma/Gain color correction + curves
        FXAA,                ///< Fast Approximate Anti-Aliasing
        DepthOfField,        ///< Bokeh depth of field
        MotionBlur,          ///< Per-pixel motion blur (uses TemporalEffects data)
        Vignette,            ///< Screen-edge darkening
        ChromaticAberration, ///< RGB channel separation
        FilmGrain,           ///< Cinematic film grain noise
        LensDistortion,      ///< Barrel/pincushion distortion
        LightShafts,         ///< God rays / volumetric light shafts
        LensFlare,           ///< Lens flare from bright light sources
        Sharpen,             ///< Contrast-adaptive sharpening
        Count
    };

    /**
     * @brief Deterministic ping-pong routing for an ordered post-process chain.
     *
     * Pass zero writes target zero, pass one writes target one, and so on.
     * A chain with no enabled passes preserves the caller's scene input rather
     * than exposing an uninitialized ping-pong target.
     */
    struct PostProcessTargetRouting
    {
        static constexpr int InputTarget = -1;

        static constexpr int DestinationForPass(uint32_t passOrdinal) { return static_cast<int>(passOrdinal & 1u); }

        static constexpr int FinalTargetForPassCount(uint32_t passCount)
        {
            return passCount == 0 ? InputTarget : DestinationForPass(passCount - 1);
        }
    };

    // =============================================================================
    // Effect Settings
    // =============================================================================

    /**
 * @brief FXAA (Fast Approximate Anti-Aliasing) settings
 */
    struct FXAASettings
    {
        bool enabled = false;
        float edgeThreshold = 0.166f;     ///< Min luminance edge detection [0.063, 0.333]
        float edgeThresholdMin = 0.0833f; ///< Darkest edge threshold
        float subpixelQuality = 0.75f;    ///< Sub-pixel AA quality [0=off, 1=max]
        int qualityPreset = 12;           ///< Quality iterations [10=low, 29=ultra]
    };

    /**
 * @brief Depth of Field settings
 */
    struct DepthOfFieldSettings
    {
        bool enabled = false;
        float focalDistance = 10.0f;  ///< Distance to focus plane in meters
        float focalLength = 50.0f;    ///< Lens focal length in mm
        float aperture = 2.8f;        ///< F-stop aperture (lower = more blur)
        float nearBlurStart = 0.5f;   ///< Near blur start distance
        float nearBlurEnd = 2.0f;     ///< Near blur full distance
        float farBlurStart = 20.0f;   ///< Far blur start distance
        float farBlurEnd = 100.0f;    ///< Far blur full distance
        float maxBokehSize = 8.0f;    ///< Maximum bokeh circle diameter in pixels
        int blurSamples = 16;         ///< Samples for blur kernel
        bool useCircularBokeh = true; ///< Circular vs hexagonal bokeh shape
        float bokehBrightness = 1.0f; ///< Brightness threshold for bokeh highlights
    };

    /**
 * @brief Vignette effect settings
 */
    struct VignetteSettings
    {
        bool enabled = false;
        float intensity = 0.3f;              ///< Darkening intensity [0, 1]
        float smoothness = 0.5f;             ///< Edge softness [0, 1]
        float roundness = 1.0f;              ///< Shape (1=circular, 0=rectangular)
        XMFLOAT3 color = {0.0f, 0.0f, 0.0f}; ///< Vignette color (default: black)
        XMFLOAT2 center = {0.5f, 0.5f};      ///< Vignette center in UV space
    };

    /**
 * @brief Chromatic aberration settings
 */
    struct ChromaticAberrationSettings
    {
        bool enabled = false;
        float intensity = 0.5f;                        ///< Separation amount [0, 3]
        float radialFalloff = 1.0f;                    ///< Stronger at edges [0=uniform, 2=strong edge]
        XMFLOAT3 channelOffsets = {1.0f, 0.0f, -1.0f}; ///< R, G, B offset multipliers
    };

    /**
 * @brief Film grain effect settings
 */
    struct FilmGrainSettings
    {
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
    struct LensDistortionSettings
    {
        bool enabled = false;
        float barrelDistortion = 0.0f;  ///< Barrel/pincushion [-1=pincushion, 1=barrel]
        float zoomCompensation = 1.0f;  ///< Zoom to compensate for distortion
        XMFLOAT2 center = {0.5f, 0.5f}; ///< Distortion center in UV space
        float cubicDistortion = 0.0f;   ///< Higher-order distortion term
    };

    /**
 * @brief Light shafts (god rays) settings
 */
    struct LightShaftSettings
    {
        bool enabled = false;
        XMFLOAT2 lightScreenPos = {0.5f, 0.3f}; ///< Light source screen position
        float density = 1.0f;                   ///< Ray density [0, 1]
        float weight = 0.01f;                   ///< Intensity per sample
        float decay = 0.97f;                    ///< Intensity decay per step [0, 1]
        float exposure = 1.0f;                  ///< Final exposure multiplier
        int sampleCount = 64;                   ///< Ray marching samples
        XMFLOAT3 color = {1.0f, 0.95f, 0.8f};   ///< Shaft color
    };

    /**
 * @brief Lens flare settings
 */
    struct LensFlareSettings
    {
        bool enabled = false;
        float threshold = 0.8f;           ///< Brightness threshold for flare trigger
        float intensity = 0.5f;           ///< Flare overall intensity
        int ghostCount = 5;               ///< Number of ghost images
        float ghostSpacing = 0.3f;        ///< Distance between ghosts
        float ghostThreshold = 10.0f;     ///< Brightness for ghost generation
        float haloRadius = 0.6f;          ///< Halo ring radius
        float haloThickness = 0.1f;       ///< Halo ring width
        float chromaticDistortion = 2.5f; ///< Color separation in flare
    };

    /**
 * @brief Contrast-adaptive sharpening settings
 */
    struct SharpenSettings
    {
        bool enabled = false;
        float amount = 0.5f;            ///< Sharpening strength [0, 1]
        float threshold = 0.05f;        ///< Edge threshold to avoid noise amplification
        bool adaptiveSharpening = true; ///< CAS (AMD FidelityFX style)
    };

    // =============================================================================
    // HDR / Tonemapping / Color Grading Settings
    // =============================================================================

    /**
     * @brief Bloom effect settings (threshold-based HDR extraction + blur + composite)
     */
    struct BloomSettings
    {
        bool enabled = false;
        float threshold = 1.0f;     ///< Luminance threshold for bright pixel extraction
        float softThreshold = 0.5f; ///< Soft knee around threshold [0, 1]
        float intensity = 0.8f;     ///< Final bloom composite intensity
        float radius = 4.0f;        ///< Blur radius in texels
        int iterations = 5;         ///< Number of downscale/blur passes [1, 8]
        float scatter = 0.7f;       ///< Energy scatter between blur passes [0, 1]
        bool highQuality = true;    ///< Use 13-tap dual filter vs 5-tap
    };

    /**
     * @brief Tonemapping operator selection
     */
    enum class TonemapOperator
    {
        ACES,     ///< Academy Color Encoding System (filmic, industry standard)
        Filmic,   ///< Uncharted 2 filmic curve (John Hable)
        Neutral,  ///< Minimal color shift, balanced contrast
        Reinhard, ///< Simple Reinhard (luminance-based)
        Count
    };

    /**
     * @brief Tonemapping settings (HDR to LDR conversion)
     */
    struct TonemappingSettings
    {
        bool enabled = false;
        TonemapOperator op = TonemapOperator::ACES; ///< Active tonemapping operator
        float exposure = 1.0f;                      ///< Pre-tonemap exposure multiplier
        float whitePoint = 11.2f;                   ///< White point for Filmic/Reinhard operators
        float contrast = 1.0f;                      ///< Post-tonemap contrast adjustment [0.5, 2.0]
        float saturation = 1.0f;                    ///< Post-tonemap saturation [0, 2]
    };

    /**
     * @brief Auto-exposure / eye adaptation settings
     */
    struct AutoExposureSettings
    {
        bool enabled = false;
        float minExposure = 0.25f;     ///< Minimum EV (prevents over-darkening)
        float maxExposure = 4.0f;      ///< Maximum EV (prevents over-brightening)
        float adaptSpeedUp = 2.0f;     ///< Adaptation speed bright-to-dark (EV/s)
        float adaptSpeedDown = 1.0f;   ///< Adaptation speed dark-to-bright (EV/s)
        float targetLuminance = 0.18f; ///< Middle-grey target (key value)
        float histogramMin = -8.0f;    ///< Log2 luminance histogram lower bound
        float histogramMax = 4.0f;     ///< Log2 luminance histogram upper bound
        float compensationEV = 0.0f;   ///< Manual EV compensation offset
    };

    /**
     * @brief Color grading settings (Lift/Gamma/Gain + curves)
     */
    struct ColorGradingSettings
    {
        bool enabled = false;

        /// Lift (shadows) — added to the darkest values
        XMFLOAT3 lift = {0.0f, 0.0f, 0.0f};

        /// Gamma (midtones) — power curve applied to midrange
        XMFLOAT3 gamma = {1.0f, 1.0f, 1.0f};

        /// Gain (highlights) — multiplied into the brightest values
        XMFLOAT3 gain = {1.0f, 1.0f, 1.0f};

        float temperature = 0.0f; ///< White balance shift [-1=cool, 1=warm]
        float tint = 0.0f;        ///< Green-magenta shift [-1=green, 1=magenta]
        float hueShift = 0.0f;    ///< Global hue rotation in degrees [-180, 180]
        float saturation = 1.0f;  ///< Global saturation [0=mono, 2=oversaturated]
        float brightness = 0.0f;  ///< Global brightness offset [-1, 1]
        float contrast = 1.0f;    ///< Global contrast [0.5, 2.0]
    };

    // =============================================================================
    // Pass Metrics
    // =============================================================================

    /**
 * @brief Performance metrics for a single post-processing pass
 */
    struct PassMetrics
    {
        std::string name;
        float timeMs = 0.0f; ///< GPU time in milliseconds
        bool isEnabled = false;
    };

} // namespace Spark::Graphics
