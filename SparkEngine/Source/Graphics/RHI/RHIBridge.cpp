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
#include <cstdlib>
#include <fstream>
#include <algorithm>

namespace
{
    // Returns true when an environment variable is set to a truthy value
    // ("1", "true", "yes", "on" — case-insensitive). Used as an opt-out for
    // GPU backends that are known to crash on the current host (e.g. Mesa
    // Lavapipe under gVisor SIGSEGVs during VulkanDevice init — set
    // SPARK_DISABLE_VULKAN=1 to skip Vulkan and fall straight through to
    // the OpenGL/Null backends).
    bool EnvFlagTrue(const char* name)
    {
        const char* v = std::getenv(name);
        if (!v || !*v)
            return false;
        // Case-insensitive compare against common truthy spellings.
        auto ieq = [](const char* a, const char* b)
        {
            for (; *a && *b; ++a, ++b)
            {
                const int ca = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a;
                const int cb = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
                if (ca != cb)
                    return false;
            }
            return *a == *b;
        };
        return ieq(v, "1") || ieq(v, "true") || ieq(v, "yes") || ieq(v, "on");
    }
} // namespace

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

                const std::streamsize streamSize = file.tellg();
                if (streamSize <= 0)
                    return nullptr;
                file.seekg(0);
                const size_t size = static_cast<size_t>(streamSize);
                std::vector<uint8_t> buffer(size);
                if (!file.read(reinterpret_cast<char*>(buffer.data()), streamSize))
                    return nullptr;
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

            // Headless safety: if no window was supplied, force NullRHI. GPU
            // backends (Vulkan/OpenGL/D3D) can fail or hang when asked to
            // initialize without a presentation surface, so callers that only
            // need device-side resources (tests, tools, dedicated servers)
            // must stay on the null backend regardless of what Auto / a
            // recommended backend would choose.
            if (windowHandle == nullptr && backend != GraphicsBackend::None)
            {
                SPARK_LOG_INFO(Spark::LogCategory::Graphics,
                               "RHIBridge::Initialize: null windowHandle — forcing NullRHI backend");
                backend = GraphicsBackend::None;
            }

            // Auto-select backend if requested
            if (backend == GraphicsBackend::Auto)
                backend = SelectBestBackend();

            // Surface the env-var escape hatches so users can see why a
            // backend was skipped. This is especially useful in CI/sandbox
            // runs where a broken Vulkan ICD would otherwise crash the
            // process instead of failing cleanly through the fallback loop.
            if (EnvFlagTrue("SPARK_DISABLE_VULKAN"))
                SPARK_LOG_INFO(Spark::LogCategory::Graphics, "SPARK_DISABLE_VULKAN=1 — Vulkan backend skipped");
            if (EnvFlagTrue("SPARK_DISABLE_OPENGL"))
                SPARK_LOG_INFO(Spark::LogCategory::Graphics, "SPARK_DISABLE_OPENGL=1 — OpenGL backend skipped");
            if (EnvFlagTrue("SPARK_DISABLE_D3D11"))
                SPARK_LOG_INFO(Spark::LogCategory::Graphics, "SPARK_DISABLE_D3D11=1 — D3D11 backend skipped");

            // Try the preferred backend first, then fall back to alternatives.
            // This handles the common case where Vulkan is preferred on Linux but
            // no Vulkan driver is present — the engine falls back to OpenGL.
            auto backendsToTry = GetAvailableBackends();

            // Move the preferred backend to the front of the list.
            //
            // If the caller explicitly requested a backend that isn't in
            // the "available" set (e.g. GraphicsBackend::None to force
            // NullRHIDevice, or a disabled backend from an env-var escape
            // hatch), we must decide whether to honor it or filter it out.
            //
            //  - None: always honor — it's the explicit headless path.
            //  - Auto: never insert — it's a sentinel meaning "pick one
            //    for me". If the list is empty we want to fall straight
            //    through to the NullRHI fallback below, not feed Auto to
            //    RHIFactory::CreateDevice (which would resolve Auto via
            //    its own GetRecommendedBackend, bypassing our env-var
            //    opt-outs).
            //  - A GPU backend that was disabled via SPARK_DISABLE_*: drop
            //    it entirely, otherwise we'd crash on the very backend the
            //    user asked us to skip.
            //  - Anything else not in the list (unsupported on this build):
            //    insert it at the front so CreateDevice gets a chance to
            //    handle it — this preserves pre-existing defensive behavior.
            auto it = std::find(backendsToTry.begin(), backendsToTry.end(), backend);
            if (it != backendsToTry.end() && it != backendsToTry.begin())
            {
                std::rotate(backendsToTry.begin(), it, it + 1);
            }
            else if (it == backendsToTry.end() && backend != GraphicsBackend::Auto)
            {
                const bool disabledByEnv =
                    (backend == GraphicsBackend::Vulkan && EnvFlagTrue("SPARK_DISABLE_VULKAN")) ||
                    (backend == GraphicsBackend::OpenGL && EnvFlagTrue("SPARK_DISABLE_OPENGL")) ||
                    (backend == GraphicsBackend::D3D11 && EnvFlagTrue("SPARK_DISABLE_D3D11"));
                if (!disabledByEnv)
                    backendsToTry.insert(backendsToTry.begin(), backend);
            }

            RHISwapChainDesc swapDesc;
            swapDesc.windowHandle = windowHandle;
            swapDesc.width = width;
            swapDesc.height = height;
            swapDesc.format = PixelFormat::R8G8B8A8_UNORM;
            swapDesc.bufferCount = 2;
            swapDesc.fullscreen = false;
            swapDesc.vsync = true;

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

                if (!m_device->Initialize(deviceDesc))
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Graphics, "Backend '%s' failed to initialize — trying next",
                                   Spark::RHI::GetBackendName(candidate));
                    m_device.reset();
                    continue;
                }

                // The NullRHIDevice explicitly does not own a swap chain, so
                // don't try to build one on top of it — leave that to the
                // headless branch below.
                if (m_device->GetBackendType() == GraphicsBackend::None)
                {
                    deviceReady = true;
                    break;
                }

                // Device init succeeded — try to stand up the swap chain on
                // this backend. If swap-chain creation fails (e.g. a Vulkan
                // ICD is present but cannot create a surface for this window),
                // tear the device back down and continue to the next
                // candidate instead of aborting the whole Initialize().
                m_swapChain = m_device->CreateSwapChain(swapDesc);
                if (!m_swapChain)
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                                   "Backend '%s' failed to create swap chain — trying next",
                                   Spark::RHI::GetBackendName(candidate));
                    m_device->Shutdown();
                    m_device.reset();
                    continue;
                }

                if (candidate != backend)
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                                   "Preferred backend '%s' unavailable — fell back to '%s'",
                                   Spark::RHI::GetBackendName(backend), Spark::RHI::GetBackendName(candidate));
                }
                deviceReady = true;
                break;
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

            // At this point the swap chain was created inside the backend
            // loop above. Only the depth buffer is left to stand up.
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
            if (!m_device)
                return nullptr;
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
            if (!m_device)
                return nullptr;
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
            if (!m_device)
                return nullptr;
            RHIBufferDesc desc;
            desc.size = size;
            desc.usage = RHIBufferUsage::Constant;
            desc.access = RHIBufferAccess::Dynamic;
            return m_device->CreateBuffer(desc);
        }

        std::unique_ptr<IRHITexture> RHIBridge::CreateTexture2D(uint32_t width, uint32_t height, PixelFormat format,
                                                                RHITextureUsage usage, const void* data)
        {
            if (!m_device)
                return nullptr;
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
            if (!m_device)
                return nullptr;
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
            if (!m_device)
                return nullptr;
            RHITextureDesc desc;
            desc.width = width;
            desc.height = height;
            desc.format = format;
            desc.usage = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource;
            desc.mipLevels = 1;
            return m_device->CreateTexture(desc);
        }

        void RHIBridge::RegisterRenderTarget(RenderTargetSlot slot, IRHITexture* texture)
        {
            const auto idx = static_cast<uint32_t>(slot);
            if (idx >= static_cast<uint32_t>(RenderTargetSlot::Count))
                return;
            m_renderTargets[idx] = texture;
        }

        IRHITexture* RHIBridge::GetRenderTarget(RenderTargetSlot slot) const
        {
            const auto idx = static_cast<uint32_t>(slot);
            if (idx >= static_cast<uint32_t>(RenderTargetSlot::Count))
                return nullptr;
            return m_renderTargets[idx];
        }

        std::unique_ptr<IRHISampler> RHIBridge::CreateSamplerLinearWrap()
        {
            if (!m_device)
                return nullptr;
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
            if (!m_device)
                return nullptr;
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
            if (!m_device)
                return nullptr;
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
            if (!m_device)
                return nullptr;
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
            static const RHIDeviceCapabilities kNoDeviceCapabilities{};
            return m_device ? m_device->GetCapabilities() : kNoDeviceCapabilities;
        }

        const RHIStatistics& RHIBridge::GetFrameStatistics() const
        {
            static const RHIStatistics kNoDeviceStatistics{};
            return m_device ? m_device->GetStatistics() : kNoDeviceStatistics;
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
            case GraphicsBackend::None:
                return "NullRHI (headless)";
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

            // Environment escape hatches — set SPARK_DISABLE_<BACKEND>=1 to
            // skip a backend that is known to misbehave on the current host.
            // This exists because some GPU drivers (e.g. Mesa Lavapipe under
            // gVisor) crash rather than returning a clean error from
            // Initialize(), which defeats RHIBridge's soft fallback loop.
            const bool disableD3D11 = EnvFlagTrue("SPARK_DISABLE_D3D11");
            const bool disableVulkan = EnvFlagTrue("SPARK_DISABLE_VULKAN");
            const bool disableOpenGL = EnvFlagTrue("SPARK_DISABLE_OPENGL");

#ifdef _WIN32
            if (!disableD3D11)
                backends.push_back(GraphicsBackend::D3D11);
#else
            (void)disableD3D11;
#endif

#ifdef SPARK_VULKAN_SUPPORT
            if (!disableVulkan)
                backends.push_back(GraphicsBackend::Vulkan);
#else
            (void)disableVulkan;
#endif

#ifdef SPARK_OPENGL_SUPPORT
            if (!disableOpenGL)
                backends.push_back(GraphicsBackend::OpenGL);
#else
            (void)disableOpenGL;
#endif

            return backends;
        }

        GraphicsBackend RHIBridge::GetRecommendedBackend()
        {
            // Delegate to the factory's GetRecommendedBackend() so the
            // SPARK_RHI_BACKEND env-var override and the gVisor-aware
            // fallback live in a single place. Previously this method
            // hard-coded D3D11/Vulkan/OpenGL at compile time and ignored
            // runtime overrides, which meant Linux startup paths (the
            // Graphics*Linux.cpp translation units) couldn't honor the
            // env var — see
            // .claude/knowledge/wine-role-and-fallback-tiers-2026-04-14.md
            // action item #5.
            return ::Spark::RHI::GetRecommendedBackend();
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
