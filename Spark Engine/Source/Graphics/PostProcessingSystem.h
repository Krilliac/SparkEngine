/**
 * @file PostProcessingSystem.h
 * @brief Post-processing system for Spark Engine (Bloom, Tone Mapping, Color Grading, FXAA)
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once
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
 * @brief Basic post-processing system
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
     * @brief Update the system
     */
    void Update(float deltaTime);
    
    /**
     * @brief Console integration - Set exposure
     */
    void Console_SetExposure(float exposure);
    
    /**
     * @brief Console integration - List effects
     */
    std::string Console_ListEffects() const;

private:
    ID3D11Device* m_device;
    ID3D11DeviceContext* m_context;
};
