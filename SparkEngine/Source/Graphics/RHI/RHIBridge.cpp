/**
 * @file RHIBridge.cpp
 * @brief Implementation of the RHI bridge layer
 * @author Spark Engine Team
 * @date 2025
 */

#include "RHIBridge.h"
#include "RHIFactory.h"
#include "NullRHIDevice.h"
#include "../../Utils/ContainerUtils.h"
#include "../../Utils/Validate.h"
#include <cassert>
#include <fstream>
#include <algorithm>

namespace Spark
{
    namespace RHI
    {

        // ============================================================================
        // SHADER CACHE
        // ============================================================================

        void ShaderCache::RegisterShader(const std::string& name, const ShaderEntry& entry)
        {
            m_entries[name] = entry;
        }

        IRHIShader* ShaderCache::GetShader(const std::string& name, IRHIDevice* device)
        {
            auto cached = m_loadedShaders.find(name);
            if (cached != m_loadedShaders.end())
                return cached->second.get();

            auto entry = m_entries.find(name);
            if (entry == m_entries.end())
                return nullptr;

            const auto& e = entry->second;
            RHIShaderDesc desc;
            desc.stage = e.stage;
            desc.entryPoint = e.entryPoint;
            desc.debugName = name;

            // Select shader source based on backend
            GraphicsBackend backend = device->GetBackendType();
            std::string filePath;

            switch (backend)
            {
            case GraphicsBackend::D3D11:
            case GraphicsBackend::D3D12:
                filePath = e.hlslPath;
                desc.language = ShaderLanguage::HLSL;
                break;
            case GraphicsBackend::Vulkan:
                filePath = !e.spirvPath.empty() ? e.spirvPath : e.glslPath;
                desc.language = !e.spirvPath.empty() ? ShaderLanguage::SPIRV : ShaderLanguage::GLSL;
                break;
            case GraphicsBackend::OpenGL:
                filePath = e.glslPath;
                desc.language = ShaderLanguage::GLSL;
                break;
            default:
                return nullptr;
            }

            if (filePath.empty())
                return nullptr;

            // Load shader source
            if (desc.language == ShaderLanguage::SPIRV)
            {
                std::ifstream file(filePath, std::ios::binary | std::ios::ate);
                if (!file.is_open())
                    return nullptr;

                size_t size = file.tellg();
                file.seekg(0);
                std::vector<uint8_t> buffer(size);
                file.read(reinterpret_cast<char*>(buffer.data()), size);
                desc.bytecode = buffer.data();
                desc.bytecodeSize = size;

                auto shader = device->CreateShader(desc);
                IRHIShader* raw = shader.get();
                if (shader)
                    m_loadedShaders[name] = std::move(shader);
                return raw;
            }
            else
            {
                std::ifstream file(filePath);
                if (!file.is_open())
                    return nullptr;

                desc.sourceCode = std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
                desc.filePath = filePath;

                auto shader = device->CreateShader(desc);
                IRHIShader* raw = shader.get();
                if (shader)
                    m_loadedShaders[name] = std::move(shader);
                return raw;
            }
        }

        // Intentional: device reserved for GPU resource cleanup in backends that need it
        void ShaderCache::Clear([[maybe_unused]] IRHIDevice* device)
        {
            m_loadedShaders.clear();
        }

        void ShaderCache::ReloadAll(IRHIDevice* device)
        {
            // Destroy existing shaders
            Clear(device);

            // Reload all registered shaders
            for (const auto& [name, entry] : m_entries)
            {
                GetShader(name, device);
            }
        }

        // ============================================================================
        // RHI BRIDGE
        // ============================================================================

        RHIBridge::RHIBridge() = default;

        RHIBridge::~RHIBridge()
        {
            Shutdown();
        }

