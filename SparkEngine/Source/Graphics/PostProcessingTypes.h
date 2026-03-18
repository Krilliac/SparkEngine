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

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include <string>
#include <cstdint>

namespace Spark::Graphics
{

    // =============================================================================
    // Post-Processing Pass Types
    // =============================================================================

    /**
 * @brief Individual post-processing passes in render order
 */
    enum class PostProcessPass
    {
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
