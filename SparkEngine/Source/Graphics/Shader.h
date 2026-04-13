/**
 * @file Shader.h
 * @brief Advanced HLSL shader management with PBR, lighting, and console integration
 * @author Spark Engine Team
 * @date 2025
 * 
 * This class provides a comprehensive shader management system for DirectX 11,
 * handling vertex and pixel shader compilation, loading, constant buffer
 * updates, and advanced rendering features for AAA-quality games.
 */

#pragma once

#include "Utils/Assert.h"
#include "../Core/framework.h"
// Phase O: activated Tier 2 graphics orphan — pure CPU keyword / variant
// bookkeeping, lives outside the Windows guard.
#include "ShaderVariantSystem.h"

// Forward declarations for RHI types used by the Linux rendering path.
// Full headers are included only in the .cpp files that need them.
namespace Spark::RHI
{
    class IRHIDevice;
    class IRHIShader;
    class IRHIPipelineState;
    class IRHIBuffer;
    enum class RHIShaderStage;
} // namespace Spark::RHI
#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <wrl/client.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <functional>
#include <mutex>

namespace Spark
{
    class LocalFileCache;
}

using Microsoft::WRL::ComPtr;

/**
 * @brief Legacy constant buffer structure for backward compatibility
 */
struct ConstantBuffer
{
    DirectX::XMMATRIX World;
    DirectX::XMMATRIX View;
    DirectX::XMMATRIX Projection;
};


/**
 * @brief Shader variant structure for managing different shader configurations
 */
struct ShaderVariant
{
    int id;                           ///< Unique variant ID
    std::string name;                 ///< Variant name
    std::string baseName;             ///< Base shader name
    std::vector<std::string> defines; ///< Preprocessor defines
    bool isCompiled;                  ///< Compilation status
    FILETIME lastModified;            ///< Last modification time

    // Constructor for C++14 compatibility
    ShaderVariant() : id(-1), isCompiled(false)
    {
        lastModified.dwLowDateTime = 0;
        lastModified.dwHighDateTime = 0;
    }
};

/**
 * @brief Shader types supported by the engine
 */
enum class ShaderType
{
    VERTEX_SHADER = 0,
    PIXEL_SHADER = 1,
    GEOMETRY_SHADER = 2,
    HULL_SHADER = 3,
    DOMAIN_SHADER = 4,
    COMPUTE_SHADER = 5
};

/**
 * @brief Shader compilation flags and settings (C++14 compatible)
 */
struct ShaderCompilationFlags
{
    bool enableDebug;                      ///< Include debug information
    bool enableOptimization;               ///< Enable shader optimization
    bool enableValidation;                 ///< Enable shader validation
    bool treatWarningsAsErrors;            ///< Treat warnings as compilation errors
    std::string entryPoint;                ///< Shader entry point function name
    std::string target;                    ///< Shader model target (auto-detected if empty)
    std::vector<std::string> defines;      ///< Preprocessor defines
    std::vector<std::string> includePaths; ///< Include search paths

    // Constructor for C++14 compatibility
    ShaderCompilationFlags()
        : enableDebug(false), enableOptimization(true), enableValidation(true), treatWarningsAsErrors(false),
          entryPoint("main"), target("")
    {
    }
};

/**
 * @brief Per-frame constant buffer structure
 */
struct PerFrameConstants
{
    DirectX::XMMATRIX ViewMatrix;
    DirectX::XMMATRIX ProjectionMatrix;
    DirectX::XMMATRIX ViewProjectionMatrix;
    DirectX::XMFLOAT3 CameraPosition;
    float Time;
    DirectX::XMFLOAT3 CameraDirection;
    float DeltaTime;
    DirectX::XMFLOAT2 ScreenResolution;
    DirectX::XMFLOAT2 InvScreenResolution;

    // Lighting parameters
    DirectX::XMFLOAT3 DirectionalLightDir;
    float DirectionalLightIntensity;
    DirectX::XMFLOAT3 DirectionalLightColor;
    float AmbientIntensity;
    DirectX::XMFLOAT3 AmbientColor;
    float _padding1;
};

/**
 * @brief Per-object constant buffer structure
 */
