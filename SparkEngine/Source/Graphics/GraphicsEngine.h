/**
 * @file GraphicsEngine.h
 * @brief Advanced DirectX 11 graphics engine with AAA features and console integration
 * @author Spark Engine Team
 * @date 2025
 * 
 * This class manages the entire DirectX 11 rendering pipeline with advanced features
 * including PBR materials, deferred rendering, post-processing, temporal effects,
 * texture streaming, asset pipeline, physics integration, and comprehensive 
 * console integration for real-time graphics debugging.
 */

#pragma once

#include "../Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#include <wrl/client.h>
#include <d3d11_1.h>
#include <dxgi1_3.h>
#include <dxgidebug.h>
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include "../Core/framework.h"
#include "Shader.h" // ✅ ADD: Include for PerObjectConstants and PerFrameConstants
#include <functional>
#include <mutex>
#include <chrono>
#include <vector>
#include <unordered_map>
#include <memory>
#include <atomic> // **CRITICAL FIX: Added for atomic frame state**

using Microsoft::WRL::ComPtr;

// Forward declarations for engine systems
class RenderTarget;
class MaterialSystem;
class LightManager;
namespace Spark::Graphics
{
    class PostProcessingPipeline;
}
#include "TemporalEffects.h" // Full definition needed for TAASettings struct
class TextureSystem;
class LightingSystem;
class PostProcessingSystem;
class AssetPipeline;
class PhysicsSystem;
class GameObject;
class Shader;

/**
 * @brief Render path / pipeline types for advanced graphics
 *
 * Unified enum replacing the previous separate RenderingPipeline and RenderPath
 * enums which had identical values.
 */
enum class RenderPath
{
    Forward,     ///< Forward rendering
    Deferred,    ///< Deferred rendering
    ForwardPlus, ///< Forward+ (tiled forward) rendering
    Clustered    ///< Clustered rendering
};

/// @brief Legacy alias - use RenderPath directly for new code
using RenderingPipeline = RenderPath;

/**
 * @brief Rendering quality presets
 */
enum class QualityPreset
{
    Low,    ///< Low quality (mobile/integrated graphics)
    Medium, ///< Medium quality (mid-range hardware)
    High,   ///< High quality (high-end hardware)
    Ultra,  ///< Ultra quality (enthusiast hardware)
    Custom  ///< Custom quality settings
};

/**
 * @brief Multi-sampling anti-aliasing settings
 */
enum class MSAALevel
{
    None = 1,   ///< No MSAA
    MSAA2x = 2, ///< 2x MSAA
    MSAA4x = 4, ///< 4x MSAA
    MSAA8x = 8  ///< 8x MSAA
};

// TAASettings is defined in TemporalEffects.h (included above)

/**
 * @brief Screen-space ambient occlusion settings
 */
struct SSAOSettings
{
    bool enabled = false;   ///< Enable SSAO
    float radius = 0.5f;    ///< SSAO sampling radius
    float intensity = 1.0f; ///< SSAO intensity
    int sampleCount = 16;   ///< Number of SSAO samples
    float bias = 0.025f;    ///< SSAO bias to prevent self-occlusion
    bool blur = true;       ///< Enable SSAO blur
};

/**
 * @brief Screen-space reflection settings
 */
struct SSRSettings
{
    bool enabled = false;       ///< Enable SSR
    float maxDistance = 100.0f; ///< Maximum reflection distance
    int maxSteps = 32;          ///< Maximum ray marching steps
    float thickness = 0.5f;     ///< Surface thickness for intersection
    float fadeStart = 80.0f;    ///< Distance to start fading reflections
    float fadeEnd = 100.0f;     ///< Distance to fully fade reflections
};

/**
 * @brief Volumetric lighting settings
 */
struct VolumetricSettings
{
    bool enabled = false;     ///< Enable volumetric lighting
    int sampleCount = 32;     ///< Number of volumetric samples
    float scattering = 0.1f;  ///< Light scattering factor
    float extinction = 0.01f; ///< Light extinction factor
    float anisotropy = 0.3f;  ///< Phase function anisotropy
};

