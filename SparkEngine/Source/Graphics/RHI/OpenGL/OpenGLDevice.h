/**
 * @file OpenGLDevice.h
 * @brief OpenGL 4.6 Core Profile implementation of the RHI device interface
 * @author Spark Engine Team
 * @date 2025
 *
 * Full OpenGL 4.6 Core Profile backend supporting DSA (Direct State Access),
 * SPIR-V shaders, compute shaders, and modern OpenGL features.
 */

#pragma once
#include "../../../Core/Platform.h"

#include "../RHIDevice.h"
#include "../RHIResources.h"
// Phase Z Theme 3B: activated Tier 2 RHI orphan — per-frame transient
// vertex/index bump allocator. Each OpenGLDevice instance owns one so
// render systems can request cheap per-frame CPU-visible GPU memory.
#include "../TransientBufferAllocator.h"

#ifdef SPARK_OPENGL_SUPPORT

// OpenGL headers - using GLAD loader
#include <glad/glad.h>

#ifdef _WIN32
#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#endif // SPARK_PLATFORM_WINDOWS
#elif defined(__linux__)
#ifdef SPARK_EGL_SUPPORT
#include <EGL/egl.h>
#else
#include <GL/glx.h>
// glxext.h provides ARB extension typedefs (e.g. PFNGLXCREATECONTEXTATTRIBSARBPROC)
#if __has_include(<GL/glxext.h>)
#include <GL/glxext.h>
#endif
// Fallback declarations if glxext.h is not available
#ifndef GLX_ARB_create_context
typedef GLXContext (*PFNGLXCREATECONTEXTATTRIBSARBPROC)(Display*, GLXFBConfig, GLXContext, int, const int*);
#define GLX_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define GLX_CONTEXT_MINOR_VERSION_ARB 0x2092
#define GLX_CONTEXT_PROFILE_MASK_ARB 0x9126
#define GLX_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001
#endif
#ifndef GLX_ARB_get_proc_address
extern "C" void (*glXGetProcAddressARB(const GLubyte*))();
#endif
#endif
// X11 headers define short macros (None, Always, True, Bool, etc.) that clash
// with C++ enum values. Undef them after the X11 includes are done.
#ifdef None
#undef None
#endif
#ifdef Always
#undef Always
#endif
#ifdef True
#undef True
#endif
#ifdef False
#undef False
#endif
#ifdef Bool
#undef Bool
#endif
#ifdef Success
#undef Success
#endif
#ifdef Status
#undef Status
#endif
#ifdef KeyPress
#undef KeyPress
#endif
#ifdef KeyRelease
#undef KeyRelease
#endif
#ifdef FocusIn
#undef FocusIn
#endif
#ifdef FocusOut
#undef FocusOut
#endif
#ifdef Expose
#undef Expose
#endif
#endif

#include <vector>
#include <unordered_map>
#include <string>

namespace Spark
{
    namespace RHI
    {
        namespace OpenGL
        {

            // ============================================================================
            // FORWARD DECLARATIONS
            // ============================================================================

            class GLDevice;

            // ============================================================================
            // OPENGL RESOURCE IMPLEMENTATIONS
            // ============================================================================

            class GLBuffer : public IRHIBuffer
            {
              public:
                GLBuffer(const RHIBufferDesc& desc, GLuint buffer);
                ~GLBuffer() override;

                const std::string& GetDebugName() const override { return m_desc.debugName; }
                void SetDebugName(const std::string& name) override;
                bool IsValid() const override { return m_buffer != 0; }

                const RHIBufferDesc& GetDesc() const override { return m_desc; }
                uint64_t GetSize() const override { return m_desc.size; }
                uint32_t GetStride() const override { return m_desc.stride; }
                void* GetNativeHandle() const override
                {
                    return reinterpret_cast<void*>(static_cast<uintptr_t>(m_buffer));
                }

                GLuint GetGLBuffer() const { return m_buffer; }

              private:
                RHIBufferDesc m_desc;
                GLuint m_buffer;
            };

            class GLTexture : public IRHITexture
            {
              public:
                GLTexture(const RHITextureDesc& desc, GLuint texture, GLuint framebuffer = 0,
                          GLenum target = GL_TEXTURE_2D);
                ~GLTexture() override;