        bool RHIBridge::Initialize(void* windowHandle, uint32_t width, uint32_t height, GraphicsBackend backend,
                                   bool enableDebug)
        {
            SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
            SPARK_LOG_INFO(Spark::LogCategory::Graphics, "RHIBridge::Initialize %ux%u", width, height);
            if (m_initialized)
                Shutdown();

            m_windowHandle = windowHandle;
            m_width = width;
            m_height = height;
            m_headless = false;

            // Auto-select backend if requested
            if (backend == GraphicsBackend::Auto)
                backend = SelectBestBackend();

            // Try the preferred backend first, then fall back to alternatives.
            // This handles the common case where Vulkan is preferred on Linux but
            // no Vulkan driver is present — the engine falls back to OpenGL.
            auto backendsToTry = GetAvailableBackends();

            // Move the preferred backend to the front of the list
            auto it = std::find(backendsToTry.begin(), backendsToTry.end(), backend);
            if (it != backendsToTry.end() && it != backendsToTry.begin())
                std::rotate(backendsToTry.begin(), it, it + 1);
            else if (it == backendsToTry.end())
                backendsToTry.insert(backendsToTry.begin(), backend);

            bool deviceReady = false;
            for (auto candidate : backendsToTry)
            {
                m_device = CreateDevice(candidate);
                if (!m_device)
                    continue;

                RHIDeviceDesc deviceDesc;
                deviceDesc.preferredBackend = candidate;
                deviceDesc.enableDebugLayer = enableDebug;
                deviceDesc.enableGPUValidation = enableDebug;
                deviceDesc.applicationName = "SparkEngine";

                if (m_device->Initialize(deviceDesc))
                {
                    if (candidate != backend)
                    {
                        SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                                       "Preferred backend '%s' unavailable — fell back to '%s'",
                                       Spark::RHI::GetBackendName(backend), Spark::RHI::GetBackendName(candidate));
                    }
                    deviceReady = true;
                    break;
                }

                SPARK_LOG_WARN(Spark::LogCategory::Graphics, "Backend '%s' failed to initialize — trying next",
                               Spark::RHI::GetBackendName(candidate));
                m_device.reset();
            }