/**
 * @brief Advanced graphics settings
 */
struct GraphicsSettings
{
    // Rendering
    RenderPath renderPath = RenderPath::Deferred;
    QualityPreset qualityPreset = QualityPreset::High;
    bool vsync = true;
    bool hdr = true;
    uint32_t msaaSamples = 4;

    // Textures
    uint32_t maxTextureSize = 2048;
    bool anisotropicFiltering = true;
    uint32_t anisotropyLevel = 16;

    // Shadows
    bool shadows = true;
    uint32_t shadowMapSize = 2048;
    uint32_t cascadeCount = 3;

    // Post-processing
    bool bloom = true;
    bool ssao = false;
    bool taa = false;
    bool motionBlur = false;

    // Performance
    bool frustumCulling = true;
    bool occlusionCulling = false;
    bool levelOfDetail = true;
    uint32_t maxDrawCalls = 1000;

    // Legacy compatibility
    bool wireframeMode = false;
    bool debugMode = false;
    bool showFPS = false;
    float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float renderScale = 1.0f;
    bool enableGPUTiming = false;
};

/**
 * @brief Comprehensive render statistics
 */
struct RenderStatistics
{
    // Performance
    float frameTime = 0.0f; ///< Total frame time (ms)
    float cpuTime = 0.0f;   ///< CPU time (ms)
    float gpuTime = 0.0f;   ///< GPU time (ms)
    uint32_t fps = 0;       ///< Frames per second

    // Rendering
    uint32_t drawCalls = 0;        ///< Draw calls per frame
    uint32_t triangles = 0;        ///< Triangles rendered
    uint32_t vertices = 0;         ///< Vertices processed
    uint32_t textureBinds = 0;     ///< Texture binds per frame
    uint32_t materialSwitches = 0; ///< Material switches per frame

    // Culling
    uint32_t totalObjects = 0;   ///< Total objects in scene
    uint32_t visibleObjects = 0; ///< Objects after culling
    uint32_t culledObjects = 0;  ///< Objects culled
    float cullingTime = 0.0f;    ///< Culling time (ms)

    // Memory
    size_t textureMemory = 0;  ///< Texture memory usage (bytes)
    size_t meshMemory = 0;     ///< Mesh memory usage (bytes)
    size_t totalGPUMemory = 0; ///< Total GPU memory usage (bytes)

    // Lighting
    uint32_t activeLights = 0;     ///< Active lights
    uint32_t shadowUpdates = 0;    ///< Shadow map updates
    float lightCullingTime = 0.0f; ///< Light culling time (ms)

    // Post-processing
    float postProcessTime = 0.0f;   ///< Post-processing time (ms)
    uint32_t postProcessPasses = 0; ///< Post-processing passes

    // Legacy compatibility
    float renderTime = 0.0f;    ///< Legacy render time
    float presentTime = 0.0f;   ///< Legacy present time
    size_t bufferMemory = 0;    ///< Legacy buffer memory
    float gpuUsage = 0.0f;      ///< Legacy GPU usage
    bool vsyncEnabled = false;  ///< Legacy VSync state
    bool wireframeMode = false; ///< Legacy wireframe state
    bool debugMode = false;     ///< Legacy debug state
};

/**
 * @brief Advanced DirectX 11 graphics engine with AAA features
 * 
 * Comprehensive graphics engine supporting modern rendering techniques,
 * multiple rendering pipelines, advanced post-processing, and extensive
 * console integration for real-time debugging and performance tuning.
 * 
 * Integrates all advanced systems:
 * - TextureSystem for advanced texture management and streaming
 * - MaterialSystem for PBR materials and shader management
 * - LightingSystem for advanced lighting and shadow mapping
 * - PostProcessingSystem for HDR and visual effects
 * - AssetPipeline for model loading and asset streaming
 * - PhysicsSystem for physics simulation integration
 */
