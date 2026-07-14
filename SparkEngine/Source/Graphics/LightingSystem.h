/**
 * @file LightingSystem.h
 * @brief Advanced lighting system with PBR support for Spark Engine
 * @author Spark Engine Team
 * @date 2025
 * 
 * This class provides a comprehensive lighting system supporting physically-based
 * rendering (PBR), shadow mapping, image-based lighting (IBL), and advanced
 * lighting effects for AAA-quality visuals.
 */

#pragma once
#include "../Core/Platform.h"

#include "Utils/Assert.h"
// Phase M: activated Tier 2 graphics orphans — both are pure CPU and
// compile on every platform, so they live outside the Windows guard.
#include "CachedShadowAtlas.h"
#include "ReflectionProbeCache.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <wrl/client.h>
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <functional>

using Microsoft::WRL::ComPtr;

/**
 * @brief Light types supported by the system
 */
enum class LightType
{
    Directional, ///< Directional light (sun)
    Point,       ///< Point light (bulb)
    Spot,        ///< Spot light (flashlight)
    Area,        ///< Area light (panel)
    Environment  ///< Environment/IBL light
};

/**
 * @brief Shadow mapping techniques
 */
enum class ShadowTechnique
{
    None,  ///< No shadows
    Basic, ///< Basic shadow mapping
    PCF,   ///< Percentage Closer Filtering
    VSM,   ///< Variance Shadow Maps
    CSM,   ///< Cascaded Shadow Maps
    PCSS   ///< Percentage Closer Soft Shadows
};

/**
 * @brief Light data structure for shaders
 */
struct LightData
{
    XMFLOAT4 position;     ///< Light position (w = light type)
    XMFLOAT4 direction;    ///< Light direction (w = spot angle)
    XMFLOAT4 color;        ///< Light color (w = intensity)
    XMFLOAT4 attenuation;  ///< Attenuation factors (constant, linear, quadratic, range)
    XMFLOAT4 shadowParams; ///< Shadow parameters (enabled, bias, normal bias, split)
    XMMATRIX lightMatrix;  ///< Light space transformation matrix
    XMMATRIX shadowMatrix; ///< Shadow projection matrix
};

/**
 * @brief Light component
 */
class Light
{
  public:
    Light(LightType type = LightType::Directional);
    ~Light() = default;

    // Type and basic properties
    LightType GetType() const { return m_type; }
    void SetType(LightType type) { m_type = type; }

    // Transform
    const XMFLOAT3& GetPosition() const { return m_position; }
    void SetPosition(const XMFLOAT3& position)
    {
        m_position = position;
        m_dirty = true;
    }

    const XMFLOAT3& GetDirection() const { return m_direction; }
    void SetDirection(const XMFLOAT3& direction)
    {
        m_direction = direction;
        m_dirty = true;
    }

    const XMFLOAT3& GetRotation() const { return m_rotation; }
    void SetRotation(const XMFLOAT3& rotation)
    {
        m_rotation = rotation;
        m_dirty = true;
    }

    // Color and intensity
    const XMFLOAT3& GetColor() const { return m_color; }
    void SetColor(const XMFLOAT3& color) { m_color = color; }

    float GetIntensity() const { return m_intensity; }
    void SetIntensity(float intensity) { m_intensity = intensity; }

    // Attenuation (for point/spot lights)
    float GetRange() const { return m_range; }
    void SetRange(float range) { m_range = range; }

    const XMFLOAT3& GetAttenuation() const { return m_attenuation; }
    void SetAttenuation(const XMFLOAT3& attenuation) { m_attenuation = attenuation; }

    // Spot light specific
    float GetSpotAngle() const { return m_spotAngle; }
    void SetSpotAngle(float angle) { m_spotAngle = angle; }

    float GetSpotExponent() const { return m_spotExponent; }
    void SetSpotExponent(float exponent) { m_spotExponent = exponent; }

    // Shadow settings
    bool GetCastShadows() const { return m_castShadows; }
    void SetCastShadows(bool cast) { m_castShadows = cast; }

    ShadowTechnique GetShadowTechnique() const { return m_shadowTechnique; }
    void SetShadowTechnique(ShadowTechnique technique) { m_shadowTechnique = technique; }