                const std::string& GetDebugName() const override { return m_desc.debugName; }
                void SetDebugName(const std::string& name) override;
                bool IsValid() const override { return m_texture != 0; }

                const RHITextureDesc& GetDesc() const override { return m_desc; }
                uint32_t GetWidth() const override { return m_desc.width; }
                uint32_t GetHeight() const override { return m_desc.height; }
                uint32_t GetDepth() const override { return m_desc.depth; }
                uint32_t GetMipLevels() const override { return m_desc.mipLevels; }
                PixelFormat GetFormat() const override { return m_desc.format; }
                void* GetNativeHandle() const override
                {
                    return reinterpret_cast<void*>(static_cast<uintptr_t>(m_texture));
                }
                void* GetShaderResourceView() const override { return GetNativeHandle(); }
                void* GetRenderTargetView() const override
                {
                    return reinterpret_cast<void*>(static_cast<uintptr_t>(m_framebuffer));
                }
                void* GetDepthStencilView() const override { return GetNativeHandle(); }

                GLuint GetGLTexture() const { return m_texture; }
                GLenum GetGLTarget() const { return m_target; }
                GLuint GetGLFramebuffer() const { return m_framebuffer; }
                void SetFramebuffer(GLuint fbo) { m_framebuffer = fbo; }

              private:
                RHITextureDesc m_desc;
                GLuint m_texture;
                GLenum m_target;
                GLuint m_framebuffer;
            };

            class GLShader : public IRHIShader
            {
              public:
                GLShader(const RHIShaderDesc& desc, GLuint shader, std::string compiledSource);
                ~GLShader() override;

                const std::string& GetDebugName() const override { return m_desc.debugName; }
                void SetDebugName(const std::string& name) override { m_desc.debugName = name; }
                bool IsValid() const override { return m_shader != 0; }

                RHIShaderStage GetStage() const override { return m_desc.stage; }
                const std::string& GetEntryPoint() const override { return m_desc.entryPoint; }
                void* GetNativeHandle() const override
                {
                    return reinterpret_cast<void*>(static_cast<uintptr_t>(m_shader));
                }
                const void* GetBytecode() const override { return m_compiledSource.data(); }
                size_t GetBytecodeSize() const override { return m_compiledSource.size(); }

                GLuint GetGLShader() const { return m_shader; }

              private:
                RHIShaderDesc m_desc;
                GLuint m_shader;
                std::string m_compiledSource;
            };

            class GLSampler : public IRHISampler
            {
              public:
                GLSampler(const RHISamplerDesc& desc, GLuint sampler);
                ~GLSampler() override;

                const std::string& GetDebugName() const override { return m_debugName; }
                void SetDebugName(const std::string& name) override { m_debugName = name; }
                bool IsValid() const override { return m_sampler != 0; }

                const RHISamplerDesc& GetDesc() const override { return m_desc; }
                void* GetNativeHandle() const override
                {
                    return reinterpret_cast<void*>(static_cast<uintptr_t>(m_sampler));
                }

                GLuint GetGLSampler() const { return m_sampler; }

              private:
                RHISamplerDesc m_desc;
                GLuint m_sampler;
                std::string m_debugName;
            };

            class GLPipelineState : public IRHIPipelineState
            {
              public:
                GLPipelineState(const RHIPipelineStateDesc& desc, GLuint program, GLuint vao);
                ~GLPipelineState() override;

                const std::string& GetDebugName() const override { return m_desc.debugName; }
                void SetDebugName(const std::string& name) override { m_desc.debugName = name; }
                bool IsValid() const override { return m_program != 0; }

                const RHIPipelineStateDesc& GetDesc() const override { return m_desc; }
                void* GetNativeHandle() const override
                {
                    return reinterpret_cast<void*>(static_cast<uintptr_t>(m_program));
                }

                GLuint GetGLProgram() const { return m_program; }
                GLuint GetGLVAO() const { return m_vao; }

                void ApplyRasterizerState() const;
                void ApplyDepthStencilState() const;
                void ApplyBlendState() const;

