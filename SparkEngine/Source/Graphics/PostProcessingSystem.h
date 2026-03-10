/**
 * @file PostProcessingSystem.h
 * @brief Post-processing system for Spark Engine (Bloom, Tone Mapping, Color Grading, FXAA,
 *        Vignette, Chromatic Aberration, Exposure Adaptation)
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

// Mutual exclusion: PostProcessing.h defines an older version of the same class
#ifdef SPARK_POST_PROCESSING_H
#error "Do not include both PostProcessing.h and PostProcessingSystem.h in the same translation unit"
#endif
#define SPARK_POST_PROCESSING_SYSTEM_H

#include "../Core/Platform.h"

#include "Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <memory>

using Microsoft::WRL::ComPtr;

/**
 * @brief Full-featured post-processing system.
 *
 * Pipeline order:
 *   Scene HDR -> Exposure Adaptation -> Bloom -> Tone Mapping -> Color Grading
 *              -> Vignette -> Chromatic Aberration -> FXAA -> Final Output
 */
class PostProcessingSystem
{
  public:
    PostProcessingSystem();
    ~PostProcessingSystem();

    /**
     * @brief Initialize the post-processing system
     */
    HRESULT Initialize(ID3D11Device* device, ID3D11DeviceContext* context);

    /**
     * @brief Shutdown the post-processing system
     */
    void Shutdown();

    /**
     * @brief Update the system (called once per frame, runs the full pipeline)
     */
    void Update(float deltaTime);

    /**
     * @brief Execute the full post-processing pipeline on the current back buffer.
     *
     * This is the primary entry point called each frame after scene rendering.
     * Captures the back buffer, runs all enabled effects in order, and writes
     * the final result back to the back buffer.
     */
    void Execute(float deltaTime);

    // =========================================================================
    // Individual render passes (called internally by Execute)
    // =========================================================================

    /**
     * @brief Render the bloom effect.
     *
     * Bright-pass extraction, multi-pass Gaussian blur (horizontal + vertical)
     * at progressively downsampled mip levels, then upsample and composite.
     */
    void RenderBloom();

    /**
     * @brief Render tone mapping (ACES filmic curve by default).
     */
    void RenderToneMapping();

    /**
     * @brief Render FXAA anti-aliasing pass.
     *
     * Luminance-based edge detection with 12-iteration directional blur.
     */
    void RenderFXAA();

    /**
     * @brief Render auto-exposure adaptation.
     *
     * Computes average scene luminance via successive downsampling and
     * smoothly adapts the exposure value over time.
     */
    void RenderExposureAdaptation(float deltaTime);

    /**
     * @brief Render vignette effect.
     *
     * Darkens the screen edges based on distance from center.
     */
    void RenderVignette();

    /**
     * @brief Render chromatic aberration effect.
     *
     * Offsets R, G, B channels based on UV distance from center.
     */
    void RenderChromaticAberration();

    // =========================================================================
    // Console integration
    // =========================================================================

    /**
     * @brief Console integration - Set exposure
     */
    void Console_SetExposure(float exposure);

    /**
     * @brief Console integration - List effects
     */
    std::string Console_ListEffects() const;

    /**
     * @brief Console integration - Toggle an effect by name
     */
    void Console_ToggleEffect(const std::string& effectName, bool enabled);

    /**
     * @brief Console integration - Set vignette intensity
     */
    void Console_SetVignetteIntensity(float intensity);

    /**
     * @brief Console integration - Set chromatic aberration intensity
     */
    void Console_SetChromaticAberrationIntensity(float intensity);

    /**
     * @brief Console integration - Enable/disable auto-exposure
     */
    void Console_SetAutoExposure(bool enabled);

  private:
    ID3D11Device* m_device;
    ID3D11DeviceContext* m_context;
};
