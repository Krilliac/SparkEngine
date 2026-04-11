/**
 * @file MaterialSystem.h
 * @brief Advanced material system for AAA-quality rendering with PBR support
 * @author Spark Engine Team
 * @date 2025
 * 
 * This class provides a comprehensive material system supporting physically-based
 * rendering (PBR), material variants, texture streaming, and extensive console
 * integration for real-time material debugging and authoring.
 */

#pragma once
#include "../Core/Platform.h"

#include "Utils/Assert.h"
// Phase P: activated Tier 2 graphics orphan — pure CPU persistent
// material constant buffer manager, runs on every platform.
#include "PersistentMaterialCB.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <functional>
#include <mutex>
#include <chrono>

namespace Spark
{
    class LocalFileCache;
}

using Microsoft::WRL::ComPtr;
using namespace DirectX;

/**
 * @brief GPU-aligned constant buffer matching HLSL material layout
 */
struct alignas(16) MaterialConstants
{
    XMFLOAT4 albedoColor;    // rgba
    float metallicFactor;    // metallic
    float roughnessFactor;   // roughness
    float normalScale;       // normal map intensity
    float occlusionStrength; // AO strength
    XMFLOAT3 emissiveColor;  // emissive RGB
    float emissiveFactor;    // emissive intensity
    float alphaCutoff;       // alpha test threshold
    float indexOfRefraction; // IOR
    float pad0, pad1;        // padding to 16-byte alignment
};

/**
 * @brief Material blend modes
 */
enum class BlendMode
{
    Opaque,      ///< Fully opaque material
    AlphaTest,   ///< Alpha testing (cutout)
    Transparent, ///< Alpha blending
    Additive,    ///< Additive blending
    Multiply,    ///< Multiplicative blending
    Screen       ///< Screen blending
};

/**
 * @brief Material cull modes
 */
enum class CullMode
{
    None,  ///< No culling (double-sided)
    Front, ///< Front face culling
    Back   ///< Back face culling (default)
};

/**
 * @brief Texture types for material slots
 */
enum class MaterialTextureType
{
    Albedo,             ///< Base color/albedo texture
    Normal,             ///< Normal map texture
    Metallic,           ///< Metallic map texture
    Roughness,          ///< Roughness map texture
    Occlusion,          ///< Ambient occlusion texture
    Emissive,           ///< Emissive texture
    Height,             ///< Height/displacement map
    DetailAlbedo,       ///< Detail albedo texture
    DetailNormal,       ///< Detail normal map
    Subsurface,         ///< Subsurface scattering map
    Transmission,       ///< Transmission map
    Clearcoat,          ///< Clearcoat layer map
    ClearcoatRoughness, ///< Clearcoat roughness map
    Anisotropy,         ///< Anisotropy direction map
    Custom0,            ///< Custom texture slot 0
    Custom1,            ///< Custom texture slot 1
    Custom2,            ///< Custom texture slot 2
    Custom3             ///< Custom texture slot 3
};

/**
 * @brief Texture sampling parameters
 */
struct TextureSampling
{
    D3D11_FILTER filter = D3D11_FILTER_ANISOTROPIC;                   ///< Texture filtering mode
    D3D11_TEXTURE_ADDRESS_MODE addressU = D3D11_TEXTURE_ADDRESS_WRAP; ///< U address mode
    D3D11_TEXTURE_ADDRESS_MODE addressV = D3D11_TEXTURE_ADDRESS_WRAP; ///< V address mode
    D3D11_TEXTURE_ADDRESS_MODE addressW = D3D11_TEXTURE_ADDRESS_WRAP; ///< W address mode
    UINT maxAnisotropy = 16;                                          ///< Maximum anisotropy level
    float mipLODBias = 0.0f;                                          ///< MIP level bias
    float minLOD = 0.0f;                                              ///< Minimum LOD
    float maxLOD = D3D11_FLOAT32_MAX;                                 ///< Maximum LOD
    XMFLOAT4 borderColor = {0, 0, 0, 0};                              ///< Border color for border address mode
};

/**
 * @brief Material texture slot
 */