    float GetShadowBias() const { return m_shadowBias; }
    void SetShadowBias(float bias) { m_shadowBias = bias; }

    uint32_t GetShadowMapSize() const { return m_shadowMapSize; }
    void SetShadowMapSize(uint32_t size) { m_shadowMapSize = size; }

    // State
    bool IsEnabled() const { return m_enabled; }
    void SetEnabled(bool enabled) { m_enabled = enabled; }

    bool IsDirty() const { return m_dirty; }
    void SetClean() { m_dirty = false; }

    // Matrix calculations
    XMMATRIX GetLightMatrix() const;
    XMMATRIX GetShadowMatrix() const;

    // Shader data generation
    LightData GetShaderData() const;

    // Console integration
    std::string GetInfo() const;
    void Console_SetProperty(const std::string& property, float value);
    void Console_SetColor(float r, float g, float b);

  private:
    LightType m_type;

    // Transform
    XMFLOAT3 m_position = {0, 0, 0};
    XMFLOAT3 m_direction = {0, -1, 0};
    XMFLOAT3 m_rotation = {0, 0, 0};

    // Color and intensity
    XMFLOAT3 m_color = {1, 1, 1};
    float m_intensity = 1.0f;

    // Attenuation
    float m_range = 10.0f;
    XMFLOAT3 m_attenuation = {1.0f, 0.09f, 0.032f}; // constant, linear, quadratic

    // Spot light
    float m_spotAngle = 45.0f;
    float m_spotExponent = 1.0f;

    // Shadow settings
    bool m_castShadows = true;
    ShadowTechnique m_shadowTechnique = ShadowTechnique::PCF;
    float m_shadowBias = 0.005f;
    uint32_t m_shadowMapSize = 1024;

    // State
    bool m_enabled = true;
    bool m_dirty = true;
};

/**
 * @brief Environment lighting settings
 */
struct EnvironmentLighting
{
    // Image-based lighting
    ComPtr<ID3D11ShaderResourceView> environmentMap; ///< HDR environment map
    ComPtr<ID3D11ShaderResourceView> irradianceMap;  ///< Precomputed irradiance map
    ComPtr<ID3D11ShaderResourceView> prefilterMap;   ///< Prefiltered environment map
    ComPtr<ID3D11ShaderResourceView> brdfLUT;        ///< BRDF integration LUT

    // Sky settings
    XMFLOAT3 skyColor = {0.5f, 0.7f, 1.0f}; ///< Sky color
    float skyIntensity = 1.0f;              ///< Sky intensity
    float skyTurbidity = 2.0f;              ///< Atmospheric turbidity

    // Sun settings (for procedural sky)
    XMFLOAT3 sunDirection = {0.5f, 0.5f, 0.5f}; ///< Sun direction
    float sunSize = 0.04f;                      ///< Sun angular size
    float sunIntensity = 5.0f;                  ///< Sun intensity

    // Fog settings
    bool fogEnabled = false;                ///< Enable volumetric fog
    XMFLOAT3 fogColor = {0.5f, 0.6f, 0.7f}; ///< Fog color
    float fogDensity = 0.01f;               ///< Fog density
    float fogStart = 10.0f;                 ///< Fog start distance
    float fogEnd = 100.0f;                  ///< Fog end distance
};

/**
 * @brief Shadow map resource
 */
struct ShadowMap
{
    ComPtr<ID3D11Texture2D> texture;      ///< Shadow map texture
    ComPtr<ID3D11DepthStencilView> dsv;   ///< Depth stencil view
    ComPtr<ID3D11ShaderResourceView> srv; ///< Shader resource view
    uint32_t size = 0;                    ///< Shadow map size
    XMMATRIX lightMatrix;                 ///< Light projection matrix
    XMMATRIX shadowMatrix;                ///< Shadow transformation matrix
};

/**
 * @brief Cascaded shadow map data
 */
struct CascadedShadowMap
{
    static const int MAX_CASCADES = 4;

    std::vector<ShadowMap> cascades;     ///< Shadow map cascades
    std::vector<float> splitDistances;   ///< Cascade split distances
    std::vector<XMMATRIX> lightMatrices; ///< Light matrices for each cascade
    uint32_t cascadeCount = 3;           ///< Number of cascades
    float splitLambda = 0.5f;            ///< Cascade split interpolation factor
};