            if (!deviceReady)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                               "All GPU backends failed — falling back to NullRHIDevice (headless)");
                m_device = std::make_unique<NullRHIDevice>();
                RHIDeviceDesc nullDesc;
                nullDesc.preferredBackend = GraphicsBackend::None;
                nullDesc.applicationName = "SparkEngine";
                m_device->Initialize(nullDesc);
            }

            // Headless path: NullRHIDevice can't create a swap chain or depth buffer,
            // but the device itself is valid for non-rendering work (ECS, physics, etc.)
            if (m_device->GetBackendType() == GraphicsBackend::None)
            {
                m_headless = true;
                SPARK_LOG_INFO(Spark::LogCategory::Graphics,
                               "RHIBridge initialized in headless mode (NullRHIDevice) — no swap chain or "
                               "depth buffer");
                m_initialized = true;
                return true;
            }

            // Create swap chain
            RHISwapChainDesc swapDesc;
            swapDesc.windowHandle = windowHandle;
            swapDesc.width = width;
            swapDesc.height = height;
            swapDesc.format = PixelFormat::R8G8B8A8_UNORM;
            swapDesc.bufferCount = 2;
            swapDesc.fullscreen = false;
            swapDesc.vsync = true;

            m_swapChain = m_device->CreateSwapChain(swapDesc);
            if (!m_swapChain)
            {
                m_device->Shutdown();
                m_device.reset();
                return false;
            }

            // Create depth buffer
            if (!CreateDepthBufferInternal(width, height))
            {
                m_swapChain.reset();
                m_device->Shutdown();
                m_device.reset();
                return false;
            }

            m_initialized = true;
            return true;
        }

        void RHIBridge::Shutdown()
        {
            SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
            SPARK_LOG_INFO(Spark::LogCategory::Graphics, "RHIBridge::Shutdown");
            if (!m_initialized)
                return;

            m_shaderCache.Clear(m_device.get());

            m_depthBuffer.reset();

            m_swapChain.reset();

            if (m_device)
            {
                m_device->WaitForIdle();
                m_device->Shutdown();
                m_device.reset();
            }

            m_initialized = false;
        }

        bool RHIBridge::Resize(uint32_t width, uint32_t height)
        {
            if (!m_initialized)
                return false;
            if (width == 0 || height == 0)
                return false;

            if (m_device)
                m_device->WaitForIdle();

            // Release old depth buffer
            m_depthBuffer.reset();

            // Headless mode has no swap chain or depth buffer; track the new
            // size but skip the GPU resource recreation.
            if (!m_swapChain)
            {
                m_width = width;
                m_height = height;
                return true;
            }

            // Resize swap chain
            if (!m_swapChain->Resize(width, height))
                return false;

            m_width = width;
            m_height = height;

            // Recreate depth buffer
            return CreateDepthBufferInternal(width, height);
        }

        void RHIBridge::BeginFrame()
        {
            if (m_device)
                m_device->BeginFrame();
        }

        void RHIBridge::EndFrame()
        {
            if (m_device)
                m_device->EndFrame();
        }

        bool RHIBridge::Present(bool vsync)
        {
            return m_swapChain ? m_swapChain->Present(vsync) : false;
        }

        IRHICommandList* RHIBridge::GetCommandList() const
        {
            return m_device ? m_device->GetImmediateCommandList() : nullptr;
        }

        IRHITexture* RHIBridge::GetBackBuffer() const
        {
            return m_swapChain ? m_swapChain->GetBackBuffer() : nullptr;
        }

        GraphicsBackend RHIBridge::GetActiveBackend() const
        {
            return m_device ? m_device->GetBackendType() : GraphicsBackend::Auto;
        }

        // ============================================================================
        // RESOURCE CONVENIENCE METHODS
        // ============================================================================

        std::unique_ptr<IRHIBuffer> RHIBridge::CreateVertexBuffer(const void* data, uint64_t size, uint32_t stride)
        {
            RHIBufferDesc desc;
            desc.size = size;
            desc.stride = stride;
            desc.usage = RHIBufferUsage::Vertex;
            desc.access = data ? RHIBufferAccess::Static : RHIBufferAccess::Dynamic;
            desc.initialData = data;
            return m_device->CreateBuffer(desc);
        }

        std::unique_ptr<IRHIBuffer> RHIBridge::CreateIndexBuffer(const void* data, uint64_t size, uint32_t stride)
        {
            RHIBufferDesc desc;
            desc.size = size;
            desc.stride = stride;
            desc.usage = RHIBufferUsage::Index;
            desc.access = data ? RHIBufferAccess::Static : RHIBufferAccess::Dynamic;
            desc.initialData = data;
            return m_device->CreateBuffer(desc);
        }

        std::unique_ptr<IRHIBuffer> RHIBridge::CreateConstantBuffer(uint64_t size)
        {
            RHIBufferDesc desc;
            desc.size = size;
            desc.usage = RHIBufferUsage::Constant;
            desc.access = RHIBufferAccess::Dynamic;
            return m_device->CreateBuffer(desc);
        }

        std::unique_ptr<IRHITexture> RHIBridge::CreateTexture2D(uint32_t width, uint32_t height, PixelFormat format,
                                                                RHITextureUsage usage, const void* data)
        {
            RHITextureDesc desc;
            desc.width = width;
            desc.height = height;
            desc.format = format;
            desc.usage = usage;
            desc.mipLevels = 1;
            return m_device->CreateTexture(desc);
        }

        std::unique_ptr<IRHITexture> RHIBridge::CreateDepthBuffer(uint32_t width, uint32_t height, PixelFormat format)
        {
            RHITextureDesc desc;
            desc.width = width;
            desc.height = height;
            desc.format = format;
            desc.usage = RHITextureUsage::DepthStencil;
            desc.mipLevels = 1;
            return m_device->CreateTexture(desc);
        }

        std::unique_ptr<IRHITexture> RHIBridge::CreateRenderTarget(uint32_t width, uint32_t height, PixelFormat format)
        {
            RHITextureDesc desc;
            desc.width = width;
            desc.height = height;
            desc.format = format;
            desc.usage = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource;
            desc.mipLevels = 1;
            return m_device->CreateTexture(desc);
        }

        std::unique_ptr<IRHISampler> RHIBridge::CreateSamplerLinearWrap()
        {
            RHISamplerDesc desc;
            desc.minFilter = RHIFilterMode::Linear;
            desc.magFilter = RHIFilterMode::Linear;
            desc.mipFilter = RHIFilterMode::Linear;
            desc.addressU = RHIAddressMode::Wrap;
            desc.addressV = RHIAddressMode::Wrap;
            desc.addressW = RHIAddressMode::Wrap;
            return m_device->CreateSampler(desc);
        }

        std::unique_ptr<IRHISampler> RHIBridge::CreateSamplerLinearClamp()
        {
            RHISamplerDesc desc;
            desc.minFilter = RHIFilterMode::Linear;
            desc.magFilter = RHIFilterMode::Linear;
            desc.mipFilter = RHIFilterMode::Linear;
            desc.addressU = RHIAddressMode::Clamp;
            desc.addressV = RHIAddressMode::Clamp;
            desc.addressW = RHIAddressMode::Clamp;
            return m_device->CreateSampler(desc);
        }

        std::unique_ptr<IRHISampler> RHIBridge::CreateSamplerPointClamp()
        {
            RHISamplerDesc desc;
            desc.minFilter = RHIFilterMode::Nearest;
            desc.magFilter = RHIFilterMode::Nearest;
            desc.mipFilter = RHIFilterMode::Nearest;
            desc.addressU = RHIAddressMode::Clamp;
            desc.addressV = RHIAddressMode::Clamp;
            desc.addressW = RHIAddressMode::Clamp;
            return m_device->CreateSampler(desc);
        }

        std::unique_ptr<IRHISampler> RHIBridge::CreateSamplerAnisotropic(uint32_t maxAnisotropy)
        {
            RHISamplerDesc desc;
            desc.minFilter = RHIFilterMode::Anisotropic;
            desc.magFilter = RHIFilterMode::Anisotropic;
            desc.mipFilter = RHIFilterMode::Linear;
            desc.addressU = RHIAddressMode::Wrap;
            desc.addressV = RHIAddressMode::Wrap;
            desc.addressW = RHIAddressMode::Wrap;
            desc.maxAnisotropy = maxAnisotropy;
            return m_device->CreateSampler(desc);
        }

        // ============================================================================
        // SHADER MANAGEMENT
        // ============================================================================

        void RHIBridge::RegisterShader(const std::string& name, RHIShaderStage stage, const std::string& hlslPath,
                                       const std::string& glslPath, const std::string& spirvPath,
                                       const std::string& entryPoint)
        {
            ShaderCache::ShaderEntry entry;
            entry.hlslPath = hlslPath;
            entry.glslPath = glslPath;
            entry.spirvPath = spirvPath;
            entry.entryPoint = entryPoint;
            entry.stage = stage;
            m_shaderCache.RegisterShader(name, entry);
        }

        IRHIShader* RHIBridge::GetShader(const std::string& name)
        {
            return m_shaderCache.GetShader(name, m_device.get());
        }

        // ============================================================================
        // CAPABILITIES & INFO
        // ============================================================================

        const RHIDeviceCapabilities& RHIBridge::GetCapabilities() const
        {
            return m_device->GetCapabilities();
        }

        const RHIStatistics& RHIBridge::GetFrameStatistics() const
        {
            return m_device->GetStatistics();
        }

        std::string RHIBridge::GetDeviceInfo() const
        {
            return m_device ? m_device->GetDeviceInfo() : "No device";
        }

        std::string RHIBridge::GetBackendName() const
        {
            switch (GetActiveBackend())
            {
            case GraphicsBackend::D3D11:
                return "DirectX 11";
            case GraphicsBackend::D3D12:
                return "DirectX 12";
            case GraphicsBackend::Vulkan:
                return "Vulkan";
            case GraphicsBackend::OpenGL:
                return "OpenGL";
            case GraphicsBackend::Metal:
                return "Metal";
            default:
                return "Unknown";
            }
        }

        bool RHIBridge::IsBackendAvailable(GraphicsBackend backend)
        {
            auto available = GetAvailableBackends();
            return Spark::ContainerUtils::Contains(available, backend);
        }

        std::vector<GraphicsBackend> RHIBridge::GetAvailableBackends()
        {
            std::vector<GraphicsBackend> backends;

#ifdef _WIN32
            backends.push_back(GraphicsBackend::D3D11);
#endif

#ifdef SPARK_VULKAN_SUPPORT
            backends.push_back(GraphicsBackend::Vulkan);
#endif

#ifdef SPARK_OPENGL_SUPPORT
            backends.push_back(GraphicsBackend::OpenGL);
#endif

            return backends;
        }

        GraphicsBackend RHIBridge::GetRecommendedBackend()
        {
#ifdef _WIN32
            return GraphicsBackend::D3D11;
#elif defined(SPARK_VULKAN_SUPPORT)
            return GraphicsBackend::Vulkan;
#elif defined(SPARK_OPENGL_SUPPORT)
            return GraphicsBackend::OpenGL;
#else
            return GraphicsBackend::Auto;
#endif
        }

        // ============================================================================
        // PRIVATE
        // ============================================================================

        GraphicsBackend RHIBridge::SelectBestBackend() const
        {
            return GetRecommendedBackend();
        }

        bool RHIBridge::CreateDepthBufferInternal(uint32_t width, uint32_t height)
        {
            m_depthBuffer = CreateDepthBuffer(width, height);
            return m_depthBuffer != nullptr;
        }

    } // namespace RHI
} // namespace Spark