struct MaterialTexture
{
    ComPtr<ID3D11ShaderResourceView> texture; ///< DirectX texture resource
    TextureSampling sampling;                 ///< Sampling parameters
    XMFLOAT2 tiling = {1.0f, 1.0f};           ///< UV tiling
    XMFLOAT2 offset = {0.0f, 0.0f};           ///< UV offset
    float intensity = 1.0f;                   ///< Texture intensity/strength
    bool enabled = false;                     ///< Whether this texture slot is active
    std::string filePath;                     ///< Original file path for hot-reloading
};

/**
 * @brief PBR material properties
 */
struct PBRProperties
{
    XMFLOAT4 albedoColor = {1.0f, 1.0f, 1.0f, 1.0f}; ///< Base albedo color
    float metallicFactor = 0.0f;                     ///< Metallic factor (0 = dielectric, 1 = metallic)
    float roughnessFactor = 0.5f;                    ///< Roughness factor (0 = mirror, 1 = fully rough)
    float normalScale = 1.0f;                        ///< Normal map intensity
    float occlusionStrength = 1.0f;                  ///< Ambient occlusion strength
    XMFLOAT3 emissiveColor = {0.0f, 0.0f, 0.0f};     ///< Emissive color
    float emissiveFactor = 0.0f;                     ///< Emissive intensity
    float alphaCutoff = 0.5f;                        ///< Alpha cutoff for alpha testing
    float indexOfRefraction = 1.5f;                  ///< Index of refraction for dielectrics
};

/**
 * @brief Advanced material properties
 */
struct AdvancedProperties
{
    // Subsurface scattering
    bool subsurfaceEnabled = false;                ///< Enable subsurface scattering
    XMFLOAT3 subsurfaceColor = {1.0f, 1.0f, 1.0f}; ///< Subsurface scattering color
    float subsurfaceRadius = 1.0f;                 ///< Subsurface scattering radius

    // Clearcoat
    bool clearcoatEnabled = false;   ///< Enable clearcoat layer
    float clearcoatFactor = 0.0f;    ///< Clearcoat layer strength
    float clearcoatRoughness = 0.0f; ///< Clearcoat layer roughness

    // Anisotropy
    bool anisotropyEnabled = false;              ///< Enable anisotropic reflections
    float anisotropyFactor = 0.0f;               ///< Anisotropy strength
    XMFLOAT2 anisotropyDirection = {1.0f, 0.0f}; ///< Anisotropy direction

    // Transmission
    bool transmissionEnabled = false;                ///< Enable transmission
    float transmissionFactor = 0.0f;                 ///< Transmission strength
    XMFLOAT3 transmissionColor = {1.0f, 1.0f, 1.0f}; ///< Transmission color

    // Sheen (fabric-like materials)
    bool sheenEnabled = false;                ///< Enable sheen
    XMFLOAT3 sheenColor = {0.0f, 0.0f, 0.0f}; ///< Sheen color
    float sheenRoughness = 0.0f;              ///< Sheen roughness

    // Iridescence
    bool iridescenceEnabled = false;     ///< Enable iridescence
    float iridescenceFactor = 0.0f;      ///< Iridescence strength
    float iridescenceIOR = 1.3f;         ///< Iridescence IOR
    float iridescenceThickness = 100.0f; ///< Iridescence thickness (nm)
};

/**
 * @brief Material render state
 */
struct MaterialRenderState
{
    BlendMode blendMode = BlendMode::Opaque; ///< Blending mode
    CullMode cullMode = CullMode::Back;      ///< Face culling mode
    bool depthTest = true;                   ///< Enable depth testing
    bool depthWrite = true;                  ///< Enable depth writing
    bool castShadows = true;                 ///< Cast shadows
    bool receiveShadows = true;              ///< Receive shadows
    int renderQueue = 2000;                  ///< Render queue priority
    bool doubleSided = false;                ///< Double-sided rendering
};

/**
 * @brief Material definition
 */
class Material
{
  public:
    Material(const std::string& name);
    ~Material() = default;