/**
 * @brief Lighting system manager
 */
class LightingSystem
{
  public:
    /**
     * @brief Lighting system metrics
     */
    struct LightingMetrics
    {
        uint32_t activeLights = 0;        ///< Number of active lights
        uint32_t shadowCastingLights = 0; ///< Number of shadow casting lights
        uint32_t shadowMapUpdates = 0;    ///< Shadow map updates per frame
        float shadowMapMemory = 0.0f;     ///< Shadow map memory usage (MB)
        float lightCullingTime = 0.0f;    ///< Light culling time (ms)
        float shadowRenderTime = 0.0f;    ///< Shadow rendering time (ms)
        uint32_t visibleLights = 0;       ///< Lights visible to camera
        uint32_t culledLights = 0;        ///< Lights culled this frame
    };

    LightingSystem();
    ~LightingSystem();

    /**
     * @brief Initialize the lighting system
     */
    HRESULT Initialize(ID3D11Device* device, ID3D11DeviceContext* context);

    /**
     * @brief Shutdown the lighting system
     */
    void Shutdown();

    /**
     * @brief Update lighting system
     */
    void Update(float deltaTime, const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix);

    /**
     * @brief Render shadow maps
     */
    void RenderShadowMaps(std::function<void(const XMMATRIX&, const XMMATRIX&)> renderCallback);

    /**
     * @brief Bind lighting data to shaders
     */
    void BindLightingData(ID3D11DeviceContext* context);

    // ------------------------------------------------------------------
    // Phase M: Tier 2 orphan activation — cached shadow atlas + probe cache
    // ------------------------------------------------------------------

    /**
     * @brief Get the cached shadow atlas (Phase M activation).
     *
     * The `CachedShadowAtlas` is lifecycle-owned by the lighting system.
     * It is initialised in `Initialize()`, ticked (`BeginFrame`/`EndFrame`)
     * by `Update()`, and torn down in `Shutdown()`. Callers can request
     * shadow tiles via `GetCachedShadowAtlas().RequestShadow(...)`; cached
     * static lights reuse their tile, dynamic or invalidated lights land
     * in `GetShadowsToRender()`.
     */
    Spark::Graphics::CachedShadowAtlas& GetCachedShadowAtlas() { return m_shadowCache; }
    const Spark::Graphics::CachedShadowAtlas& GetCachedShadowAtlas() const { return m_shadowCache; }

    /**
     * @brief Get the reflection probe cache (Phase M activation).
     *
     * The `ReflectionProbeCache` is lifecycle-owned by the lighting
     * system. Tests and downstream code can register probes, call
     * `Update(cameraX, cameraY, cameraZ)` to get the list of probes to
     * render this frame, and query cache slots via `GetCacheSlot()`.
     * The cache is ticked automatically from `LightingSystem::Update`
     * using the camera position extracted from the inverse view matrix.
     */
    Spark::Graphics::ReflectionProbeCache& GetReflectionProbeCache() { return m_probeCache; }
    const Spark::Graphics::ReflectionProbeCache& GetReflectionProbeCache() const { return m_probeCache; }

    // Light management
    std::shared_ptr<Light> CreateLight(LightType type = LightType::Directional);
    void AddLight(std::shared_ptr<Light> light);
    void RemoveLight(std::shared_ptr<Light> light);
    void RemoveAllLights();
    const std::vector<std::shared_ptr<Light>>& GetLights() const { return m_lights; }

    // Environment lighting
    EnvironmentLighting& GetEnvironmentLighting() { return m_environmentLighting; }
    const EnvironmentLighting& GetEnvironmentLighting() const { return m_environmentLighting; }
    void SetEnvironmentMap(const std::string& filePath);
    void GenerateIBLTextures();

    // Shadow mapping
    void SetGlobalShadowQuality(uint32_t size);
    uint32_t GetGlobalShadowQuality() const { return m_shadowMapSize; }
    void EnableShadows(bool enabled);
    bool AreShadowsEnabled() const { return m_shadowsEnabled; }

