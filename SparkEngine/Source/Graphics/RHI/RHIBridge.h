/**
 * @file RHIBridge.h
 * @brief Integration bridge between the RHI abstraction and the existing GraphicsEngine
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides the glue layer that allows the legacy DirectX 11 GraphicsEngine to
 * operate through the new RHI abstraction, enabling transparent backend switching
 * between D3D11, Vulkan, and OpenGL without modifying existing rendering code.
 *
 * Usage:
 *   // At initialization (replaces direct D3D11 creation):
 *   RHIBridge bridge;
 *   bridge.Initialize(hwnd, GraphicsBackend::Vulkan);
 *
 *   // During rendering (existing code works through bridge):
 *   auto* cmd = bridge.GetCommandList();
 *   cmd->SetRenderTargets(&backBuffer, 1, depthBuffer);
 *   cmd->ClearRenderTarget(backBuffer, clearColor);
 *   // ... draw calls ...
 *   bridge.Present(true);
 */

#pragma once

#include "RHIDevice.h"
#include "RHIResources.h"
#include "RHITypes.h"
#include <memory>
#include <string>
#include <functional>
#include <unordered_map>

namespace Spark
{
    namespace RHI
    {

        // ============================================================================
        // SHADER CACHE
        // ============================================================================

        /**
 * @brief Cross-platform shader cache that loads the correct shader variant
 *        based on the active graphics backend.
 */
        class ShaderCache
        {
          public:
            struct ShaderEntry
            {
                std::string hlslPath;
                std::string glslPath;
                std::string spirvPath;
                std::string entryPoint;
                RHIShaderStage stage;
            };

            void RegisterShader(const std::string& name, const ShaderEntry& entry);
            IRHIShader* GetShader(const std::string& name, IRHIDevice* device);
            void Clear(IRHIDevice* device);
            void ReloadAll(IRHIDevice* device);

          private:
            std::unordered_map<std::string, ShaderEntry> m_entries;
            std::unordered_map<std::string, std::unique_ptr<IRHIShader>> m_loadedShaders;
        };

        // ============================================================================
        // RHI BRIDGE
        // ============================================================================

        class RHIBridge
        {
          public:
            RHIBridge();
            ~RHIBridge();

            /**
     * @brief Initialize the RHI with the specified backend
     * @param windowHandle Platform window handle (HWND on Windows)
     * @param width Initial window width
     * @param height Initial window height
     * @param backend Graphics backend to use (Auto = best available)
     * @param enableDebug Enable GPU debug/validation layers
     * @return true on success
     */
            bool Initialize(void* windowHandle, uint32_t width, uint32_t height,
                            GraphicsBackend backend = GraphicsBackend::Auto, bool enableDebug = false);

            /**
     * @brief Shutdown and release all RHI resources
     */
            void Shutdown();

            /**
     * @brief Resize the swap chain and related render targets
     */
            bool Resize(uint32_t width, uint32_t height);

            // ========================================================================
            // FRAME MANAGEMENT
            // ========================================================================

            void BeginFrame();
            void EndFrame();
            bool Present(bool vsync = true);

            // ========================================================================
            // DEVICE ACCESS
            // ========================================================================

            [[nodiscard]] IRHIDevice* GetDevice() const { return m_device.get(); }
            [[nodiscard]] IRHISwapChain* GetSwapChain() const { return m_swapChain.get(); }
            [[nodiscard]] IRHICommandList* GetCommandList() const;
            [[nodiscard]] IRHITexture* GetBackBuffer() const;
            [[nodiscard]] IRHITexture* GetDepthBuffer() const { return m_depthBuffer.get(); }
            GraphicsBackend GetActiveBackend() const;

            /** @brief True when running on NullRHIDevice (no GPU / headless). */
            bool IsHeadless() const { return m_headless; }

            // ========================================================================
            // RESOURCE CONVENIENCE METHODS
            // ========================================================================

            /**
     * @brief Create a vertex buffer from data
     */
            [[nodiscard]] std::unique_ptr<IRHIBuffer> CreateVertexBuffer(const void* data, uint64_t size,
                                                                         uint32_t stride);

            /**
     * @brief Create an index buffer from data
     */
            [[nodiscard]] std::unique_ptr<IRHIBuffer> CreateIndexBuffer(const void* data, uint64_t size,
                                                                        uint32_t stride = 4);

            /**
     * @brief Create a constant buffer
     */
            [[nodiscard]] std::unique_ptr<IRHIBuffer> CreateConstantBuffer(uint64_t size);

            /**
     * @brief Create a 2D texture with optional initial data
     */
            [[nodiscard]] std::unique_ptr<IRHITexture> CreateTexture2D(uint32_t width, uint32_t height,
                                                                       PixelFormat format, RHITextureUsage usage,
                                                                       const void* data = nullptr);

            /**
     * @brief Create a depth buffer
     */
            [[nodiscard]] std::unique_ptr<IRHITexture> CreateDepthBuffer(
                uint32_t width, uint32_t height, PixelFormat format = PixelFormat::D24_UNORM_S8_UINT);

            /**
     * @brief Create a render target texture
     */
            [[nodiscard]] std::unique_ptr<IRHITexture> CreateRenderTarget(
                uint32_t width, uint32_t height, PixelFormat format = PixelFormat::R8G8B8A8_UNORM);

            /**
     * @brief Create a sampler state with common presets
     */
            [[nodiscard]] std::unique_ptr<IRHISampler> CreateSamplerLinearWrap();
            [[nodiscard]] std::unique_ptr<IRHISampler> CreateSamplerLinearClamp();
            [[nodiscard]] std::unique_ptr<IRHISampler> CreateSamplerPointClamp();
            [[nodiscard]] std::unique_ptr<IRHISampler> CreateSamplerAnisotropic(uint32_t maxAnisotropy = 16);

            // ========================================================================
            // SHADER CACHE
            // ========================================================================

            ShaderCache& GetShaderCache() { return m_shaderCache; }

            /**
     * @brief Register a shader pair (HLSL + GLSL)
     */
            void RegisterShader(const std::string& name, RHIShaderStage stage, const std::string& hlslPath,
                                const std::string& glslPath, const std::string& spirvPath = "",
                                const std::string& entryPoint = "main");

            /**
     * @brief Get a loaded shader by name
     */
            [[nodiscard]] IRHIShader* GetShader(const std::string& name);

            // ========================================================================
            // CAPABILITIES & INFO
            // ========================================================================

            const RHIDeviceCapabilities& GetCapabilities() const;
            const RHIStatistics& GetFrameStatistics() const;
            std::string GetDeviceInfo() const;
            std::string GetBackendName() const;

            /**
     * @brief Check if a specific backend is available on this system
     */
            static bool IsBackendAvailable(GraphicsBackend backend);

            /**
     * @brief Get a list of all available backends
     */
            static std::vector<GraphicsBackend> GetAvailableBackends();

            /**
     * @brief Get the recommended backend for this system
     */
            static GraphicsBackend GetRecommendedBackend();

          private:
            GraphicsBackend SelectBestBackend() const;
            bool CreateDepthBufferInternal(uint32_t width, uint32_t height);

            std::unique_ptr<IRHIDevice> m_device;
            std::unique_ptr<IRHISwapChain> m_swapChain;
            std::unique_ptr<IRHITexture> m_depthBuffer;
            ShaderCache m_shaderCache;

            void* m_windowHandle = nullptr;
            uint32_t m_width = 0;
            uint32_t m_height = 0;
            bool m_initialized = false;
            bool m_headless = false;
        };

    } // namespace RHI
} // namespace Spark