    /**
     * @brief Validate that PBR material properties are within expected ranges.
     * @return true if all properties are valid.
     */
    static bool ValidatePBRProperties(const PBRProperties& props)
    {
        ASSERT_MSG(props.metallicFactor >= 0.0f && props.metallicFactor <= 1.0f, "Metallic factor must be in [0, 1]");
        ASSERT_MSG(props.roughnessFactor >= 0.0f && props.roughnessFactor <= 1.0f,
                   "Roughness factor must be in [0, 1]");
        ASSERT_MSG(props.occlusionStrength >= 0.0f && props.occlusionStrength <= 1.0f,
                   "Occlusion strength must be in [0, 1]");
        ASSERT_MSG(props.alphaCutoff >= 0.0f && props.alphaCutoff <= 1.0f, "Alpha cutoff must be in [0, 1]");
        ASSERT_MSG(props.indexOfRefraction > 0.0f, "Index of refraction must be positive");
        ASSERT_MSG(props.emissiveFactor >= 0.0f, "Emissive factor must be non-negative");
        return props.metallicFactor >= 0.0f && props.metallicFactor <= 1.0f && props.roughnessFactor >= 0.0f &&
               props.roughnessFactor <= 1.0f && props.occlusionStrength >= 0.0f && props.occlusionStrength <= 1.0f &&
               props.alphaCutoff >= 0.0f && props.alphaCutoff <= 1.0f && props.indexOfRefraction > 0.0f &&
               props.emissiveFactor >= 0.0f;
    }

    // Getters
    const std::string& GetName() const { return m_name; }
    const PBRProperties& GetPBRProperties() const { return m_pbrProperties; }
    const AdvancedProperties& GetAdvancedProperties() const { return m_advancedProperties; }
    const MaterialRenderState& GetRenderState() const { return m_renderState; }
    const MaterialTexture& GetTexture(MaterialTextureType type) const;
    const std::string& GetActiveVariant() const;
    std::vector<std::string> GetAvailableVariants() const;

    // Setters
    void SetPBRProperties(const PBRProperties& properties)
    {
        ASSERT_MSG(ValidatePBRProperties(properties), "Invalid PBR properties");
        m_pbrProperties = properties;
    }
    void SetAdvancedProperties(const AdvancedProperties& properties) { m_advancedProperties = properties; }
    void SetRenderState(const MaterialRenderState& state) { m_renderState = state; }
    void SetTexture(MaterialTextureType type, const MaterialTexture& texture);

    // Texture management
    bool LoadTexture(MaterialTextureType type, const std::string& filePath, ID3D11Device* device);
    void UnloadTexture(MaterialTextureType type);
    bool HasTexture(MaterialTextureType type) const;

    // Shader parameter binding
    void BindToShader(ID3D11DeviceContext* context) const;

    // Pipeline state compilation
    HRESULT CompileMaterial(ID3D11Device* device);

    // Shader permutation generation
    std::vector<std::string> GetShaderPermutation() const;

    // Material instancing (clone with overridable properties)
    std::shared_ptr<Material> CreateInstance(const std::string& instanceName) const;

    // Hot-reload: reload textures from disk and recompile pipeline state
    bool ReloadMaterial(ID3D11Device* device);

    // Material variants
    void CreateVariant(const std::string& variantName, const std::vector<std::string>& defines);
    void SetActiveVariant(const std::string& variantName);

    // Serialization
    bool SaveToFile(const std::string& filePath) const;
    bool LoadFromFile(const std::string& filePath, ID3D11Device* device);

    void SetFileCache(Spark::LocalFileCache* cache) { m_fileCache = cache; }

    // Console integration
    std::string GetDetailedInfo() const;
    void Console_SetProperty(const std::string& property, float value);
    void Console_SetColor(const std::string& property, float r, float g, float b);
    void Console_ReloadTextures(ID3D11Device* device);

    friend class MaterialSystem;

  private:
    std::string m_name;
    PBRProperties m_pbrProperties;
    AdvancedProperties m_advancedProperties;
    MaterialRenderState m_renderState;
    std::unordered_map<MaterialTextureType, MaterialTexture> m_textures;
    std::unordered_map<std::string, std::vector<std::string>> m_variants;
    std::string m_activeVariant;

    // Compiled D3D11 pipeline state objects
    ComPtr<ID3D11BlendState> m_blendState;
    ComPtr<ID3D11DepthStencilState> m_depthStencilState;
    ComPtr<ID3D11RasterizerState> m_rasterizerState;
    ComPtr<ID3D11Buffer> m_constantBuffer;
    bool m_compiled = false;
    Spark::LocalFileCache* m_fileCache = nullptr;
};