    // Light culling
    void EnableLightCulling(bool enabled) { m_lightCullingEnabled = enabled; }
    bool IsLightCullingEnabled() const { return m_lightCullingEnabled; }
    void SetMaxLightsPerTile(uint32_t count) { m_maxLightsPerTile = count; }

    // Metrics
    const LightingMetrics& GetMetrics() const { return m_metrics; }

    // ========================================================================
    // CONSOLE INTEGRATION METHODS
    // ========================================================================

    /**
     * @brief Get lighting system metrics for console
     */
    LightingMetrics Console_GetMetrics() const;

    /**
     * @brief List all lights
     */
    std::string Console_ListLights() const;

    /**
     * @brief Get detailed light information
     */
    std::string Console_GetLightInfo(int lightIndex) const;

    /**
     * @brief Create light via console
     */
    int Console_CreateLight(const std::string& type);

    /**
     * @brief Delete light via console
     */
    bool Console_DeleteLight(int lightIndex);

    /**
     * @brief Set light property via console
     */
    void Console_SetLightProperty(int lightIndex, const std::string& property, float value);

    /**
     * @brief Set light color via console
     */
    void Console_SetLightColor(int lightIndex, float r, float g, float b);

    /**
     * @brief Enable/disable shadows globally
     */
    void Console_EnableShadows(bool enabled);

    /**
     * @brief Set shadow quality
     */
    void Console_SetShadowQuality(const std::string& quality);

    /**
     * @brief Set environment lighting
     */
    void Console_SetEnvironment(const std::string& skyType);

    /**
     * @brief Enable/disable light culling
     */
    void Console_EnableLightCulling(bool enabled);

    /**
     * @brief Reload IBL textures
     */
    void Console_ReloadIBL();

  private:
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;

    // Light storage
    std::vector<std::shared_ptr<Light>> m_lights;
    std::vector<LightData> m_lightDataArray;

    // Environment lighting
    EnvironmentLighting m_environmentLighting;

    // Shadow mapping
    bool m_shadowsEnabled = true;
    uint32_t m_shadowMapSize = 1024;
    std::unordered_map<Light*, std::unique_ptr<ShadowMap>> m_shadowMaps;
    std::unique_ptr<CascadedShadowMap> m_csmShadowMap;

    // Light culling
    bool m_lightCullingEnabled = true;
    uint32_t m_maxLightsPerTile = 64;
    ComPtr<ID3D11Buffer> m_lightBuffer;
    ComPtr<ID3D11ShaderResourceView> m_lightBufferSRV;

    // Constant buffers
    ComPtr<ID3D11Buffer> m_lightDataBuffer;
    ComPtr<ID3D11Buffer> m_environmentBuffer;
    ComPtr<ID3D11Buffer> m_shadowDataBuffer;

    // Metrics
    LightingMetrics m_metrics;

    // Phase M: Tier 2 orphan activations — pure CPU members that live
    // outside the Windows guard so they are consistently available on
    // every platform's build of LightingSystem.
    Spark::Graphics::CachedShadowAtlas m_shadowCache;
    Spark::Graphics::ReflectionProbeCache m_probeCache;

    // Helper methods
    HRESULT CreateConstantBuffers();
    HRESULT CreateShadowMap(uint32_t size, ShadowMap& shadowMap);
    HRESULT CreateCascadedShadowMap();
    void UpdateLightBuffer();
    void UpdateShadowMaps(const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix);
    void CullLights(const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix);
    void CalculateCSMSplits(float nearPlane, float farPlane, CascadedShadowMap& csm);
    XMMATRIX CalculateLightMatrix(const Light& light, const XMMATRIX& viewMatrix, float nearPlane, float farPlane);

    // IBL generation
    HRESULT GenerateIrradianceMap(ID3D11ShaderResourceView* environmentMap);
    HRESULT GeneratePrefilterMap(ID3D11ShaderResourceView* environmentMap);
    HRESULT GenerateBRDFLUT();

    // Default environment
    HRESULT CreateDefaultEnvironment();
};

// Utility functions
std::string LightTypeToString(LightType type);
LightType StringToLightType(const std::string& str);
std::string ShadowTechniqueToString(ShadowTechnique technique);
ShadowTechnique StringToShadowTechnique(const std::string& str);