class GraphicsEngine
{
  public:
    /**
     * @brief Default constructor
     */
    GraphicsEngine();

    /**
     * @brief Destructor
     */
    ~GraphicsEngine();

    /**
     * @brief Initialize the graphics engine with advanced features
     * @param hWnd Handle to the window for rendering
     * @return HRESULT indicating success or failure of initialization
     */
    HRESULT Initialize(Spark::NativeWindowHandle hWnd);

    /**
     * @brief Clean up all DirectX resources
     */
    void Shutdown();

    /**
     * @brief Resize the rendering surface
     * @param width New width
     * @param height New height
     */
    HRESULT Resize(uint32_t width, uint32_t height);

    /**
     * @brief Begin a new rendering frame with advanced setup
     */
    void BeginFrame();

    /**
     * @brief Complete the current frame and present to screen
     */
    void EndFrame();

    /**
     * @brief Render the scene with advanced pipeline
     * @param viewMatrix View transformation matrix
     * @param projMatrix Projection transformation matrix
     * @param objects List of objects to render
     */
    void RenderScene(const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix, const std::vector<GameObject*>& objects);

    /**
     * @brief Handle window resize events with advanced buffer management
     * @param width New window width in pixels
     * @param height New window height in pixels
     */
    void OnResize(unsigned int width, unsigned int height);

    // ========================================================================
    // ADVANCED SYSTEM ACCESSORS
    // ========================================================================

    TextureSystem* GetTextureSystem() const;
    MaterialSystem* GetMaterialSystem() const;
    LightingSystem* GetLightingSystem() const;
    PostProcessingSystem* GetPostProcessingSystem() const;
    AssetPipeline* GetAssetPipeline() const;

    /** @deprecated Physics is no longer owned by GraphicsEngine. Use EngineContext::GetPhysics(). */
    [[deprecated("Use EngineContext::GetPhysics() instead")]]
    PhysicsSystem* GetPhysicsSystem() const;

    /** @brief Set non-owning physics pointer (called by engine during init) */
    void SetPhysicsSystem(PhysicsSystem* physics) { m_physicsSystem = physics; }

    // Legacy compatibility accessors
    LightManager* GetLightManager() const;

    // ========================================================================
    // ADVANCED RENDERING FEATURES
    // ========================================================================

    /**
     * @brief Set the active rendering pipeline
     * @param pipeline Rendering pipeline type
     */
    void SetRenderingPipeline(RenderingPipeline pipeline);

    /**
     * @brief Get the current rendering pipeline
     */
    RenderingPipeline GetRenderingPipeline() const;

    /**
     * @brief Set graphics settings
     */
    void SetGraphicsSettings(const GraphicsSettings& settings);
    const GraphicsSettings& GetGraphicsSettings() const;

    /**
     * @brief Set quality preset
     */
    void SetQualityPreset(QualityPreset preset);

    /**
     * @brief Set render path
     */
    void SetRenderPath(RenderPath path);

    /**
     * @brief Enable/disable HDR rendering
     * @param enabled HDR state
     */
    void SetHDREnabled(bool enabled);

    /**
     * @brief Set multi-sampling anti-aliasing level
     * @param msaaLevel MSAA level
     */
    void SetMSAALevel(MSAALevel msaaLevel);

    /**
     * @brief Configure temporal anti-aliasing
     * @param settings TAA settings
     */
    void SetTAASettings(const TAASettings& settings);

    /**
     * @brief Configure screen-space ambient occlusion
     * @param settings SSAO settings
     */
    void SetSSAOSettings(const SSAOSettings& settings);

    /**
     * @brief Configure screen-space reflections
     * @param settings SSR settings
     */
    void SetSSRSettings(const SSRSettings& settings);

    /**
     * @brief Configure volumetric lighting
     * @param settings Volumetric lighting settings
     */
    void SetVolumetricSettings(const VolumetricSettings& settings);