struct PerObjectConstants
{
    DirectX::XMMATRIX WorldMatrix;
    DirectX::XMMATRIX WorldViewProjectionMatrix;
    DirectX::XMMATRIX WorldInverseTransposeMatrix;
    DirectX::XMMATRIX PreviousWorldMatrix;
    DirectX::XMFLOAT3 ObjectPosition;
    float ObjectScale;
    DirectX::XMFLOAT4 ObjectColor;
    DirectX::XMFLOAT4 MaterialProperties; // x: metallic, y: roughness, z: emissive, w: alpha
    DirectX::XMFLOAT4 UVTiling;           // xy: tiling, zw: offset
};

/**
 * @brief Per-material constant buffer structure
 */
struct PerMaterialConstants
{
    DirectX::XMFLOAT4 AlbedoColor;
    float MetallicFactor;
    float RoughnessFactor;
    float NormalScale;
    float OcclusionStrength;
    float EmissiveFactor;
    float AlphaCutoff;
    DirectX::XMFLOAT2 _materialPadding;
};

/**
 * @brief Lighting data structure for advanced lighting
 */
struct LightingData
{
    // Directional lights
    DirectX::XMFLOAT4 DirectionalLights[4];      // xyz: direction, w: intensity
    DirectX::XMFLOAT4 DirectionalLightColors[4]; // rgb: color, a: shadow index

    // Point lights
    DirectX::XMFLOAT4 PointLightPositions[32]; // xyz: position, w: range
    DirectX::XMFLOAT4 PointLightColors[32];    // rgb: color, a: intensity

    // Spot lights
    DirectX::XMFLOAT4 SpotLightPositions[16];  // xyz: position, w: range
    DirectX::XMFLOAT4 SpotLightDirections[16]; // xyz: direction, w: inner cone
    DirectX::XMFLOAT4 SpotLightColors[16];     // rgb: color, a: outer cone

    int NumDirectionalLights;
    int NumPointLights;
    int NumSpotLights;
    float LightingScale;

    // IBL parameters
    float IBLIntensity;
    float IBLRotation;
    float MaxReflectionLOD;
    float _lightingPadding;
};

/**
 * @brief Post-processing parameters
 */
struct PostProcessingConstants
{
    float Exposure;
    float Gamma;
    float Contrast;
    float Saturation;
    float Brightness;
    float Vignette;
    float FilmGrain;
    float ChromaticAberration;
    DirectX::XMFLOAT4 ColorGrading;      // xyz: shadows/midtones/highlights, w: temperature
    DirectX::XMFLOAT4 TonemappingParams; // ACES, Reinhard, etc.
};

/**
 * @brief Individual shader resource
 */
class ShaderResource
{
  public:
    explicit ShaderResource(ShaderType type) : m_type(type) {}
    virtual ~ShaderResource() = default;

    ShaderType GetType() const { return m_type; }
    virtual void Bind(ID3D11DeviceContext* context) = 0;
    virtual void Unbind(ID3D11DeviceContext* context) = 0;
    virtual bool IsValid() const = 0;

  protected:
    ShaderType m_type;
};

/**
 * @brief Vertex shader resource
 */
class VertexShaderResource : public ShaderResource
{
  public:
    VertexShaderResource() : ShaderResource(ShaderType::VERTEX_SHADER) {}

    void Bind(ID3D11DeviceContext* context) override;
    void Unbind(ID3D11DeviceContext* context) override;
    bool IsValid() const override { return m_vertexShader && m_inputLayout; }

    ComPtr<ID3D11VertexShader> m_vertexShader;
    ComPtr<ID3D11InputLayout> m_inputLayout;
    ComPtr<ID3DBlob> m_shaderBlob; // Keep for input layout creation
};

/**
 * @brief Pixel shader resource
 */
class PixelShaderResource : public ShaderResource
{
  public:
    PixelShaderResource() : ShaderResource(ShaderType::PIXEL_SHADER) {}

    void Bind(ID3D11DeviceContext* context) override;
    void Unbind(ID3D11DeviceContext* context) override;
    bool IsValid() const override { return m_pixelShader; }

    ComPtr<ID3D11PixelShader> m_pixelShader;
};

/**
 * @brief Enhanced shader management class with AAA features
 * 
 * Comprehensive shader system supporting PBR materials, advanced lighting,
 * hot-reloading, shader variants, and extensive console integration.
 */
class Shader
{
  public:
    /**
     * @brief Default constructor
     */
    Shader();

    /**
     * @brief Destructor
     */
    ~Shader();

    /**
     * @brief Initialize the shader system
     * @param device DirectX 11 device for resource creation
     * @param context DirectX 11 device context for rendering operations
     * @return HRESULT indicating success or failure of initialization
     */
    HRESULT Initialize(ID3D11Device* device, ID3D11DeviceContext* context);