              private:
                RHIPipelineStateDesc m_desc;
                GLuint m_program;
                GLuint m_vao;
            };

            // ============================================================================
            // OPENGL SWAP CHAIN
            // ============================================================================

            class GLSwapChain : public IRHISwapChain
            {
              public:
                GLSwapChain(const RHISwapChainDesc& desc);
                ~GLSwapChain() override;

                bool Present(bool vsync) override;
                bool Resize(uint32_t width, uint32_t height) override;
                IRHITexture* GetBackBuffer() override;
                PixelFormat GetFormat() const override { return m_desc.format; }
                uint32_t GetWidth() const override { return m_desc.width; }
                uint32_t GetHeight() const override { return m_desc.height; }
                uint32_t GetCurrentBufferIndex() const override { return 0; }

              private:
                RHISwapChainDesc m_desc;
                std::unique_ptr<GLTexture> m_backBuffer;
                bool m_windowed = false; ///< True when rendering to an on-screen window

#ifdef _WIN32
                HDC m_hdc = nullptr;
                HGLRC m_hglrc = nullptr;
#elif defined(__linux__)
                void* m_sdlWindow = nullptr; ///< SDL_Window* for windowed Present
#ifdef SPARK_EGL_SUPPORT
                EGLDisplay m_eglDisplay = EGL_NO_DISPLAY;
                EGLSurface m_eglSurface = EGL_NO_SURFACE;
                EGLContext m_eglContext = EGL_NO_CONTEXT;
#endif
#endif
            };

            // ============================================================================
            // OPENGL COMMAND LIST
            // ============================================================================

            class GLCommandList : public IRHICommandList
            {
              public:
                GLCommandList(bool isImmediate, RHIStatistics* statistics);
                ~GLCommandList() override = default;

                void Begin() override;
                void End() override;
                void Reset() override;

                void SetRenderTargets(IRHITexture* const* renderTargets, uint32_t count,
                                      IRHITexture* depthStencil) override;
                void ClearRenderTarget(IRHITexture* target, const float color[4]) override;
                void ClearDepthStencil(IRHITexture* target, float depth, uint8_t stencil) override;

                void SetViewport(const RHIViewport& viewport) override;
                void SetScissorRect(const RHIScissorRect& rect) override;

                void SetPipelineState(IRHIPipelineState* pipelineState) override;
                void SetPrimitiveTopology(RHIPrimitiveTopology topology) override;

                void SetVertexBuffer(IRHIBuffer* buffer, uint32_t slot, uint32_t offset) override;
                void SetIndexBuffer(IRHIBuffer* buffer, uint32_t offset) override;
                void SetConstantBuffer(RHIShaderStage stage, uint32_t slot, IRHIBuffer* buffer) override;
                void SetShaderResource(RHIShaderStage stage, uint32_t slot, IRHITexture* texture) override;
                void SetSampler(RHIShaderStage stage, uint32_t slot, IRHISampler* sampler) override;

                void Draw(uint32_t vertexCount, uint32_t startVertex) override;
                void DrawIndexed(uint32_t indexCount, uint32_t startIndex, int32_t baseVertex) override;
                void DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex,
                                   uint32_t startInstance) override;
                void DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t startIndex,
                                          int32_t baseVertex, uint32_t startInstance) override;

                void Dispatch(uint32_t x, uint32_t y, uint32_t z) override;

                void DrawInstancedIndirect(IRHIBuffer* argsBuffer, uint32_t argsOffset) override;
                void DrawIndexedInstancedIndirect(IRHIBuffer* argsBuffer, uint32_t argsOffset) override;
                void DispatchIndirect(IRHIBuffer* argsBuffer, uint32_t argsOffset) override;

                void CopyTexture(IRHITexture* dst, IRHITexture* src) override;

                void BeginEvent(const char* name) override;
                void EndEvent() override;
                void SetMarker(const char* name) override;

              private:
                bool m_isImmediate;
                RHIStatistics* m_statistics = nullptr;
                GLenum m_currentTopology = GL_TRIANGLES;
                GLuint m_currentVAO = 0;
                GLuint m_currentProgram = 0;
                GLuint m_boundIndexBuffer = 0;
                uint32_t m_indexStride = 4;
                IRHIPipelineState* m_lastBoundPipeline = nullptr; ///< Redundant bind elimination
            };