    // ========================================================================
    // RESOURCE ACCESS
    // ========================================================================

    /**
     * @brief Get the DirectX 11 device
     */
    ID3D11Device* GetDevice() const;

    /**
     * @brief Get the DirectX 11 device context
     */
    ID3D11DeviceContext* GetContext() const;

    /**
     * @brief Get the current window width
     */
    UINT GetWindowWidth() const;

    /**
     * @brief Get the current window height
     */
    UINT GetWindowHeight() const;

    /**
     * @brief Get the DXGI swap chain
     */
    IDXGISwapChain* GetSwapChain() const;

    /**
     * @brief Get render target views
     */
    ID3D11RenderTargetView* GetBackBufferRTV() const;
    ID3D11DepthStencilView* GetDepthStencilView() const;

    // ========================================================================
    // STATISTICS AND PERFORMANCE
    // ========================================================================

    /**
     * @brief Get render statistics
     */
    const RenderStatistics& GetStatistics() const;
    void ResetStatistics();

    /**
     * @brief Save screenshot
     */
    HRESULT SaveScreenshot(const std::string& filename);

    // ========================================================================
    // CONSOLE INTEGRATION METHODS
    // Console commands are registered via GraphicsConsoleCommands.h.
    // These methods implement the underlying operations.
    // ========================================================================

    RenderStatistics Console_GetStatistics() const;
    void Console_SetQuality(const std::string& preset);
    void Console_SetRenderPath(const std::string& path);
    void Console_EnableFeature(const std::string& feature, bool enabled);
    void Console_SetSetting(const std::string& setting, float value);
    void Console_ReloadShaders();
    bool Console_Screenshot(const std::string& filename);
    std::string Console_GetSystemInfo() const;
    std::string Console_Benchmark(int seconds = 10);
    void Console_SetWireframe(bool enabled);
    void Console_SetVSync(bool enabled);
    void Console_SetRenderingPipeline(RenderingPipeline pipeline);
    void Console_SetHDR(bool enabled);
    void Console_SetDebugMode(bool enabled);
    void Console_SetClearColor(float r, float g, float b, float a);
    void Console_SetRenderScale(float scale);
    void Console_ResetDevice();
    void Console_ForceGarbageCollection();
    void Console_ApplySettings(const GraphicsSettings& settings);
    void Console_ResetToDefaults();
    void Console_RegisterStateCallback(std::function<void()> callback);
    void Console_SetGPUTiming(bool enabled);
    size_t Console_GetVRAMUsage() const;

    /** @deprecated Use Console_SetWireframe() */
    [[deprecated("Use Console_SetWireframe()")]]
    void Console_SetWireframeMode(bool enabled);
    /** @deprecated Use Console_Screenshot() */
    [[deprecated("Use Console_Screenshot()")]]
    bool Console_TakeScreenshot(const std::string& filename = "");
    /** @deprecated Use GetGraphicsSettings() */
    [[deprecated("Use GetGraphicsSettings()")]]
    GraphicsSettings Console_GetSettings() const;
    /** @deprecated Use Console_GetStatistics() */
    [[deprecated("Use Console_GetStatistics()")]]
    RenderStatistics Console_GetMetrics() const;

    // ========================================================================
    // BASIC SHADER SYSTEM (for rendering pipeline)
    // ========================================================================

    /**
     * @brief Set basic shaders for rendering
     */
    void SetBasicShaders();

    /**
     * @brief Update basic constant buffer with transformation matrices
     * @param world World transformation matrix
     * @param view View transformation matrix  
     * @param proj Projection transformation matrix
     */
    void UpdateBasicConstants(const DirectX::XMMATRIX& world, const DirectX::XMMATRIX& view,
                              const DirectX::XMMATRIX& proj);