    /**
     * @brief Clean up all shader resources
     */
    void Shutdown();

    // ========================================================================
    // SHADER LOADING AND MANAGEMENT
    // ========================================================================

    /**
     * @brief Load and compile a vertex shader from file
     * @param filename Path to the HLSL vertex shader file
     * @param flags Compilation flags and settings
     * @return HRESULT indicating success or failure
     */
    HRESULT LoadVertexShader(const std::wstring& filename,
                             const ShaderCompilationFlags& flags = ShaderCompilationFlags());

    /**
     * @brief Load and compile a pixel shader from file
     * @param filename Path to the HLSL pixel shader file
     * @param flags Compilation flags and settings
     * @return HRESULT indicating success or failure
     */
    HRESULT LoadPixelShader(const std::wstring& filename,
                            const ShaderCompilationFlags& flags = ShaderCompilationFlags());

    /**
     * @brief Load shader from memory
     * @param source Shader source code
     * @param type Shader type to compile
     * @param flags Compilation flags and settings
     * @return HRESULT indicating success or failure
     */
    HRESULT LoadShaderFromSource(const std::string& source, ShaderType type,
                                 const ShaderCompilationFlags& flags = ShaderCompilationFlags());

    /**
     * @brief Load shader from file
     * @param filePath Path to the shader file
     * @param type Shader type to compile
     * @param flags Compilation flags and settings
     * @return HRESULT indicating success or failure
     */
    HRESULT LoadFromFile(const std::string& filePath, ShaderType type,
                         const ShaderCompilationFlags& flags = ShaderCompilationFlags());

    /**
     * @brief Create a shader variant with preprocessor defines
     * @param baseName Base shader name
     * @param defines Preprocessor defines for the variant
     * @return Shader variant ID, or -1 on failure
     */
    int CreateShaderVariant(const std::string& baseName, const std::vector<std::string>& defines);

    /**
     * @brief Set active shader variant
     * @param variantId Shader variant ID
     */
    void SetActiveVariant(int variantId);

    /**
     * @brief Hot-reload shaders from disk
     * @return Number of shaders successfully reloaded
     */
    int HotReloadShaders();

    /**
     * @brief Set the file cache used for shader source loading
     * @param cache Non-owning pointer to a LocalFileCache instance
     */
    void SetFileCache(Spark::LocalFileCache* cache) { m_fileCache = cache; }

    // ========================================================================
    // SHADER BINDING AND STATE
    // ========================================================================

    /**
     * @brief Bind shaders to the graphics pipeline
     */
    void SetShaders();

    /**
     * @brief Unbind all shaders
     */
    void UnbindShaders();

    /**
     * @brief Check if shaders are valid and ready for rendering
     */
    bool IsValid() const;

    // ========================================================================
    // CONSTANT BUFFER MANAGEMENT
    // ========================================================================

    /**
     * @brief Update per-frame constants
     * @param constants Per-frame constant data
     */
    void UpdatePerFrameConstants(const PerFrameConstants& constants);

    /**
     * @brief Update per-object constants
     * @param constants Per-object constant data
     */
    void UpdatePerObjectConstants(const PerObjectConstants& constants);

    /**
     * @brief Update per-material constants
     * @param constants Per-material constant data
     */
    void UpdatePerMaterialConstants(const PerMaterialConstants& constants);

    /**
     * @brief Update lighting data
     * @param lightingData Advanced lighting parameters
     */
    void UpdateLightingData(const LightingData& lightingData);

    /**
     * @brief Update post-processing constants
     * @param constants Post-processing parameters
     */
    void UpdatePostProcessingConstants(const PostProcessingConstants& constants);

    // ========================================================================
    // CONSOLE INTEGRATION METHODS
    // ========================================================================

    /**
     * @brief Shader performance metrics structure
     */
    struct ShaderMetrics
    {
        int compiledShaders;      ///< Number of compiled shaders
        int failedCompilations;   ///< Number of failed compilations
        int activeVariants;       ///< Number of active shader variants
        int hotReloadCount;       ///< Number of hot reloads performed
        float lastCompileTime;    ///< Last compilation time in milliseconds
        float totalCompileTime;   ///< Total compilation time
        size_t shaderMemoryUsage; ///< Memory usage of compiled shaders
        bool hotReloadEnabled;    ///< Hot reload status