            // ============================================================================
            // OPENGL DEVICE
            // ============================================================================

            class GLDevice : public IRHIDevice
            {
              public:
                GLDevice();
                ~GLDevice() override;

                bool Initialize(const RHIDeviceDesc& desc) override;
                void Shutdown() override;

                std::unique_ptr<IRHISwapChain> CreateSwapChain(const RHISwapChainDesc& desc) override;

                std::unique_ptr<IRHIBuffer> CreateBuffer(const RHIBufferDesc& desc) override;
                std::unique_ptr<IRHITexture> CreateTexture(const RHITextureDesc& desc) override;
                std::unique_ptr<IRHITexture> WrapNativeTexture(void* nativeHandle, const RHITextureDesc& desc) override;
                std::unique_ptr<IRHIShader> CreateShader(const RHIShaderDesc& desc) override;
                std::unique_ptr<IRHISampler> CreateSampler(const RHISamplerDesc& desc) override;
                std::unique_ptr<IRHIPipelineState> CreatePipelineState(const RHIPipelineStateDesc& desc,
                                                                       IRHIShader* vertexShader,
                                                                       IRHIShader* pixelShader) override;

                void* MapBuffer(IRHIBuffer* buffer) override;
                void UnmapBuffer(IRHIBuffer* buffer) override;
                void UpdateBuffer(IRHIBuffer* buffer, const void* data, size_t size, size_t offset) override;
                void UpdateTexture(IRHITexture* texture, const void* data, uint32_t mipLevel,
                                   uint32_t arraySlice) override;

                void GenerateMips(IRHITexture* texture);

                IRHICommandList* GetImmediateCommandList() override;
                std::unique_ptr<IRHICommandList> CreateDeferredCommandList() override;
                void ExecuteCommandList(IRHICommandList* commandList) override;

                void BeginFrame() override;
                void EndFrame() override;
                void WaitForIdle() override;

                GraphicsBackend GetBackendType() const override { return GraphicsBackend::OpenGL; }
                const RHIDeviceCapabilities& GetCapabilities() const override { return m_capabilities; }
                const RHIStatistics& GetStatistics() const override { return m_statistics; }
                void ResetStatistics() override;
                std::string GetDeviceInfo() const override;

              private:
                GLenum ConvertFormat(PixelFormat format) const;
                GLenum ConvertInternalFormat(PixelFormat format) const;
                GLenum ConvertFormatType(PixelFormat format) const;
                GLenum ConvertAddressMode(RHIAddressMode mode) const;
                GLenum GetTextureTarget(const RHITextureDesc& desc) const;
                GLenum GetDepthAttachmentType(PixelFormat format) const;

                void QueryCapabilities();

                std::unique_ptr<GLCommandList> m_immediateCommandList;
                RHIDeviceCapabilities m_capabilities;
                RHIStatistics m_statistics;
                bool m_debugEnabled = false;
                bool m_shutdownCalled = false; ///< Guards against double Shutdown() calls

#if defined(__linux__) && !defined(SPARK_EGL_SUPPORT)
                Display* m_glxDisplay = nullptr;
                GLXContext m_glxContext = nullptr;
                GLXPbuffer m_glxPbuffer = 0;
                bool m_ownsGLXContext = false; ///< False when SDL2 created the context
#elif defined(__linux__) && defined(SPARK_EGL_SUPPORT)
                EGLDisplay m_bootstrapDisplay = EGL_NO_DISPLAY;
                EGLContext m_bootstrapContext = EGL_NO_CONTEXT;
                EGLSurface m_bootstrapSurface = EGL_NO_SURFACE;
#endif

                // Phase Z Theme 3B: per-frame transient vertex/index allocator.
                // Initialized in OpenGLDevice::Initialize after the GL context
                // is ready; Shutdown releases its GL buffers.
                TransientBufferAllocator m_transientBuffers{4 * 1024 * 1024, 2 * 1024 * 1024};
            };

        } // namespace OpenGL
    } // namespace RHI
} // namespace Spark

#endif // SPARK_OPENGL_SUPPORT