/**
 * @brief Material system manager
 */
class MaterialSystem
{
  public:
    /**
     * @brief Material system performance metrics
     */
    struct MaterialMetrics
    {
        int loadedMaterials;   ///< Number of loaded materials
        int textureCount;      ///< Total number of loaded textures
        size_t textureMemory;  ///< Texture memory usage in bytes
        int materialSwitches;  ///< Material switches per frame
        int textureBinds;      ///< Texture binds per frame
        float averageLoadTime; ///< Average material load time
        int failedLoads;       ///< Number of failed material loads
        bool hotReloadEnabled; ///< Hot reload status
        int variantCount;      ///< Number of material variants
    };

    MaterialSystem();
    ~MaterialSystem();

    /**
     * @brief Initialize the material system
     * @param device   D3D11 device (must not be null)
     * @param context  D3D11 device context (must not be null)
     */
    HRESULT Initialize(ID3D11Device* device, ID3D11DeviceContext* context);

    /**
     * @brief Shutdown the material system
     */
    void Shutdown();

    // Material management

    /**
     * @brief Create a new material with the given name.
     * @param name  Non-empty unique name for the material.
     */
    std::shared_ptr<Material> CreateMaterial(const std::string& name);
    std::shared_ptr<Material> LoadMaterial(const std::string& filePath);
    std::shared_ptr<Material> GetMaterial(const std::string& name) const;
    void UnloadMaterial(const std::string& name);
    void UnloadAllMaterials();

    // Material instancing: clone a template material with overridable properties
    std::shared_ptr<Material> CreateMaterialInstance(const std::string& templateName, const std::string& instanceName);

    // Bind a material for rendering: sets SRVs, constant buffer, blend/rasterizer states
    void BindMaterial(const std::string& name);
    void BindMaterial(const std::shared_ptr<Material>& material);

    // Get shader permutation defines for a material
    std::vector<std::string> GetShaderPermutation(const std::string& name) const;

    // Reload a material from disk (textures + recompile pipeline state)
    bool ReloadMaterial(const std::string& name);

    // Get material system metrics
    MaterialMetrics GetMetrics() const;

    // Default materials
    std::shared_ptr<Material> GetDefaultMaterial() const { return m_defaultMaterial; }
    std::shared_ptr<Material> GetErrorMaterial() const { return m_errorMaterial; }

    // ------------------------------------------------------------------
    // Phase P: PersistentMaterialCB accessor
    // ------------------------------------------------------------------

    /**
     * @brief Get the persistent material CB manager (Phase P activation).
     *
     * The `PersistentMaterialCBManager` is lifecycle-owned by the
     * material system. It manages a 4096-material × 256-byte mega
     * buffer with per-material dirty tracking. Callers register a
     * material via `RegisterMaterial(id)`, upload data with
     * `UpdateMaterial(id, data, size)` (which hashes the data and
     * only marks a GPU upload when the hash changes), and drain
     * dirty slots via `GetDirtySlots()` / `ClearDirtyFlags()` after
     * the GPU-side upload completes.
     *
     * The manager is pure CPU — it tracks a shadow buffer + dirty
     * bookkeeping, no D3D11 resources. A future GPU-side companion
     * that actually creates the mega `ID3D11Buffer` can read the
     * shadow via `GetMaterialData()` and upload only the dirty
     * sub-ranges.
     */
    Spark::Graphics::PersistentMaterialCBManager& GetPersistentMaterialCB() { return m_persistentCB; }
    const Spark::Graphics::PersistentMaterialCBManager& GetPersistentMaterialCB() const { return m_persistentCB; }

    // Texture management
    ComPtr<ID3D11ShaderResourceView> LoadTexture(const std::string& filePath);
    void UnloadTexture(const std::string& filePath);
    ComPtr<ID3D11SamplerState> GetSampler(const TextureSampling& sampling);

    // Hot reloading
    void EnableHotReload(bool enabled) { m_hotReloadEnabled = enabled; }
    void EnableHotReloading(bool enabled);
    void UpdateHotReload();
    int ReloadAllMaterials();

    // Frame management
    void BeginFrame();
    void EndFrame();

    // ========================================================================
    // CONSOLE INTEGRATION METHODS
    // ========================================================================

    /**
     * @brief Get material system metrics
     */
    MaterialMetrics Console_GetMetrics() const;