        // Constructor for C++14 compatibility
        ShaderMetrics()
            : compiledShaders(0), failedCompilations(0), activeVariants(0), hotReloadCount(0), lastCompileTime(0.0f),
              totalCompileTime(0.0f), shaderMemoryUsage(0), hotReloadEnabled(false)
        {
        }
    };

    /**
     * @brief Get shader compilation metrics
     */
    ShaderMetrics Console_GetMetrics() const;

    /**
     * @brief Force recompilation of all shaders via console
     */
    void Console_RecompileAll();

    /**
     * @brief Enable/disable hot reloading via console
     * @param enabled Hot reload state
     */
    void Console_SetHotReload(bool enabled);

    /**
     * @brief Set shader compilation flags via console
     * @param enableDebug Enable debug information
     * @param enableOptimization Enable optimization
     */
    void Console_SetCompilationFlags(bool enableDebug, bool enableOptimization);

    /**
     * @brief List all loaded shaders via console
     * @return String containing shader list
     */
    std::string Console_ListShaders() const;

    /**
     * @brief Get detailed shader information via console
     * @param shaderName Name of the shader to inspect
     * @return Detailed shader information string
     */
    std::string Console_GetShaderInfo(const std::string& shaderName) const;

    /**
     * @brief Register console state change callback
     * @param callback Function to call when shader state changes
     */
    void Console_RegisterStateCallback(std::function<void()> callback);

    /**
     * @brief Validate all loaded shaders via console
     * @return Number of validation errors found
     */
    int Console_ValidateShaders();

    /**
     * @brief Clear shader cache via console
     */
    void Console_ClearCache();

    /**
     * @brief Set shader search paths via console
     * @param paths Vector of search paths for shader files
     */
    void Console_SetSearchPaths(const std::vector<std::string>& paths);

    // ========================================================================
    // RHI CROSS-PLATFORM COMPILATION
    // ========================================================================

    /**
     * @brief Compile a shader through the RHI abstraction layer
     *
     * Uses RHIFactory::CompileShader() for cross-platform compilation,
     * enabling HLSL->SPIR-V and HLSL->GLSL cross-compilation paths.
     *
     * @param sourceFile Path to the shader source file
     * @param type Shader type (vertex, pixel, etc.)
     * @param targetBackend Target graphics backend (Auto for current platform)
     * @return true on success
     */
    bool CompileWithRHI(const std::string& sourceFile, ShaderType type, int targetBackend = 0);

    // ========================================================================
    // LEGACY COMPATIBILITY (for existing code)
    // ========================================================================

    /**
     * @brief Legacy constant buffer update (for backward compatibility)
     * @param cb Legacy constant buffer structure
     */
    void UpdateConstantBuffer(const ConstantBuffer& cb);

    /**
     * @brief Static shader compilation utility (legacy support)
     */
    static HRESULT CompileShaderFromFile(const std::wstring& filename, const std::string& entryPoint,
                                         const std::string& shaderModel, ID3DBlob** blobOut);

    // ========================================================================
    // Phase O: ShaderVariantSystem accessor
    // ========================================================================

    /**
     * @brief Get the shader variant system (Phase O activation).
     *
     * The `ShaderVariantSystem` is lifecycle-owned by each `Shader`
     * instance and initialised from `Shader::Initialize`. It tracks
     * keyword declarations, keyword groups, per-variant preprocessor
     * define lists, and variant-stripping bookkeeping.
     *
     * Usage:
     *   auto& vs = shader.GetVariantSystem();
     *   vs.RegisterKeyword({"_NORMALMAP", "ENABLE_NORMALMAP",
     *                       Spark::Graphics::KeywordType::MultiCompile, false});
     *   vs.SetGlobalKeyword("_NORMALMAP", true);
     *   auto defines = vs.GetDefinesForVariant(vs.GetGlobalVariantKey());
     */
    Spark::Graphics::ShaderVariantSystem& GetVariantSystem() { return m_variantSystem; }
    const Spark::Graphics::ShaderVariantSystem& GetVariantSystem() const { return m_variantSystem; }

  private:
    /**
     * @brief Create constant buffers for the shader system
     */
    HRESULT CreateConstantBuffers();

    /**
     * @brief Compile shader from file with advanced options
     */
    HRESULT CompileShaderFromFileAdvanced(const std::wstring& filename, ShaderType type,
                                          const ShaderCompilationFlags& flags, ID3DBlob** blobOut);