    /**
     * @brief Update per-frame constants for lighting and camera
     * @param view View transformation matrix
     * @param proj Projection transformation matrix
     * @param cameraPos Camera position in world space
     */
    void UpdateFrameConstants(const XMMATRIX& view, const XMMATRIX& proj, const XMFLOAT3& cameraPos);

  private:
    // ========================================================================
    // ADVANCED RENDERING SUBSYSTEMS
    // ========================================================================

    std::unique_ptr<TextureSystem> m_textureSystem;
    std::unique_ptr<MaterialSystem> m_materialSystem;
    std::unique_ptr<LightingSystem> m_lightingSystem;
    std::unique_ptr<PostProcessingSystem> m_postProcessingSystem;
    std::unique_ptr<AssetPipeline> m_assetPipeline;
    // Non-owning: PhysicsSystem lifetime managed by SparkEngine.cpp / EngineContext
    PhysicsSystem* m_physicsSystem = nullptr;

    // Shader system
    std::unique_ptr<class Shader> m_shader;

    // Legacy rendering subsystems
    std::unique_ptr<LightManager> m_lightManager;
    std::unique_ptr<Spark::Graphics::PostProcessingPipeline> m_postProcessing;
    std::unique_ptr<TemporalEffects> m_temporalEffects;

    // ========================================================================
    // DIRECTX RESOURCES
    // ========================================================================

    ComPtr<ID3D11Device1> m_device;
    ComPtr<ID3D11DeviceContext1> m_context;
    ComPtr<IDXGISwapChain1> m_swapChain;
    ComPtr<ID3D11RenderTargetView> m_renderTargetView;
    ComPtr<ID3D11DepthStencilView> m_depthStencilView;

    // Advanced render targets for deferred/forward+ rendering
    ComPtr<ID3D11Texture2D> m_gBufferTextures[4]; // Albedo, Normal, Material, Motion
    ComPtr<ID3D11RenderTargetView> m_gBufferRTVs[4];
    ComPtr<ID3D11ShaderResourceView> m_gBufferSRVs[4];
    ComPtr<ID3D11Texture2D> m_hdrTexture;
    ComPtr<ID3D11RenderTargetView> m_hdrRTV;
    ComPtr<ID3D11ShaderResourceView> m_hdrSRV;
    ComPtr<ID3D11Texture2D> m_depthStencilTexture;

    // Enhanced render targets for deferred rendering
    std::unordered_map<std::string, std::unique_ptr<RenderTarget>> m_renderTargets;

    // ========================================================================
    // RENDERING STATE
    // ========================================================================

    RenderingPipeline m_currentPipeline;
    GraphicsSettings m_settings;
    RenderStatistics m_statistics;
    uint32_t m_width;
    uint32_t m_height;
    bool m_fullscreen;
    Spark::NativeWindowHandle m_hwnd;

    // Advanced settings
    bool m_hdrEnabled;
    MSAALevel m_msaaLevel;
    TAASettings m_taaSettings;
    SSAOSettings m_ssaoSettings;
    SSRSettings m_ssrSettings;
    VolumetricSettings m_volumetricSettings;

    // Window dimensions
    UINT m_windowWidth;
    UINT m_windowHeight;

    // ========================================================================
    // PERFORMANCE TRACKING
    // ========================================================================

    std::chrono::high_resolution_clock::time_point m_frameStartTime;
    std::chrono::high_resolution_clock::time_point m_renderStartTime;
    std::chrono::high_resolution_clock::time_point m_geometryStartTime;
    std::chrono::high_resolution_clock::time_point m_lightingStartTime;
    std::chrono::high_resolution_clock::time_point m_postProcessStartTime;

    ComPtr<ID3D11Query> m_disjointQuery;
    ComPtr<ID3D11Query> m_timestampStartQuery;
    ComPtr<ID3D11Query> m_timestampEndQuery;
    ComPtr<ID3D11Query> m_gpuTimingQuery;
    ComPtr<ID3D11Query> m_timestampBegin;
    ComPtr<ID3D11Query> m_timestampEnd;
    ComPtr<ID3D11Query> m_timestampDisjoint;