    /**
     * @brief List all loaded materials
     */
    std::string Console_ListMaterials() const;

    /**
     * @brief Get detailed material information
     */
    std::string Console_GetMaterialInfo(const std::string& materialName) const;

    /**
     * @brief Reload specific material
     */
    bool Console_ReloadMaterial(const std::string& materialName);

    /**
     * @brief Reload all materials
     */
    int Console_ReloadAllMaterials();

    /**
     * @brief Create material variant
     */
    bool Console_CreateVariant(const std::string& materialName, const std::string& variantName,
                               const std::vector<std::string>& defines);

    /**
     * @brief Set material property via console
     */
    void Console_SetMaterialProperty(const std::string& materialName, const std::string& property, float value);

    /**
     * @brief Set material color property via console
     */
    void Console_SetMaterialColor(const std::string& materialName, const std::string& property, float r, float g,
                                  float b);

    /**
     * @brief Enable/disable hot reload
     */
    void Console_SetHotReload(bool enabled);

    /**
     * @brief Clear material cache
     */
    void Console_ClearCache();

    /**
     * @brief Force garbage collection of unused materials
     */
    void Console_GarbageCollect();

    /**
     * @brief Set texture quality settings
     */
    void Console_SetTextureQuality(const std::string& quality);

    /**
     * @brief Get texture memory usage
     */
    std::string Console_GetTextureMemoryInfo() const;

    /**
     * @brief Validate all loaded materials
     */
    int Console_ValidateMaterials();

    /**
     * @brief Dump detailed material information
     */
    std::string Console_DumpMaterialDetails(const std::string& materialName) const;

    /**
     * @brief Export material to file
     */
    bool Console_ExportMaterial(const std::string& materialName, const std::string& filePath);

    /**
     * @brief Import material from file
     */
    bool Console_ImportMaterial(const std::string& filePath);

    /**
     * @brief List all available texture types
     */
    std::string Console_ListTextureTypes() const;

    /**
     * @brief Load texture to specific material slot
     */
    bool Console_LoadTextureToSlot(const std::string& materialName, const std::string& textureType,
                                   const std::string& texturePath);

    /**
     * @brief Unload texture from material slot
     */
    void Console_UnloadTextureFromSlot(const std::string& materialName, const std::string& textureType);

    /**
     * @brief List material variants
     */
    std::string Console_ListMaterialVariants(const std::string& materialName) const;

  private:
    ID3D11Device* m_device;
    ID3D11DeviceContext* m_context;

    // Material storage
    std::unordered_map<std::string, std::shared_ptr<Material>> m_materials;
    std::unordered_map<std::string, ComPtr<ID3D11ShaderResourceView>> m_textureCache;
    std::unordered_map<size_t, ComPtr<ID3D11SamplerState>> m_samplerCache;

    // Default materials
    std::shared_ptr<Material> m_defaultMaterial;
    std::shared_ptr<Material> m_errorMaterial;

    // Hot reloading
    bool m_hotReloadEnabled;
    std::unordered_map<std::string, uint64_t> m_fileTimestamps;

    // Performance tracking
    mutable std::mutex m_metricsMutex;
    MaterialMetrics m_metrics;
    std::chrono::high_resolution_clock::time_point m_frameStartTime;

    // Phase P: persistent material constant buffer manager. Pure CPU —
    // tracks a shadow buffer + per-material dirty bookkeeping so
    // materials only upload to the GPU when their data actually
    // changes. Lives outside the Windows guard; runs on every
    // platform's build of MaterialSystem.
    Spark::Graphics::PersistentMaterialCBManager m_persistentCB;

    // Helper methods
    HRESULT CreateDefaultMaterials();
    HRESULT CreateSampler(const TextureSampling& sampling, ID3D11SamplerState** sampler);
    size_t HashSampling(const TextureSampling& sampling) const;
    uint64_t GetFileTimestamp(const std::string& filePath) const;
    ComPtr<ID3D11ShaderResourceView> LoadTextureFromFile(const std::string& filePath);
    void UpdateMetrics();
    void PerformPeriodicMaintenance();
    std::string TextureTypeToString(MaterialTextureType type) const;
    MaterialTextureType StringToTextureType(const std::string& str) const;
};