    /**
     * @brief Create input layout from vertex shader
     */
    HRESULT CreateInputLayout(ID3DBlob* vertexShaderBlob, ID3D11InputLayout** inputLayout);

    /**
     * @brief Update shader file monitoring for hot reload
     */
    void UpdateFileMonitoring();

    /**
     * @brief Notify console of shader state changes
     */
    void NotifyStateChange();

    /**
     * @brief Thread-safe metrics access
     */
    ShaderMetrics GetMetricsThreadSafe() const;

    // DirectX resources
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;

    // Shader resources
    std::unique_ptr<VertexShaderResource> m_vertexShader;
    std::unique_ptr<PixelShaderResource> m_pixelShader;

    // Constant buffers
    ComPtr<ID3D11Buffer> m_perFrameBuffer;
    ComPtr<ID3D11Buffer> m_perObjectBuffer;
    ComPtr<ID3D11Buffer> m_perMaterialBuffer;
    ComPtr<ID3D11Buffer> m_lightingDataBuffer;
    ComPtr<ID3D11Buffer> m_postProcessingBuffer;

    // Shader management
    std::unordered_map<std::string, std::unique_ptr<ShaderResource>> m_shaderCache;
    std::vector<std::unique_ptr<ShaderResource>> m_shaderVariants;
    int m_activeVariant = 0;

    // File monitoring for hot reload
    std::vector<std::wstring> m_watchedFiles;
    std::vector<std::string> m_searchPaths;

    // Console integration
    mutable std::mutex m_metricsMutex;
    std::function<void()> m_stateCallback;
    mutable ShaderMetrics m_metrics;

    // Configuration
    ShaderCompilationFlags m_defaultFlags;
    bool m_hotReloadEnabled = false;
    bool m_validationEnabled = false;

    // Phase O: keyword / variant bookkeeping. Pure CPU — lives on every
    // platform's build and is initialised from both the Windows and
    // Linux paths of Shader::Initialize().
    Spark::Graphics::ShaderVariantSystem m_variantSystem;

    // Additional members for implementation
    std::vector<ShaderVariant> m_variants;         ///< Shader variants
    std::string m_filePath;                        ///< Current shader file path
    FILETIME m_lastModified = {};                  ///< Last modification time
    ShaderType m_type = ShaderType::VERTEX_SHADER; ///< Current shader type
    bool m_isCompiled = false;                     ///< Compilation status
    ID3D11DeviceChild* m_shader = nullptr;         ///< Generic shader interface
    Spark::LocalFileCache* m_fileCache = nullptr;  ///< Optional file cache for shader source reads

    /// Compiled shader source stored after RHI compilation. On OpenGL this is
    /// the GLSL text; on D3D11 the bytecodes live in the VertexShaderResource /
    /// PixelShaderResource ComPtrs above. Callers that build RHI pipeline states
    /// can retrieve these via GetCompiledVertexSource() / GetCompiledPixelSource().
    std::string m_compiledVertexSource;
    std::string m_compiledPixelSource;

    /// RHI shader objects and pipeline state (used on Linux / OpenGL path).
    /// Created lazily after both VS and PS are compiled; bound from SetShaders().
    Spark::RHI::IRHIDevice* m_rhiDevice = nullptr;
    std::unique_ptr<Spark::RHI::IRHIShader> m_rhiVertexShader;
    std::unique_ptr<Spark::RHI::IRHIShader> m_rhiPixelShader;
    std::unique_ptr<Spark::RHI::IRHIPipelineState> m_rhiPipeline;
    std::unique_ptr<Spark::RHI::IRHIBuffer> m_rhiPerFrameCB;
    std::unique_ptr<Spark::RHI::IRHIBuffer> m_rhiPerObjectCB;

    /// Create RHI shader objects from stored source and build pipeline state.
    void CreateRHIPipelineIfReady();

  public:
    /// @brief Get the compiled vertex shader source (GLSL on OpenGL, empty on D3D11).
    const std::string& GetCompiledVertexSource() const { return m_compiledVertexSource; }

    /// @brief Get the compiled pixel shader source (GLSL on OpenGL, empty on D3D11).
    const std::string& GetCompiledPixelSource() const { return m_compiledPixelSource; }

    /// @brief Get the RHI pipeline state (non-null when both VS and PS compiled on Linux).
    Spark::RHI::IRHIPipelineState* GetRHIPipelineState() const { return m_rhiPipeline.get(); }
};