    // ========================================================================
    // RENDER STATE OBJECTS
    // ========================================================================

    ComPtr<ID3D11RasterizerState> m_defaultRasterState;
    ComPtr<ID3D11RasterizerState> m_wireframeRasterState;
    ComPtr<ID3D11RasterizerState> m_solidRasterState;
    ComPtr<ID3D11DepthStencilState> m_defaultDepthState;
    ComPtr<ID3D11BlendState> m_defaultBlendState;

    // ========================================================================
    // THREADING AND STATE MANAGEMENT
    // ========================================================================

    mutable std::mutex m_metricsMutex;
    std::function<void()> m_stateCallback;

    // **CRITICAL FIX: Added atomic frame state for thread-safe frame management**
    std::atomic<bool> m_frameInProgress;

    // Resource tracking
    size_t m_textureMemoryUsage;
    size_t m_bufferMemoryUsage;

    // ✅ ADD: Basic shader system resources
    ComPtr<ID3D11VertexShader> m_basicVertexShader;
    ComPtr<ID3D11PixelShader> m_basicPixelShader;
    ComPtr<ID3D11InputLayout> m_basicInputLayout;
    ComPtr<ID3D11Buffer> m_basicConstantBuffer;
    ComPtr<ID3D11Buffer> m_basicFrameConstantBuffer; // ✅ ADD: Per-frame constant buffer
    ComPtr<ID3D11SamplerState> m_basicSamplerState;
    ComPtr<ID3D11Texture2D> m_defaultTexture;      // ✅ ADD: Default white texture
    ComPtr<ID3D11ShaderResourceView> m_defaultSRV; // ✅ ADD: Default texture SRV

    // ========================================================================
    // PRIVATE METHODS
    // ========================================================================

    HRESULT CreateDeviceAndSwapChain(HWND hWnd);
    HRESULT CreateDevice(HWND hwnd, uint32_t width, uint32_t height, bool fullscreen);
    HRESULT CreateRenderTargetView();
    HRESULT CreateDepthStencilView();
    HRESULT CreateRenderTargets();
    HRESULT CreateAdvancedRenderTargets();
    HRESULT CreateRenderStates();
    void SetViewport();
    void UpdateMetrics();
    void UpdateAdvancedMetrics();
    void ApplyGraphicsState();
    void ApplyAdvancedGraphicsState();
    void ApplyQualityPreset(QualityPreset preset);
    void NotifyStateChange();
    void SetupDeferredPipeline();
    void SetupForwardPlusPipeline();

    // Advanced rendering methods
    void RenderForward(const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix, const std::vector<GameObject*>& objects);
    void RenderDeferred(const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix,
                        const std::vector<GameObject*>& objects);
    void RenderForwardPlus(const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix,
                           const std::vector<GameObject*>& objects);
    void FillGBuffer(const std::vector<GameObject*>& objects, const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix);
    void LightingPass(const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix);
    void CullObjects(const std::vector<GameObject*>& objects, const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix,
                     std::vector<GameObject*>& visibleObjects);
    void RenderGeometryPass();
    void RenderLightingPass();
    void RenderPostProcessing();
    void RenderTemporalEffects();

    // ✅ ADD: Basic shader system methods
    HRESULT InitializeBasicShaders();
    HRESULT CompileShaderFromFile(const std::wstring& filename, const char* entryPoint, const char* shaderModel,
                                  ID3DBlob** blobOut);
    HRESULT CreateBasicConstantBuffer();
    HRESULT CreateDefaultTexture();                          // ✅ ADD: Default texture creation
    HRESULT CompileEmbeddedVertexShader(ID3DBlob** blobOut); // ✅ ADD: Embedded vertex shader
    HRESULT CompileEmbeddedPixelShader(ID3DBlob** blobOut);  // ✅ ADD: Embedded pixel shader
};
