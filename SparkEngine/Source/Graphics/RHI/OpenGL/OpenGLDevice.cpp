/**
 * @file OpenGLDevice.cpp
 * @brief OpenGL 4.6 Core Profile RHI backend implementation
 * @author Spark Engine Team
 * @date 2025
 *
 * Modern OpenGL implementation using DSA (Direct State Access),
 * SPIR-V shader support, and GL 4.6 Core Profile features.
 *
 * RHI Ownership Model: Create*() methods return raw pointers. The RHI device
 * owns the underlying GPU resource. Callers must call the corresponding
 * Destroy*() method to release. This pattern is intentional — it matches
 * the D3D11/D3D12/Vulkan/OpenGL resource lifecycle and avoids forcing
 * std::unique_ptr across the backend-agnostic RHI boundary.
 */

#ifdef SPARK_OPENGL_SUPPORT

#include "OpenGLDevice.h"
#include "../RHIFormatUtils.h"
#include "../../../Utils/Validate.h"
#ifdef SPARK_SDL2_AVAILABLE
#include <SDL2/SDL.h>
#endif
#include <algorithm>
#include <cassert>
#include <cstring>
#include <sstream>

#if defined(__APPLE__)
#include <dlfcn.h>
#endif

namespace Spark
{
    namespace RHI
    {
        namespace OpenGL
        {

            namespace
            {
                /// @brief The backend uses GL 4.5 core entry points (DSA, KHR_debug, glGetTextureSubImage)
                ///        unconditionally. A context below 4.5 (e.g. Windows' GDI Generic GL 1.1 on a
                ///        GPU-less host) leaves those pointers null, so treat it as "no usable GL".
                bool HasRequiredGLVersion()
                {
                    if (GLAD_GL_VERSION_4_5)
                        return true;
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                    "OpenGL 4.5 core is required but the current context provides %d.%d",
                                    GLVersion.major, GLVersion.minor);
                    return false;
                }

                GLenum ConvertCompareOp(RHICompareOp op)
                {
                    switch (op)
                    {
                    case RHICompareOp::Never:
                        return GL_NEVER;
                    case RHICompareOp::Less:
                        return GL_LESS;
                    case RHICompareOp::Equal:
                        return GL_EQUAL;
                    case RHICompareOp::LessEqual:
                        return GL_LEQUAL;
                    case RHICompareOp::Greater:
                        return GL_GREATER;
                    case RHICompareOp::NotEqual:
                        return GL_NOTEQUAL;
                    case RHICompareOp::GreaterEqual:
                        return GL_GEQUAL;
                    case RHICompareOp::Always:
                        return GL_ALWAYS;
                    }
                    return GL_ALWAYS;
                }

                GLenum ConvertStencilOp(RHIStencilOp op)
                {
                    switch (op)
                    {
                    case RHIStencilOp::Keep:
                        return GL_KEEP;
                    case RHIStencilOp::Zero:
                        return GL_ZERO;
                    case RHIStencilOp::Replace:
                        return GL_REPLACE;
                    case RHIStencilOp::IncrSat:
                        return GL_INCR;
                    case RHIStencilOp::DecrSat:
                        return GL_DECR;
                    case RHIStencilOp::Invert:
                        return GL_INVERT;
                    case RHIStencilOp::IncrWrap:
                        return GL_INCR_WRAP;
                    case RHIStencilOp::DecrWrap:
                        return GL_DECR_WRAP;
                    }
                    return GL_KEEP;
                }

                /// D3D clears ignore the bound pipeline's write masks and scissor; GL clears
                /// honor them. Force full writes for the clear and restore the pipeline state.
                class ClearStateScope
                {
                  public:
                    ClearStateScope()
                    {
                        glGetBooleanv(GL_COLOR_WRITEMASK, m_colorMask);
                        glGetBooleanv(GL_DEPTH_WRITEMASK, &m_depthMask);
                        glGetIntegerv(GL_STENCIL_WRITEMASK, &m_stencilMask);
                        glGetIntegerv(GL_STENCIL_BACK_WRITEMASK, &m_stencilBackMask);
                        m_scissor = glIsEnabled(GL_SCISSOR_TEST);
                        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                        glDepthMask(GL_TRUE);
                        glStencilMask(0xFF);
                        glDisable(GL_SCISSOR_TEST);
                    }
                    ~ClearStateScope()
                    {
                        glColorMask(m_colorMask[0], m_colorMask[1], m_colorMask[2], m_colorMask[3]);
                        glDepthMask(m_depthMask);
                        glStencilMaskSeparate(GL_FRONT, static_cast<GLuint>(m_stencilMask));
                        glStencilMaskSeparate(GL_BACK, static_cast<GLuint>(m_stencilBackMask));
                        if (m_scissor)
                            glEnable(GL_SCISSOR_TEST);
                    }
                    ClearStateScope(const ClearStateScope&) = delete;
                    ClearStateScope& operator=(const ClearStateScope&) = delete;

                  private:
                    GLboolean m_colorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
                    GLboolean m_depthMask = GL_TRUE;
                    GLint m_stencilMask = 0xFF;
                    GLint m_stencilBackMask = 0xFF;
                    GLboolean m_scissor = GL_FALSE;
                };
            } // namespace

            // ============================================================================
            // GL BUFFER
            // ============================================================================

            GLBuffer::GLBuffer(const RHIBufferDesc& desc, GLuint buffer) : m_desc(desc), m_buffer(buffer) {}

            GLBuffer::~GLBuffer()
            {
                if (m_buffer != 0)
                {
                    glDeleteBuffers(1, &m_buffer);
                    m_buffer = 0;
                }
            }

            void GLBuffer::SetDebugName(const std::string& name)
            {
                m_desc.debugName = name;
                if (m_buffer != 0)
                {
                    glObjectLabel(GL_BUFFER, m_buffer, static_cast<GLsizei>(name.size()), name.c_str());
                }
            }

            // ============================================================================
            // GL TEXTURE
            // ============================================================================

            GLTexture::GLTexture(const RHITextureDesc& desc, GLuint texture, GLuint framebuffer, GLenum target,
                                 bool ownsTexture)
                : m_desc(desc), m_texture(texture), m_target(target), m_framebuffer(framebuffer),
                  m_ownsTexture(ownsTexture)
            {
            }

            GLTexture::~GLTexture()
            {
                if (m_framebuffer != 0)
                {
                    glDeleteFramebuffers(1, &m_framebuffer);
                }
                // Wrapped textures (WrapNativeTexture) are externally owned — never delete them
                if (m_texture != 0 && m_ownsTexture)
                {
                    glDeleteTextures(1, &m_texture);
                }
            }

            void GLTexture::SetDebugName(const std::string& name)
            {
                m_desc.debugName = name;
                if (m_texture != 0)
                {
                    glObjectLabel(GL_TEXTURE, m_texture, static_cast<GLsizei>(name.size()), name.c_str());
                }
            }

            // ============================================================================
            // GL SHADER
            // ============================================================================

            GLShader::GLShader(const RHIShaderDesc& desc, GLuint shader, std::string compiledSource)
                : m_desc(desc), m_shader(shader), m_compiledSource(std::move(compiledSource))
            {
            }

            GLShader::~GLShader()
            {
                if (m_shader != 0)
                {
                    glDeleteShader(m_shader);
                }
            }

            // ============================================================================
            // GL SAMPLER
            // ============================================================================

            GLSampler::GLSampler(const RHISamplerDesc& desc, GLuint sampler) : m_desc(desc), m_sampler(sampler) {}

            GLSampler::~GLSampler()
            {
                if (m_sampler != 0)
                {
                    glDeleteSamplers(1, &m_sampler);
                }
            }

            // ============================================================================
            // GL PIPELINE STATE
            // ============================================================================

            GLPipelineState::GLPipelineState(const RHIPipelineStateDesc& desc, GLuint program, GLuint vao)
                : m_desc(desc), m_program(program), m_vao(vao)
            {
            }

            GLPipelineState::~GLPipelineState()
            {
                if (m_program != 0)
                    glDeleteProgram(m_program);
                if (m_vao != 0)
                    glDeleteVertexArrays(1, &m_vao);
            }

            void GLPipelineState::ApplyRasterizerState() const
            {
                // Fill mode
                glPolygonMode(GL_FRONT_AND_BACK,
                              m_desc.rasterizer.fillMode == RHIFillMode::Wireframe ? GL_LINE : GL_FILL);

                // Cull mode
                if (m_desc.rasterizer.cullMode == RHICullMode::None)
                {
                    glDisable(GL_CULL_FACE);
                }
                else
                {
                    glEnable(GL_CULL_FACE);
                    glCullFace(m_desc.rasterizer.cullMode == RHICullMode::Front ? GL_FRONT : GL_BACK);
                }

                // Front face
                glFrontFace(m_desc.rasterizer.frontCounterClockwise ? GL_CCW : GL_CW);

                // Depth bias
                if (m_desc.rasterizer.depthBias != 0 || m_desc.rasterizer.slopeScaledDepthBias != 0)
                {
                    glEnable(GL_POLYGON_OFFSET_FILL);
                    glPolygonOffset(m_desc.rasterizer.slopeScaledDepthBias,
                                    static_cast<float>(m_desc.rasterizer.depthBias));
                }
                else
                {
                    glDisable(GL_POLYGON_OFFSET_FILL);
                }

                // Scissor
                if (m_desc.rasterizer.scissorEnable)
                    glEnable(GL_SCISSOR_TEST);
                else
                    glDisable(GL_SCISSOR_TEST);

                // Depth clamp
                if (!m_desc.rasterizer.depthClipEnable)
                    glEnable(GL_DEPTH_CLAMP);
                else
                    glDisable(GL_DEPTH_CLAMP);
            }

            void GLPipelineState::ApplyDepthStencilState() const
            {
                const auto& ds = m_desc.depthStencil;
                if (ds.depthEnable)
                {
                    glEnable(GL_DEPTH_TEST);
                    glDepthFunc(ConvertCompareOp(ds.depthFunc));
                }
                else
                {
                    glDisable(GL_DEPTH_TEST);
                }

                // Depth write
                glDepthMask(ds.depthWrite ? GL_TRUE : GL_FALSE);

                // Stencil test: func/ops per face; reference is 0 to match the D3D11 backend
                if (ds.stencilEnable)
                {
                    glEnable(GL_STENCIL_TEST);
                    glStencilFuncSeparate(GL_FRONT, ConvertCompareOp(ds.frontFace.stencilFunc), 0, ds.stencilReadMask);
                    glStencilOpSeparate(GL_FRONT, ConvertStencilOp(ds.frontFace.stencilFail),
                                        ConvertStencilOp(ds.frontFace.stencilDepthFail),
                                        ConvertStencilOp(ds.frontFace.stencilPass));
                    glStencilFuncSeparate(GL_BACK, ConvertCompareOp(ds.backFace.stencilFunc), 0, ds.stencilReadMask);
                    glStencilOpSeparate(GL_BACK, ConvertStencilOp(ds.backFace.stencilFail),
                                        ConvertStencilOp(ds.backFace.stencilDepthFail),
                                        ConvertStencilOp(ds.backFace.stencilPass));
                    glStencilMask(ds.stencilWriteMask);
                }
                else
                {
                    glDisable(GL_STENCIL_TEST);
                }
            }

            void GLPipelineState::ApplyBlendState() const
            {
                const auto& rt0 = m_desc.blend.renderTargets[0];
                if (rt0.blendEnable)
                {
                    glEnable(GL_BLEND);

                    auto convertBlend = [](RHIBlendFactor f) -> GLenum
                    {
                        switch (f)
                        {
                        case RHIBlendFactor::Zero:
                            return GL_ZERO;
                        case RHIBlendFactor::One:
                            return GL_ONE;
                        case RHIBlendFactor::SrcColor:
                            return GL_SRC_COLOR;
                        case RHIBlendFactor::InvSrcColor:
                            return GL_ONE_MINUS_SRC_COLOR;
                        case RHIBlendFactor::SrcAlpha:
                            return GL_SRC_ALPHA;
                        case RHIBlendFactor::InvSrcAlpha:
                            return GL_ONE_MINUS_SRC_ALPHA;
                        case RHIBlendFactor::DstAlpha:
                            return GL_DST_ALPHA;
                        case RHIBlendFactor::InvDstAlpha:
                            return GL_ONE_MINUS_DST_ALPHA;
                        case RHIBlendFactor::DstColor:
                            return GL_DST_COLOR;
                        case RHIBlendFactor::InvDstColor:
                            return GL_ONE_MINUS_DST_COLOR;
                        default:
                            return GL_ZERO;
                        }
                    };

                    auto convertOp = [](RHIBlendOp op) -> GLenum
                    {
                        switch (op)
                        {
                        case RHIBlendOp::Add:
                            return GL_FUNC_ADD;
                        case RHIBlendOp::Subtract:
                            return GL_FUNC_SUBTRACT;
                        case RHIBlendOp::RevSubtract:
                            return GL_FUNC_REVERSE_SUBTRACT;
                        case RHIBlendOp::Min:
                            return GL_MIN;
                        case RHIBlendOp::Max:
                            return GL_MAX;
                        default:
                            return GL_FUNC_ADD;
                        }
                    };

                    glBlendFuncSeparate(convertBlend(rt0.srcBlend), convertBlend(rt0.dstBlend),
                                        convertBlend(rt0.srcBlendAlpha), convertBlend(rt0.dstBlendAlpha));
                    glBlendEquationSeparate(convertOp(rt0.blendOp), convertOp(rt0.blendOpAlpha));
                }
                else
                {
                    glDisable(GL_BLEND);
                }

                glColorMask((rt0.writeMask & 0x01) ? GL_TRUE : GL_FALSE, (rt0.writeMask & 0x02) ? GL_TRUE : GL_FALSE,
                            (rt0.writeMask & 0x04) ? GL_TRUE : GL_FALSE, (rt0.writeMask & 0x08) ? GL_TRUE : GL_FALSE);
            }

            // ============================================================================
            // GL SWAP CHAIN
            // ============================================================================

            GLSwapChain::GLSwapChain(const RHISwapChainDesc& desc, [[maybe_unused]] void* deviceDC,
                                     [[maybe_unused]] void* deviceContext)
                : m_desc(desc)
            {
                if (desc.windowHandle == nullptr)
                {
                    // Headless mode (all platforms): an FBO stands in for the swap chain back buffer and
                    // renders on the device's own context.
                    m_windowed = false;

                    GLuint colorTex = 0;
                    glCreateTextures(GL_TEXTURE_2D, 1, &colorTex);
                    glTextureStorage2D(colorTex, 1, GL_RGBA8, desc.width, desc.height);

                    GLuint fbo = 0;
                    glCreateFramebuffers(1, &fbo);
                    glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0, colorTex, 0);

                    GLenum status = glCheckNamedFramebufferStatus(fbo, GL_FRAMEBUFFER);
                    if (status != GL_FRAMEBUFFER_COMPLETE)
                    {
                        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Headless swap chain FBO incomplete: 0x%x",
                                        status);
                    }

                    RHITextureDesc texDesc;
                    texDesc.width = desc.width;
                    texDesc.height = desc.height;
                    texDesc.format = desc.format;
                    texDesc.usage = RHITextureUsage::RenderTarget;
                    texDesc.debugName = "HeadlessBackBuffer";

                    m_backBuffer = std::make_unique<GLTexture>(texDesc, colorTex, fbo);

                    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "OpenGL swap chain: headless FBO mode (%ux%u)",
                                   desc.width, desc.height);
                    return;
                }

                // Windowed mode: render to the default framebuffer (FBO 0)
                m_windowed = true;

                RHITextureDesc texDesc;
                texDesc.width = desc.width;
                texDesc.height = desc.height;
                texDesc.format = desc.format;
                texDesc.usage = RHITextureUsage::RenderTarget;
                texDesc.debugName = "DefaultFramebuffer";

                m_backBuffer = std::make_unique<GLTexture>(texDesc, 0, 0); // FBO 0 = default

#if defined(__linux__)
                // SDL2 created the GL context and owns the window; Present uses SDL_GL_SwapWindow.
                m_sdlWindow = desc.windowHandle;
#elif defined(_WIN32)
                // Rebind the device's context to the application window. wglMakeCurrent requires the
                // window DC to use the same pixel format the context was created with, so copy the
                // device DC's format instead of choosing a new one. A separate context here would not
                // share any object the device already created.
                m_hwnd = static_cast<HWND>(desc.windowHandle);
                m_deviceDC = static_cast<HDC>(deviceDC);
                m_deviceContext = static_cast<HGLRC>(deviceContext);
                m_hdc = GetDC(m_hwnd);

                bool bound = false;
                if (m_hdc && m_deviceDC && m_deviceContext)
                {
                    const int pixelFormat = GetPixelFormat(m_deviceDC);
                    PIXELFORMATDESCRIPTOR pfd = {};
                    DescribePixelFormat(m_deviceDC, pixelFormat, sizeof(pfd), &pfd);
                    // SetPixelFormat may be called only once per window; a matching format is fine.
                    if (GetPixelFormat(m_hdc) == pixelFormat || SetPixelFormat(m_hdc, pixelFormat, &pfd))
                        bound = wglMakeCurrent(m_hdc, m_deviceContext) != FALSE;
                }
                if (!bound)
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                    "OpenGL swap chain: could not bind the device context to the window (error %lu)",
                                    GetLastError());
                }
#endif

                SPARK_LOG_INFO(Spark::LogCategory::Graphics, "OpenGL swap chain: windowed mode (%ux%u)", desc.width,
                               desc.height);
            }

            GLSwapChain::~GLSwapChain()
            {
#if defined(_WIN32)
                if (m_hdc)
                {
                    // Hand the device context back to the device's hidden window so the device keeps a
                    // current context after the window goes away.
                    if (wglGetCurrentDC() == m_hdc)
                    {
                        if (m_deviceDC && m_deviceContext)
                            wglMakeCurrent(m_deviceDC, m_deviceContext);
                        else
                            wglMakeCurrent(nullptr, nullptr);
                    }
                    ReleaseDC(m_hwnd, m_hdc);
                }
#endif
                // Linux: FBO and texture are owned by the GLTexture destructor
            }

            bool GLSwapChain::Present(bool vsync)
            {
#if defined(__linux__)
                if (m_windowed && m_sdlWindow)
                {
#ifdef SPARK_SDL2_AVAILABLE
                    SDL_GL_SetSwapInterval(vsync ? 1 : 0);
                    SDL_GL_SwapWindow(static_cast<SDL_Window*>(m_sdlWindow));
#else
                    glFlush();
#endif
                    return true;
                }
                // Headless: flush all pending GL commands (no window to swap to)
                glFlush();
                return true;
#elif defined(_WIN32)
                if (m_hdc)
                {
                    SwapBuffers(m_hdc);
                    return true;
                }
                if (!m_windowed)
                {
                    // Headless: flush all pending GL commands (no window to swap to)
                    glFlush();
                    return true;
                }
#endif
                return false;
            }

            bool GLSwapChain::Resize(uint32_t width, uint32_t height)
            {
                m_desc.width = width;
                m_desc.height = height;

                if (m_windowed)
                {
                    // Windowed: default framebuffer resizes automatically with the window.
                    // Just update the stored dimensions in the back buffer wrapper.
                    if (m_backBuffer)
                    {
                        RHITextureDesc texDesc;
                        texDesc.width = width;
                        texDesc.height = height;
                        texDesc.format = m_desc.format;
                        texDesc.usage = RHITextureUsage::RenderTarget;
                        texDesc.debugName = "DefaultFramebuffer";
                        m_backBuffer = std::make_unique<GLTexture>(texDesc, 0, 0);
                    }
                }
                else
                {
                    // Headless: recreate the FBO at the new size
                    GLuint colorTex = 0;
                    glCreateTextures(GL_TEXTURE_2D, 1, &colorTex);
                    glTextureStorage2D(colorTex, 1, GL_RGBA8, width, height);

                    GLuint fbo = 0;
                    glCreateFramebuffers(1, &fbo);
                    glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0, colorTex, 0);

                    RHITextureDesc texDesc;
                    texDesc.width = width;
                    texDesc.height = height;
                    texDesc.format = m_desc.format;
                    texDesc.usage = RHITextureUsage::RenderTarget;
                    texDesc.debugName = "HeadlessBackBuffer";

                    // Old FBO/texture cleaned up by GLTexture destructor
                    m_backBuffer = std::make_unique<GLTexture>(texDesc, colorTex, fbo);
                }

                glViewport(0, 0, width, height);
                return true;
            }

            IRHITexture* GLSwapChain::GetBackBuffer()
            {
                return m_backBuffer.get();
            }

            // ============================================================================
            // GL COMMAND LIST
            // ============================================================================

            GLCommandList::GLCommandList(bool isImmediate, RHIStatistics* statistics)
                : m_isImmediate(isImmediate), m_statistics(statistics)
            {
            }

            GLCommandList::~GLCommandList()
            {
                if (m_compositeFBO != 0)
                {
                    glDeleteFramebuffers(1, &m_compositeFBO);
                }
            }

            void GLCommandList::Begin() {}
            void GLCommandList::End()
            {
                // Intentionally no glFlush() here — flushing every command list
                // submission causes severe GPU pipeline stalls. GL commands are
                // flushed implicitly by swap or explicitly by WaitForIdle().
            }
            void GLCommandList::Reset() {}

            void GLCommandList::SetRenderTargets(IRHITexture* const* renderTargets, uint32_t count,
                                                 IRHITexture* depthStencil)
            {
                if (count == 0 || !renderTargets[0])
                {
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    return;
                }

                auto* glTex = static_cast<GLTexture*>(renderTargets[0]);

                if (glTex->GetGLFramebuffer() == 0)
                {
                    // Default framebuffer: valid draw buffers are GL_BACK (not GL_COLOR_ATTACHMENT0)
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    GLenum backBuf = GL_BACK;
                    glDrawBuffers(1, &backBuf);
                }
                else
                {
                    // Each texture's private FBO holds only that texture, so color+depth and
                    // MRT passes must compose all attachments into a command-list-owned FBO.
                    // Attachments are rewritten on every call, so no destroyed texture can
                    // linger on the FBO at draw time.
                    if (m_compositeFBO == 0)
                    {
                        glCreateFramebuffers(1, &m_compositeFBO);
                    }

                    for (uint32_t i = 0; i < count; ++i)
                    {
                        auto* colorTex = static_cast<GLTexture*>(renderTargets[i]);
                        glNamedFramebufferTexture(m_compositeFBO, GL_COLOR_ATTACHMENT0 + i,
                                                  colorTex ? colorTex->GetGLTexture() : 0, 0);
                    }
                    // Detach color attachments left over from a previous wider MRT bind
                    for (uint32_t i = count; i < m_compositeColorCount; ++i)
                    {
                        glNamedFramebufferTexture(m_compositeFBO, GL_COLOR_ATTACHMENT0 + i, 0, 0);
                    }
                    m_compositeColorCount = count;

                    // Texture 0 on GL_DEPTH_STENCIL_ATTACHMENT detaches both depth and stencil
                    glNamedFramebufferTexture(m_compositeFBO, GL_DEPTH_STENCIL_ATTACHMENT, 0, 0);
                    if (depthStencil)
                    {
                        auto* depthTex = static_cast<GLTexture*>(depthStencil);
                        const PixelFormat depthFormat = depthTex->GetFormat();
                        const bool hasStencil = depthFormat == PixelFormat::D24_UNORM_S8_UINT ||
                                                depthFormat == PixelFormat::D32_FLOAT_S8_UINT;
                        glNamedFramebufferTexture(m_compositeFBO,
                                                  hasStencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT,
                                                  depthTex->GetGLTexture(), 0);
                    }

                    std::vector<GLenum> drawBuffers(count);
                    for (uint32_t i = 0; i < count; ++i)
                        drawBuffers[i] = GL_COLOR_ATTACHMENT0 + i;
                    glNamedFramebufferDrawBuffers(m_compositeFBO, static_cast<GLsizei>(count), drawBuffers.data());

                    glBindFramebuffer(GL_FRAMEBUFFER, m_compositeFBO);
                }

                if (m_statistics)
                {
                    m_statistics->renderTargetChanges++;
                }
            }

            void GLCommandList::ClearRenderTarget(IRHITexture* target, const float color[4])
            {
                if (!target)
                    return;
                auto* glTex = static_cast<GLTexture*>(target);
                ClearStateScope scope;
                if (glTex->GetGLFramebuffer() != 0 || glTex->GetGLTexture() == 0)
                {
                    // The texture's private FBO (or the default framebuffer) holds exactly this target
                    glClearNamedFramebufferfv(glTex->GetGLFramebuffer(), GL_COLOR, 0, color);
                }
                else
                {
                    glClearTexImage(glTex->GetGLTexture(), 0, GL_RGBA, GL_FLOAT, color);
                }
            }

            void GLCommandList::ClearDepthStencil(IRHITexture* target, float depth, uint8_t stencil)
            {
                if (!target)
                    return;
                auto* glTex = static_cast<GLTexture*>(target);
                ClearStateScope scope;
                const GLuint fbo = glTex->GetGLFramebuffer();
                if (HasStencilComponent(glTex->GetFormat()))
                {
                    glClearNamedFramebufferfi(fbo, GL_DEPTH_STENCIL, 0, depth, stencil);
                }
                else
                {
                    glClearNamedFramebufferfv(fbo, GL_DEPTH, 0, &depth);
                }
            }

            void GLCommandList::SetViewport(const RHIViewport& viewport)
            {
                glViewport(static_cast<GLint>(viewport.x), static_cast<GLint>(viewport.y),
                           static_cast<GLsizei>(viewport.width), static_cast<GLsizei>(viewport.height));
                glDepthRange(viewport.minDepth, viewport.maxDepth);
            }

            void GLCommandList::SetScissorRect(const RHIScissorRect& rect)
            {
                glScissor(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
            }

            void GLCommandList::SetPipelineState(IRHIPipelineState* pipelineState)
            {
                if (!pipelineState)
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                                   "GL: SetPipelineState called with null pipeline state");
                    return;
                }
                if (pipelineState == m_lastBoundPipeline)
                    return; // Skip redundant GL state changes
                m_lastBoundPipeline = pipelineState;
                auto* glPSO = static_cast<GLPipelineState*>(pipelineState);

                m_currentProgram = glPSO->GetGLProgram();
                m_currentVAO = glPSO->GetGLVAO();

                glUseProgram(m_currentProgram);
                glBindVertexArray(m_currentVAO);

                // Vertex/index buffer bindings live on the VAO; carry them over to this pipeline's VAO
                for (uint32_t slot = 0; slot < kMaxVertexBufferSlots; ++slot)
                {
                    if (m_vertexBuffers[slot].buffer != 0)
                        ApplyVertexBuffer(slot);
                }
                glVertexArrayElementBuffer(m_currentVAO, m_boundIndexBuffer);

                glPSO->ApplyRasterizerState();
                glPSO->ApplyDepthStencilState();
                glPSO->ApplyBlendState();

                if (m_statistics)
                {
                    m_statistics->pipelineChanges++;
                }
            }

            void GLCommandList::SetPrimitiveTopology(RHIPrimitiveTopology topology)
            {
                switch (topology)
                {
                case RHIPrimitiveTopology::PointList:
                    m_currentTopology = GL_POINTS;
                    break;
                case RHIPrimitiveTopology::LineList:
                    m_currentTopology = GL_LINES;
                    break;
                case RHIPrimitiveTopology::LineStrip:
                    m_currentTopology = GL_LINE_STRIP;
                    break;
                case RHIPrimitiveTopology::TriangleList:
                    m_currentTopology = GL_TRIANGLES;
                    break;
                case RHIPrimitiveTopology::TriangleStrip:
                    m_currentTopology = GL_TRIANGLE_STRIP;
                    break;
                case RHIPrimitiveTopology::PatchList:
                    m_currentTopology = GL_PATCHES;
                    break;
                }
            }

            void GLCommandList::ApplyVertexBuffer(uint32_t slot)
            {
                const VertexBufferBinding& binding = m_vertexBuffers[slot];
                uint32_t stride = binding.stride;
                if (stride == 0 && m_lastBoundPipeline)
                    stride = static_cast<GLPipelineState*>(m_lastBoundPipeline)->GetSlotStride(slot);
                glVertexArrayVertexBuffer(m_currentVAO, slot, binding.buffer, static_cast<GLintptr>(binding.offset),
                                          static_cast<GLsizei>(stride));
            }

            void GLCommandList::SetVertexBuffer(IRHIBuffer* buffer, uint32_t slot, uint32_t offset)
            {
                if (slot >= kMaxVertexBufferSlots)
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Graphics, "GL: SetVertexBuffer slot %u exceeds %u", slot,
                                   kMaxVertexBufferSlots);
                    return;
                }
                auto* glBuf = static_cast<GLBuffer*>(buffer);
                m_vertexBuffers[slot] = {glBuf ? glBuf->GetGLBuffer() : 0, offset, glBuf ? glBuf->GetStride() : 0};
                if (m_currentVAO != 0)
                    ApplyVertexBuffer(slot);
            }

            void GLCommandList::SetIndexBuffer(IRHIBuffer* buffer, uint32_t offset)
            {
                auto* glBuf = static_cast<GLBuffer*>(buffer);
                m_boundIndexBuffer = glBuf ? glBuf->GetGLBuffer() : 0;
                m_indexStride = (glBuf && glBuf->GetStride() == 2) ? 2 : 4;
                m_indexOffset = offset;
                // GL_ELEMENT_ARRAY_BUFFER is VAO state; SetPipelineState re-applies it to the next VAO
                if (m_currentVAO != 0)
                    glVertexArrayElementBuffer(m_currentVAO, m_boundIndexBuffer);
            }

            void GLCommandList::SetConstantBuffer(RHIShaderStage, uint32_t slot, IRHIBuffer* buffer)
            {
                if (!buffer)
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                                   "GL: SetConstantBuffer called with null buffer (slot %u) — unbinding slot", slot);
                    glBindBufferBase(GL_UNIFORM_BUFFER, slot, 0);
                    return;
                }
                auto* glBuf = static_cast<GLBuffer*>(buffer);
                glBindBufferBase(GL_UNIFORM_BUFFER, slot, glBuf->GetGLBuffer());
                if (m_statistics)
                {
                    m_statistics->bufferBinds++;
                }
            }

            void GLCommandList::SetShaderResource(RHIShaderStage, uint32_t slot, IRHITexture* texture)
            {
                if (texture)
                {
                    auto* glTex = static_cast<GLTexture*>(texture);
                    glBindTextureUnit(slot, glTex->GetGLTexture());
                }
                else
                {
                    glBindTextureUnit(slot, 0);
                }
                if (m_statistics)
                {
                    m_statistics->textureBinds++;
                }
            }

            void GLCommandList::SetSampler(RHIShaderStage, uint32_t slot, IRHISampler* sampler)
            {
                if (sampler)
                {
                    auto* glSamp = static_cast<GLSampler*>(sampler);
                    glBindSampler(slot, glSamp->GetGLSampler());
                }
                else
                {
                    glBindSampler(slot, 0);
                }
            }

            void GLCommandList::Draw(uint32_t vertexCount, uint32_t startVertex)
            {
                glDrawArrays(m_currentTopology, startVertex, vertexCount);
                if (m_statistics)
                {
                    m_statistics->drawCalls++;
                    m_statistics->verticesProcessed += vertexCount;
                    if (m_currentTopology == GL_TRIANGLES)
                    {
                        m_statistics->trianglesRendered += vertexCount / 3;
                    }
                }
            }

            void GLCommandList::DrawIndexed(uint32_t indexCount, uint32_t startIndex, int32_t baseVertex)
            {
                GLenum indexType = (m_indexStride == 4) ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
                size_t offset = m_indexOffset + static_cast<size_t>(startIndex) * m_indexStride;
                glDrawElementsBaseVertex(m_currentTopology, indexCount, indexType, reinterpret_cast<void*>(offset),
                                         baseVertex);
                if (m_statistics)
                {
                    m_statistics->drawCalls++;
                    m_statistics->verticesProcessed += indexCount;
                    if (m_currentTopology == GL_TRIANGLES)
                    {
                        m_statistics->trianglesRendered += indexCount / 3;
                    }
                }
            }

            void GLCommandList::DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex,
                                              uint32_t startInstance)
            {
                glDrawArraysInstancedBaseInstance(m_currentTopology, startVertex, vertexCount, instanceCount,
                                                  startInstance);
                if (m_statistics)
                {
                    m_statistics->drawCalls++;
                    m_statistics->verticesProcessed += vertexCount * instanceCount;
                    if (m_currentTopology == GL_TRIANGLES)
                    {
                        m_statistics->trianglesRendered += (vertexCount / 3) * instanceCount;
                    }
                }
            }

            void GLCommandList::DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t startIndex,
                                                     int32_t baseVertex, uint32_t startInstance)
            {
                GLenum indexType = (m_indexStride == 4) ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
                size_t offset = m_indexOffset + static_cast<size_t>(startIndex) * m_indexStride;
                glDrawElementsInstancedBaseVertexBaseInstance(m_currentTopology, indexCount, indexType,
                                                              reinterpret_cast<void*>(offset), instanceCount,
                                                              baseVertex, startInstance);
                if (m_statistics)
                {
                    m_statistics->drawCalls++;
                    m_statistics->verticesProcessed += indexCount * instanceCount;
                    if (m_currentTopology == GL_TRIANGLES)
                    {
                        m_statistics->trianglesRendered += (indexCount / 3) * instanceCount;
                    }
                }
            }

            void GLCommandList::Dispatch(uint32_t x, uint32_t y, uint32_t z)
            {
                glDispatchCompute(x, y, z);
                glMemoryBarrier(GL_ALL_BARRIER_BITS);
                if (m_statistics)
                {
                    m_statistics->dispatchCalls++;
                }
            }

            void GLCommandList::DrawInstancedIndirect(IRHIBuffer* argsBuffer, uint32_t argsOffset)
            {
                if (!argsBuffer)
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                                   "GL: DrawInstancedIndirect called with null args buffer");
                    return;
                }
                auto* glBuf = static_cast<GLBuffer*>(argsBuffer);
                glBindBuffer(GL_DRAW_INDIRECT_BUFFER, glBuf->GetGLBuffer());
                glDrawArraysIndirect(m_currentTopology,
                                     reinterpret_cast<const void*>(static_cast<uintptr_t>(argsOffset)));
                if (m_statistics)
                    m_statistics->drawCalls++;
            }

            void GLCommandList::DrawIndexedInstancedIndirect(IRHIBuffer* argsBuffer, uint32_t argsOffset)
            {
                if (!argsBuffer)
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                                   "GL: DrawIndexedInstancedIndirect called with null args buffer");
                    return;
                }
                auto* glBuf = static_cast<GLBuffer*>(argsBuffer);
                glBindBuffer(GL_DRAW_INDIRECT_BUFFER, glBuf->GetGLBuffer());
                glDrawElementsIndirect(m_currentTopology, GL_UNSIGNED_INT,
                                       reinterpret_cast<const void*>(static_cast<uintptr_t>(argsOffset)));
                if (m_statistics)
                    m_statistics->drawCalls++;
            }

            void GLCommandList::DispatchIndirect(IRHIBuffer* argsBuffer, uint32_t argsOffset)
            {
                if (!argsBuffer)
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Graphics, "GL: DispatchIndirect called with null args buffer");
                    return;
                }
                auto* glBuf = static_cast<GLBuffer*>(argsBuffer);
                glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, glBuf->GetGLBuffer());
                glDispatchComputeIndirect(static_cast<GLintptr>(argsOffset));
                glMemoryBarrier(GL_ALL_BARRIER_BITS);
                if (m_statistics)
                    m_statistics->dispatchCalls++;
            }

            void GLCommandList::CopyTexture(IRHITexture* dst, IRHITexture* src)
            {
                if (!dst || !src)
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Graphics, "GL: CopyTexture called with null %s",
                                   !dst ? "destination" : "source");
                    return;
                }
                auto* glDst = static_cast<GLTexture*>(dst);
                auto* glSrc = static_cast<GLTexture*>(src);
                const RHITextureDesc& srcDesc = glSrc->GetDesc();
                GLsizei depth = 1;
                if (srcDesc.type == RHITextureType::Texture3D)
                    depth = static_cast<GLsizei>(srcDesc.depth);
                else if (srcDesc.type == RHITextureType::Texture2DArray)
                    depth = static_cast<GLsizei>(srcDesc.arraySize);
                else if (srcDesc.type == RHITextureType::TextureCube)
                    depth = 6;
                else if (srcDesc.type == RHITextureType::TextureCubeArray)
                    depth = static_cast<GLsizei>(srcDesc.arraySize * 6);
                glCopyImageSubData(glSrc->GetGLTexture(), glSrc->GetGLTarget(), 0, 0, 0, 0, glDst->GetGLTexture(),
                                   glDst->GetGLTarget(), 0, 0, 0, 0, glSrc->GetWidth(), glSrc->GetHeight(), depth);
            }

            void GLCommandList::BeginEvent(const char* name)
            {
                glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, -1, name);
            }

            void GLCommandList::EndEvent()
            {
                glPopDebugGroup();
            }

            void GLCommandList::SetMarker(const char* name)
            {
                glDebugMessageInsert(GL_DEBUG_SOURCE_APPLICATION, GL_DEBUG_TYPE_MARKER, 0,
                                     GL_DEBUG_SEVERITY_NOTIFICATION, -1, name);
            }

            // ============================================================================
            // GL DEVICE
            // ============================================================================

            GLDevice::GLDevice()
            {
                m_capabilities.backend = GraphicsBackend::OpenGL;
            }

            GLDevice::~GLDevice()
            {
                Shutdown();
            }

            bool GLDevice::Initialize(const RHIDeviceDesc& desc)
            {
                SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
                SPARK_LOG_INFO(Spark::LogCategory::Graphics, "GLDevice::Initialize starting");
                m_debugEnabled = desc.enableDebugLayer;

                // OpenGL requires a current context before any GL calls (including GLAD loading).
                // Create a temporary hidden window and context to bootstrap GLAD, then tear it down.
                // The real context is created later by GLSwapChain with the application window.
                //
                // On Linux with EGL, we create a surfaceless EGL context using Mesa's software
                // renderer (llvmpipe). This enables full GL rendering without a GPU or display.
#if defined(__linux__) && defined(SPARK_EGL_SUPPORT)
                // If SDL2 (or any other host) already created a GL context, reuse it
                // instead of bootstrapping an EGL pbuffer. This is critical: when SDL
                // owns the window's GL context, creating a separate EGL pbuffer and
                // making it current would route all rendering to the pbuffer while
                // SDL_GL_SwapWindow still swaps the window — frames never reach the
                // screen. Match the GLX branch's detect-and-reuse pattern.
                if (eglGetCurrentContext() != EGL_NO_CONTEXT)
                {
                    SPARK_LOG_INFO(Spark::LogCategory::Graphics,
                                   "Existing EGL context detected (SDL2/host-owned) — skipping EGL bootstrap");
                    m_bootstrapDisplay = eglGetCurrentDisplay();
                    m_bootstrapContext = eglGetCurrentContext();
                    m_bootstrapSurface = eglGetCurrentSurface(EGL_DRAW);
                    m_ownsEglContext = false; // host owns this context
                    if (!gladLoadGL())
                    {
                        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "GLAD loader failed");
                        return false;
                    }
                    if (!HasRequiredGLVersion())
                        return false; // host owns the context; leave it alone
                    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "OpenGL %s (GLSL %s) — Renderer: %s",
                                   reinterpret_cast<const char*>(glGetString(GL_VERSION)),
                                   reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION)),
                                   reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
                    // The reuse path must still run the common post-bootstrap setup —
                    // without it GetImmediateCommandList() stays null and RHIAdapter fails.
                    QueryCapabilities();
                    m_immediateCommandList = std::make_unique<GLCommandList>(true, &m_statistics);
                    m_transientBuffers.Initialize(this);
                    return true;
                }

                // EGL headless bootstrap — works with Mesa llvmpipe, no X11/GPU required
                m_bootstrapDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
                if (m_bootstrapDisplay == EGL_NO_DISPLAY)
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "eglGetDisplay failed");
                    return false;
                }

                EGLint major = 0;
                EGLint minor = 0;
                if (!eglInitialize(m_bootstrapDisplay, &major, &minor))
                {
                    // EGL_DEFAULT_DISPLAY needs X11/Wayland. With no display server (CI, containers)
                    // Mesa's surfaceless platform still provides llvmpipe for FBO-only rendering.
                    const EGLint defaultError = eglGetError();
                    auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
                        eglGetProcAddress("eglGetPlatformDisplayEXT"));
                    m_bootstrapDisplay = getPlatformDisplay ? getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA,
                                                                                 EGL_DEFAULT_DISPLAY, nullptr)
                                                            : EGL_NO_DISPLAY;
                    if (m_bootstrapDisplay == EGL_NO_DISPLAY || !eglInitialize(m_bootstrapDisplay, &major, &minor))
                    {
                        SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                        "eglInitialize failed: 0x%x (default display), 0x%x (surfaceless)",
                                        defaultError, eglGetError());
                        m_bootstrapDisplay = EGL_NO_DISPLAY;
                        return false;
                    }
                    SPARK_LOG_INFO(Spark::LogCategory::Graphics,
                                   "No EGL display server — using the Mesa surfaceless platform");
                }
                SPARK_LOG_INFO(Spark::LogCategory::Graphics, "EGL %d.%d initialized", major, minor);

                // Request an OpenGL context (not OpenGL ES)
                if (!eglBindAPI(EGL_OPENGL_API))
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "eglBindAPI(EGL_OPENGL_API) failed: 0x%x",
                                    eglGetError());
                    eglTerminate(m_bootstrapDisplay);
                    return false;
                }

                // Choose an EGL config that supports OpenGL rendering
                // clang-format off
                const EGLint configAttribs[] = {
                    EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                    EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
                    EGL_RED_SIZE,        8,
                    EGL_GREEN_SIZE,      8,
                    EGL_BLUE_SIZE,       8,
                    EGL_ALPHA_SIZE,      8,
                    EGL_DEPTH_SIZE,      24,
                    EGL_STENCIL_SIZE,    8,
                    EGL_NONE
                };
                // clang-format on

                EGLConfig config;
                EGLint numConfigs = 0;
                if (!eglChooseConfig(m_bootstrapDisplay, configAttribs, &config, 1, &numConfigs) || numConfigs == 0)
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "eglChooseConfig failed: 0x%x (configs=%d)",
                                    eglGetError(), numConfigs);
                    eglTerminate(m_bootstrapDisplay);
                    return false;
                }

                // Create a 1x1 PBuffer surface (required for context creation; rendering uses FBOs)
                // clang-format off
                const EGLint pbufferAttribs[] = {
                    EGL_WIDTH,  1,
                    EGL_HEIGHT, 1,
                    EGL_NONE
                };
                // clang-format on

                m_bootstrapSurface = eglCreatePbufferSurface(m_bootstrapDisplay, config, pbufferAttribs);
                if (m_bootstrapSurface == EGL_NO_SURFACE)
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                                   "eglCreatePbufferSurface failed: 0x%x — trying surfaceless", eglGetError());
                    // Surfaceless context is fine for FBO-only rendering
                    m_bootstrapSurface = EGL_NO_SURFACE;
                }

                // Request a Core Profile context (Mesa llvmpipe supports up to GL 4.5)
                // A debug context guarantees KHR_debug output is delivered (enableDebugLayer)
                // clang-format off
                const EGLint contextAttribs[] = {
                    EGL_CONTEXT_MAJOR_VERSION, 4,
                    EGL_CONTEXT_MINOR_VERSION, 5,
                    EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                    EGL_CONTEXT_OPENGL_DEBUG, m_debugEnabled ? EGL_TRUE : EGL_FALSE,
                    EGL_NONE
                };
                // clang-format on

                m_bootstrapContext = eglCreateContext(m_bootstrapDisplay, config, EGL_NO_CONTEXT, contextAttribs);
                if (m_bootstrapContext == EGL_NO_CONTEXT)
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "eglCreateContext failed: 0x%x", eglGetError());
                    if (m_bootstrapSurface != EGL_NO_SURFACE)
                        eglDestroySurface(m_bootstrapDisplay, m_bootstrapSurface);
                    eglTerminate(m_bootstrapDisplay);
                    return false;
                }

                if (!eglMakeCurrent(m_bootstrapDisplay, m_bootstrapSurface, m_bootstrapSurface, m_bootstrapContext))
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "eglMakeCurrent failed: 0x%x", eglGetError());
                    eglDestroyContext(m_bootstrapDisplay, m_bootstrapContext);
                    if (m_bootstrapSurface != EGL_NO_SURFACE)
                        eglDestroySurface(m_bootstrapDisplay, m_bootstrapSurface);
                    eglTerminate(m_bootstrapDisplay);
                    return false;
                }

                SPARK_LOG_INFO(Spark::LogCategory::Graphics, "EGL headless context created successfully");
#elif defined(__linux__) && !defined(SPARK_EGL_SUPPORT)
                // If an OpenGL context is already current (e.g. SDL2 created it), skip the GLX bootstrap.
                // This is the normal case when running in SDL2 windowed mode — SDL2 creates the GL context
                // and we just need to load GLAD and continue.
                if (glXGetCurrentContext() != nullptr)
                {
                    SPARK_LOG_INFO(Spark::LogCategory::Graphics,
                                   "Existing GLX context detected (SDL2) — skipping GLX bootstrap");
                    m_glxDisplay = XOpenDisplay(nullptr);
                    m_glxContext = glXGetCurrentContext();
                    m_ownsGLXContext = false; // SDL2 owns this context
                    // Load GLAD using the existing context
                    if (!gladLoadGL())
                    {
                        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "GLAD loader failed");
                        return false;
                    }
                    if (!HasRequiredGLVersion())
                        return false; // host owns the context; leave it alone
                    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "OpenGL %s (GLSL %s) — Renderer: %s",
                                   reinterpret_cast<const char*>(glGetString(GL_VERSION)),
                                   reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION)),
                                   reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
                    // The reuse path must still run the common post-bootstrap setup —
                    // without it GetImmediateCommandList() stays null and RHIAdapter fails.
                    QueryCapabilities();
                    m_immediateCommandList = std::make_unique<GLCommandList>(true, &m_statistics);
                    m_transientBuffers.Initialize(this);
                    return true;
                }

                // GLX bootstrap — requires X11 display (use Xvfb for headless software rendering)
                Display* bootstrapDpy = XOpenDisplay(nullptr);
                if (!bootstrapDpy)
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                    "XOpenDisplay failed — set DISPLAY or start Xvfb for software rendering");
                    return false;
                }

                // clang-format off
                int fbAttribs[] = {
                    GLX_RENDER_TYPE,   GLX_RGBA_BIT,
                    GLX_DRAWABLE_TYPE, GLX_PBUFFER_BIT | GLX_WINDOW_BIT,
                    GLX_RED_SIZE,      8,
                    GLX_GREEN_SIZE,    8,
                    GLX_BLUE_SIZE,     8,
                    GLX_ALPHA_SIZE,    8,
                    GLX_DEPTH_SIZE,    24,
                    GLX_STENCIL_SIZE,  8,
                    0 // None
                };
                // clang-format on

                int fbCount = 0;
                GLXFBConfig* fbConfigs =
                    glXChooseFBConfig(bootstrapDpy, DefaultScreen(bootstrapDpy), fbAttribs, &fbCount);
                if (!fbConfigs || fbCount == 0)
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "glXChooseFBConfig failed — no suitable config");
                    if (fbConfigs)
                        XFree(fbConfigs);
                    XCloseDisplay(bootstrapDpy);
                    return false;
                }

                // Create a PBuffer for off-screen rendering (no visible window needed)
                // clang-format off
                int pbAttribs[] = {
                    GLX_PBUFFER_WIDTH,  1,
                    GLX_PBUFFER_HEIGHT, 1,
                    0 // None
                };
                // clang-format on

                GLXPbuffer bootstrapPbuffer = glXCreatePbuffer(bootstrapDpy, fbConfigs[0], pbAttribs);

                // Try to create a GL 4.5 Core context via ARB extension
                auto glXCreateContextAttribsARB = reinterpret_cast<PFNGLXCREATECONTEXTATTRIBSARBPROC>(
                    glXGetProcAddressARB(reinterpret_cast<const GLubyte*>("glXCreateContextAttribsARB")));

                GLXContext bootstrapCtx = nullptr;
                if (glXCreateContextAttribsARB)
                {
                    // clang-format off
                    int ctxAttribs[] = {
                        GLX_CONTEXT_MAJOR_VERSION_ARB, 4,
                        GLX_CONTEXT_MINOR_VERSION_ARB, 5,
                        GLX_CONTEXT_PROFILE_MASK_ARB,  GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
                        GLX_CONTEXT_FLAGS_ARB,         m_debugEnabled ? GLX_CONTEXT_DEBUG_BIT_ARB : 0,
                        0 // None
                    };
                    // clang-format on

                    bootstrapCtx =
                        glXCreateContextAttribsARB(bootstrapDpy, fbConfigs[0], nullptr, 1 /*direct*/, ctxAttribs);
                }

                if (!bootstrapCtx)
                {
                    // Fallback: legacy context (may give a Compatibility Profile)
                    XVisualInfo* vi = glXGetVisualFromFBConfig(bootstrapDpy, fbConfigs[0]);
                    if (vi)
                    {
                        bootstrapCtx = glXCreateContext(bootstrapDpy, vi, nullptr, 1 /*direct*/);
                        XFree(vi);
                    }
                }

                XFree(fbConfigs);

                if (!bootstrapCtx)
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Failed to create GLX bootstrap context");
                    glXDestroyPbuffer(bootstrapDpy, bootstrapPbuffer);
                    XCloseDisplay(bootstrapDpy);
                    return false;
                }

                glXMakeCurrent(bootstrapDpy, bootstrapPbuffer, bootstrapCtx);
                m_glxDisplay = bootstrapDpy;
                m_glxContext = bootstrapCtx;
                m_glxPbuffer = bootstrapPbuffer;
                m_ownsGLXContext = true; // We created this context

                SPARK_LOG_INFO(Spark::LogCategory::Graphics,
                               "GLX bootstrap context created (software rendering via Xvfb)");
#elif defined(_WIN32)
                WNDCLASSA wc = {};
                wc.lpfnWndProc = DefWindowProcA;
                wc.hInstance = GetModuleHandleA(nullptr);
                wc.lpszClassName = "SparkGLBootstrap";
                RegisterClassA(&wc);

                HWND bootstrapWindow = CreateWindowExA(0, wc.lpszClassName, "SparkGLBootstrap", 0, 0, 0, 1, 1, nullptr,
                                                       nullptr, wc.hInstance, nullptr);
                if (!bootstrapWindow)
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Failed to create bootstrap window");
                    return false;
                }

                HDC bootstrapDC = GetDC(bootstrapWindow);

                PIXELFORMATDESCRIPTOR pfd = {};
                pfd.nSize = sizeof(pfd);
                pfd.nVersion = 1;
                pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
                pfd.iPixelType = PFD_TYPE_RGBA;
                pfd.cColorBits = 32;
                pfd.cDepthBits = 24;
                pfd.cStencilBits = 8;

                int pixelFormat = ChoosePixelFormat(bootstrapDC, &pfd);
                if (!pixelFormat || !SetPixelFormat(bootstrapDC, pixelFormat, &pfd))
                {
                    // Fallback: try simpler pixel format (16-bit color, no stencil)
                    SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                                   "Preferred pixel format failed — trying 16-bit fallback");
                    pfd.cColorBits = 16;
                    pfd.cDepthBits = 16;
                    pfd.cStencilBits = 0;
                    pixelFormat = ChoosePixelFormat(bootstrapDC, &pfd);
                    if (!pixelFormat || !SetPixelFormat(bootstrapDC, pixelFormat, &pfd))
                    {
                        SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                        "Failed to set pixel format on bootstrap context (both 32-bit and 16-bit)");
                        ReleaseDC(bootstrapWindow, bootstrapDC);
                        DestroyWindow(bootstrapWindow);
                        UnregisterClassA(wc.lpszClassName, wc.hInstance);
                        return false;
                    }
                    SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                                   "OpenGL running in degraded mode (16-bit color, no stencil)");
                }

                HGLRC bootstrapContext = wglCreateContext(bootstrapDC);
                if (!bootstrapContext)
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Failed to create bootstrap GL context");
                    ReleaseDC(bootstrapWindow, bootstrapDC);
                    DestroyWindow(bootstrapWindow);
                    UnregisterClassA(wc.lpszClassName, wc.hInstance);
                    return false;
                }

                wglMakeCurrent(bootstrapDC, bootstrapContext);

                // wglCreateContext only yields a legacy (compatibility, non-debug) context. Use it to
                // reach WGL_ARB_create_context and create the real 4.5 core context — with the debug
                // flag when the debug layer is requested, matching the EGL/GLX paths.
                {
                    constexpr int kWglContextMajorVersion = 0x2091;   // WGL_CONTEXT_MAJOR_VERSION_ARB
                    constexpr int kWglContextMinorVersion = 0x2092;   // WGL_CONTEXT_MINOR_VERSION_ARB
                    constexpr int kWglContextFlags = 0x2094;          // WGL_CONTEXT_FLAGS_ARB
                    constexpr int kWglContextProfileMask = 0x9126;    // WGL_CONTEXT_PROFILE_MASK_ARB
                    constexpr int kWglContextDebugBit = 0x0001;       // WGL_CONTEXT_DEBUG_BIT_ARB
                    constexpr int kWglContextCoreProfileBit = 0x0001; // WGL_CONTEXT_CORE_PROFILE_BIT_ARB
                    using PFNWGLCREATECONTEXTATTRIBSARB = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
                    auto wglCreateContextAttribsARB = reinterpret_cast<PFNWGLCREATECONTEXTATTRIBSARB>(
                        reinterpret_cast<void*>(wglGetProcAddress("wglCreateContextAttribsARB")));

                    HGLRC coreContext = nullptr;
                    if (wglCreateContextAttribsARB)
                    {
                        const int attribs[] = {kWglContextMajorVersion,
                                               4,
                                               kWglContextMinorVersion,
                                               5,
                                               kWglContextProfileMask,
                                               kWglContextCoreProfileBit,
                                               kWglContextFlags,
                                               m_debugEnabled ? kWglContextDebugBit : 0,
                                               0};
                        coreContext = wglCreateContextAttribsARB(bootstrapDC, nullptr, attribs);
                    }

                    if (coreContext && wglMakeCurrent(bootstrapDC, coreContext))
                    {
                        wglDeleteContext(bootstrapContext);
                        bootstrapContext = coreContext;
                    }
                    else
                    {
                        if (coreContext)
                            wglDeleteContext(coreContext);
                        wglMakeCurrent(bootstrapDC, bootstrapContext);
                        SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                                       "WGL_ARB_create_context unavailable — using a legacy GL context");
                    }
                }

                // The hidden window's context stays current as the device's rendering context (as on
                // EGL/GLX) until Shutdown; a windowed swap chain rebinds it to the application window.
                m_wglWindow = bootstrapWindow;
                m_wglDC = bootstrapDC;
                m_wglContext = bootstrapContext;
#elif defined(__APPLE__)
                // There is no headless CGL bootstrap on macOS. Without a current context Apple's GL
                // dispatch dereferences null inside glGetString (so gladLoadGL crashes instead of
                // failing); require the host (SDL2) to have made a context current first. CGL is
                // resolved at runtime from the framework GLAD dlopens, so no extra link dependency.
                bool hasCurrentCglContext = false;
                if (void* openGLFramework =
                        dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY | RTLD_LOCAL))
                {
                    using CGLGetCurrentContextFn = void* (*)();
                    auto cglGetCurrentContext =
                        reinterpret_cast<CGLGetCurrentContextFn>(dlsym(openGLFramework, "CGLGetCurrentContext"));
                    hasCurrentCglContext = cglGetCurrentContext && cglGetCurrentContext() != nullptr;
                    dlclose(openGLFramework);
                }
                if (!hasCurrentCglContext)
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                    "GLDevice: no current CGL context (macOS has no headless GL bootstrap)");
                    return false;
                }
#endif

                // GLAD can now load OpenGL function pointers from the current context
                if (!gladLoadGL() || !HasRequiredGLVersion())
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "gladLoadGL failed — no valid GL context");
#if defined(__linux__) && defined(SPARK_EGL_SUPPORT)
                    eglMakeCurrent(m_bootstrapDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                    eglDestroyContext(m_bootstrapDisplay, m_bootstrapContext);
                    if (m_bootstrapSurface != EGL_NO_SURFACE)
                        eglDestroySurface(m_bootstrapDisplay, m_bootstrapSurface);
                    eglTerminate(m_bootstrapDisplay);
#elif defined(__linux__) && !defined(SPARK_EGL_SUPPORT)
                    glXMakeCurrent(m_glxDisplay, 0, nullptr);
                    glXDestroyContext(m_glxDisplay, m_glxContext);
                    glXDestroyPbuffer(m_glxDisplay, m_glxPbuffer);
                    XCloseDisplay(m_glxDisplay);
#elif defined(_WIN32)
                    DestroyWGLContext();
#endif
                    return false;
                }

                if (m_debugEnabled)
                {
                    glEnable(GL_DEBUG_OUTPUT);
                    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);

                    // Register debug message callback for driver error/warning reporting
                    glDebugMessageCallback(
                        [](GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei /*length*/,
                           const GLchar* message, const void* /*userParam*/)
                        {
                            // Skip verbose notifications to avoid log spam
                            if (severity == GL_DEBUG_SEVERITY_NOTIFICATION)
                                return;

                            const char* severityStr = "INFO";
                            if (severity == GL_DEBUG_SEVERITY_HIGH)
                                severityStr = "HIGH";
                            else if (severity == GL_DEBUG_SEVERITY_MEDIUM)
                                severityStr = "MEDIUM";
                            else if (severity == GL_DEBUG_SEVERITY_LOW)
                                severityStr = "LOW";

                            SPARK_LOG_WARN(Spark::LogCategory::Graphics, "GL Debug [%s] src=%u type=%u id=%u: %s",
                                           severityStr, source, type, id, message);
                        },
                        nullptr);

                    // Enable all debug messages except notifications
                    glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_NOTIFICATION, 0, nullptr,
                                          GL_FALSE);
                    glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_LOW, 0, nullptr, GL_TRUE);
                    glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_MEDIUM, 0, nullptr, GL_TRUE);
                    glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_HIGH, 0, nullptr, GL_TRUE);
                }

                QueryCapabilities();

                // The bootstrap context stays alive as the device's rendering context on every
                // platform: headless rendering uses FBOs, and resources created before a swap chain
                // exists must land in a live context. (Windows used to delete it here, which left
                // every later GL call with no current context.)
#if defined(__linux__) && defined(SPARK_EGL_SUPPORT)
                SPARK_LOG_INFO(Spark::LogCategory::Graphics,
                               "EGL headless: keeping bootstrap context as rendering context");
#elif defined(__linux__)
                SPARK_LOG_INFO(Spark::LogCategory::Graphics, "GLX: keeping bootstrap context as rendering context");
#elif defined(_WIN32)
                SPARK_LOG_INFO(Spark::LogCategory::Graphics, "WGL: keeping bootstrap context as rendering context");
#endif

                m_immediateCommandList = std::make_unique<GLCommandList>(true, &m_statistics);

                SPARK_LOG_INFO(Spark::LogCategory::Graphics, "GLDevice initialized: %s (%s)",
                               m_capabilities.deviceName.c_str(), m_capabilities.apiVersion.c_str());

                // Phase Z Theme 3B: wire the transient vertex/index allocator
                // into the lifecycle after the GL context is ready.
                m_transientBuffers.Initialize(this);

                return true;
            }

            void GLDevice::Shutdown()
            {
                SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
                // Idempotent: GraphicsEngine calls Shutdown explicitly, and the destructor
                // calls it again. Without this guard we log "GLDevice::Shutdown" twice and
                // risk re-closing the GLX display on ownership paths.
                if (m_shutdownCalled)
                    return;
                m_shutdownCalled = true;
                SPARK_LOG_INFO(Spark::LogCategory::Graphics, "GLDevice::Shutdown");
                // Phase Z Theme 3B: release transient allocator buffers before
                // the GL context becomes invalid.
                m_transientBuffers.Shutdown(this);
                m_immediateCommandList.reset();

#if defined(__linux__) && !defined(SPARK_EGL_SUPPORT)
                if (m_glxDisplay)
                {
                    if (m_ownsGLXContext)
                    {
                        // Only destroy resources we created (GLX bootstrap path).
                        // When SDL2 created the context, SDL2 owns it and will destroy it.
                        glXMakeCurrent(m_glxDisplay, 0, nullptr);
                        if (m_glxContext)
                            glXDestroyContext(m_glxDisplay, m_glxContext);
                        if (m_glxPbuffer)
                            glXDestroyPbuffer(m_glxDisplay, m_glxPbuffer);
                    }
                    XCloseDisplay(m_glxDisplay);
                    m_glxDisplay = nullptr;
                    m_glxContext = nullptr;
                    m_glxPbuffer = 0;
                }
#elif defined(__linux__) && defined(SPARK_EGL_SUPPORT)
                if (m_bootstrapDisplay != EGL_NO_DISPLAY)
                {
                    if (m_ownsEglContext)
                    {
                        // Only destroy resources we created (EGL bootstrap path).
                        // When the host (SDL2) created the context, it owns it and
                        // will destroy it — we must not call eglTerminate on a
                        // display we didn't initialize.
                        eglMakeCurrent(m_bootstrapDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                        if (m_bootstrapContext != EGL_NO_CONTEXT)
                            eglDestroyContext(m_bootstrapDisplay, m_bootstrapContext);
                        if (m_bootstrapSurface != EGL_NO_SURFACE)
                            eglDestroySurface(m_bootstrapDisplay, m_bootstrapSurface);
                        eglTerminate(m_bootstrapDisplay);
                    }
                    m_bootstrapDisplay = EGL_NO_DISPLAY;
                    m_bootstrapContext = EGL_NO_CONTEXT;
                    m_bootstrapSurface = EGL_NO_SURFACE;
                }
#elif defined(_WIN32)
                DestroyWGLContext();
#endif
            }

#if defined(_WIN32)
            void GLDevice::DestroyWGLContext()
            {
                if (m_wglContext)
                {
                    if (wglGetCurrentContext() == m_wglContext)
                        wglMakeCurrent(nullptr, nullptr);
                    wglDeleteContext(m_wglContext);
                    m_wglContext = nullptr;
                }
                if (m_wglWindow)
                {
                    if (m_wglDC)
                        ReleaseDC(m_wglWindow, m_wglDC);
                    DestroyWindow(m_wglWindow);
                    // Fails harmlessly while another GLDevice still has a window of this class.
                    UnregisterClassA("SparkGLBootstrap", GetModuleHandleA(nullptr));
                }
                m_wglDC = nullptr;
                m_wglWindow = nullptr;
            }
#endif

            void GLDevice::QueryCapabilities()
            {
                m_capabilities.deviceName = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
                m_capabilities.vendorName = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
                m_capabilities.apiVersion = reinterpret_cast<const char*>(glGetString(GL_VERSION));

                GLint maxTextureSize;
                glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
                m_capabilities.maxTextureSize = maxTextureSize;

                GLint maxColorAttachments;
                glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &maxColorAttachments);
                m_capabilities.maxRenderTargets = maxColorAttachments;

                GLint maxSamplers;
                glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &maxSamplers);
                m_capabilities.maxSamplers = maxSamplers;

                // GL_MAX_TEXTURE_MAX_ANISOTROPY is core only in 4.6; 4.5 contexts need the extension
                m_hasAnisotropicFiltering = GLAD_GL_VERSION_4_6 || GLAD_GL_ARB_texture_filter_anisotropic ||
                                            GLAD_GL_EXT_texture_filter_anisotropic;
                GLfloat maxAniso = 1.0f;
                if (m_hasAnisotropicFiltering)
                    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maxAniso);
                m_capabilities.maxAnisotropy = maxAniso;

                // Since GL 3.3 the GLSL version equals the context version (4.5 -> 450); shaders
                // declaring a newer #version are clamped in PrepareGLSLSource
                GLint glMajor = 0;
                GLint glMinor = 0;
                glGetIntegerv(GL_MAJOR_VERSION, &glMajor);
                glGetIntegerv(GL_MINOR_VERSION, &glMinor);
                if (glMajor >= 4 || (glMajor == 3 && glMinor >= 3))
                    m_maxGLSLVersion = glMajor * 100 + glMinor * 10;

                m_capabilities.tessellationSupport = true;
                m_capabilities.computeShaderSupport = true;
                m_capabilities.geometryShaderSupport = true;
                m_capabilities.multiDrawIndirectSupport = true; // GL 4.3+ core (GL_ARB_multi_draw_indirect)
                m_capabilities.maxConstantBuffers = 14;

                // llvmpipe/softpipe renderer strings identify software rasterizers.
                const std::string rendererLower = m_capabilities.deviceName;
                const bool isLlvmPipe = rendererLower.find("llvmpipe") != std::string::npos;
                const bool isSoftPipe = rendererLower.find("softpipe") != std::string::npos;
                m_capabilities.isSoftwareDevice = isLlvmPipe || isSoftPipe;

                // OpenGL has no hardware RT pipeline; compute path can still drive SDFGI.
                m_capabilities.rayTracing.bestBackend = m_capabilities.computeShaderSupport
                                                            ? RayTracingBackend::Software_SDFGI
                                                            : RayTracingBackend::Disabled;

                // Query actual max MSAA sample count
                GLint maxSamples = 8;
                glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
                m_capabilities.maxMSAASamples = static_cast<uint32_t>(maxSamples);

                FinalizeDeviceCapabilities(m_capabilities);
            }

            std::unique_ptr<IRHISwapChain> GLDevice::CreateSwapChain(const RHISwapChainDesc& desc)
            {
#if defined(_WIN32)
                return std::make_unique<GLSwapChain>(desc, m_wglDC, m_wglContext);
#else
                return std::make_unique<GLSwapChain>(desc);
#endif
            }

            std::unique_ptr<IRHIBuffer> GLDevice::CreateBuffer(const RHIBufferDesc& desc)
            {
                GLuint buffer;
                glCreateBuffers(1, &buffer);

                GLenum usage = GL_STATIC_DRAW;
                GLbitfield flags = 0;

                switch (desc.access)
                {
                case RHIBufferAccess::Static:
                    usage = GL_STATIC_DRAW;
                    break;
                case RHIBufferAccess::Dynamic:
                    // DYNAMIC_STORAGE: UpdateBuffer (glNamedBufferSubData) is the common constant-buffer
                    // path. PERSISTENT: TransientBufferAllocator draws while the buffer stays mapped.
                    usage = GL_DYNAMIC_DRAW;
                    flags = GL_DYNAMIC_STORAGE_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
                    break;
                case RHIBufferAccess::Staging:
                    flags = GL_DYNAMIC_STORAGE_BIT | GL_MAP_READ_BIT | GL_MAP_WRITE_BIT;
                    break;
                case RHIBufferAccess::ReadBack:
                    flags = GL_MAP_READ_BIT;
                    break;
                }

                if (flags != 0)
                {
                    glNamedBufferStorage(buffer, desc.size, desc.initialData, flags);
                }
                else
                {
                    glNamedBufferData(buffer, desc.size, desc.initialData, usage);
                }

                return std::make_unique<GLBuffer>(desc, buffer);
            }

            std::unique_ptr<IRHITexture> GLDevice::CreateTexture(const RHITextureDesc& desc)
            {
                GLenum target = GetTextureTarget(desc);

                GLuint texture;
                glCreateTextures(target, 1, &texture);

                GLenum internalFormat = ConvertInternalFormat(desc.format);

                switch (desc.type)
                {
                case RHITextureType::Texture1D:
                    glTextureStorage1D(texture, desc.mipLevels, internalFormat, desc.width);
                    break;
                case RHITextureType::Texture3D:
                    glTextureStorage3D(texture, desc.mipLevels, internalFormat, desc.width, desc.height, desc.depth);
                    break;
                case RHITextureType::TextureCube:
                    glTextureStorage2D(texture, desc.mipLevels, internalFormat, desc.width, desc.height);
                    break;
                case RHITextureType::Texture2DArray:
                    glTextureStorage3D(texture, desc.mipLevels, internalFormat, desc.width, desc.height,
                                       desc.arraySize);
                    break;
                case RHITextureType::TextureCubeArray:
                    glTextureStorage3D(texture, desc.mipLevels, internalFormat, desc.width, desc.height,
                                       desc.arraySize * 6);
                    break;
                case RHITextureType::Texture2D:
                default:
                    if (desc.sampleCount > 1)
                    {
                        glTextureStorage2DMultisample(texture, desc.sampleCount, internalFormat, desc.width,
                                                      desc.height, GL_TRUE);
                    }
                    else
                    {
                        glTextureStorage2D(texture, desc.mipLevels, internalFormat, desc.width, desc.height);
                    }
                    break;
                }

                // Create framebuffer if render target or depth stencil
                GLuint fbo = 0;
                if (desc.usage & RHITextureUsage::RenderTarget)
                {
                    glCreateFramebuffers(1, &fbo);
                    glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0, texture, 0);

                    GLenum drawBuffers[] = {GL_COLOR_ATTACHMENT0};
                    glNamedFramebufferDrawBuffers(fbo, 1, drawBuffers);

                    if (glCheckNamedFramebufferStatus(fbo, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                    {
                        SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                        "OpenGL framebuffer incomplete for render target: %s", desc.debugName.c_str());
                    }
                }
                else if (desc.usage & RHITextureUsage::DepthStencil)
                {
                    glCreateFramebuffers(1, &fbo);
                    GLenum attachment = GetDepthAttachmentType(desc.format);
                    glNamedFramebufferTexture(fbo, attachment, texture, 0);
                }

                return std::make_unique<GLTexture>(desc, texture, fbo, target);
            }

            std::unique_ptr<IRHITexture> GLDevice::WrapNativeTexture(void* nativeHandle, const RHITextureDesc& desc)
            {
                if (!nativeHandle)
                    return nullptr;
                // Interpret as OpenGL texture name (GLuint stored in pointer).
                // Per the RHIDevice contract the wrapper does NOT own the resource.
                return std::make_unique<GLTexture>(desc, static_cast<GLuint>(reinterpret_cast<uintptr_t>(nativeHandle)),
                                                   0, GL_TEXTURE_2D, false);
            }

            std::unique_ptr<IRHIShader> GLDevice::CreateShader(const RHIShaderDesc& desc)
            {
                GLenum shaderType;
                switch (desc.stage)
                {
                case RHIShaderStage::Vertex:
                    shaderType = GL_VERTEX_SHADER;
                    break;
                case RHIShaderStage::Pixel:
                    shaderType = GL_FRAGMENT_SHADER;
                    break;
                case RHIShaderStage::Geometry:
                    shaderType = GL_GEOMETRY_SHADER;
                    break;
                case RHIShaderStage::Hull:
                    shaderType = GL_TESS_CONTROL_SHADER;
                    break;
                case RHIShaderStage::Domain:
                    shaderType = GL_TESS_EVALUATION_SHADER;
                    break;
                case RHIShaderStage::Compute:
                    shaderType = GL_COMPUTE_SHADER;
                    break;
                default:
                    return nullptr;
                }

                GLuint shader = glCreateShader(shaderType);

                if (desc.language == ShaderLanguage::SPIRV && desc.bytecode && desc.bytecodeSize > 0)
                {
                    // SPIR-V shader loading (GL_ARB_gl_spirv)
                    glShaderBinary(1, &shader, GL_SHADER_BINARY_FORMAT_SPIR_V, desc.bytecode,
                                   static_cast<GLsizei>(desc.bytecodeSize));

                    GLenum binaryErr = glGetError();
                    if (binaryErr != GL_NO_ERROR)
                    {
                        SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                        "OpenGL SPIR-V binary load failed (GL error 0x%X)", binaryErr);
                        glDeleteShader(shader);
                        return nullptr;
                    }

                    glSpecializeShader(shader, desc.entryPoint.c_str(), 0, nullptr, nullptr);

                    // Check specialization/compilation status
                    GLint specSuccess;
                    glGetShaderiv(shader, GL_COMPILE_STATUS, &specSuccess);
                    if (!specSuccess)
                    {
                        char infoLog[1024];
                        glGetShaderInfoLog(shader, sizeof(infoLog), nullptr, infoLog);
                        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "OpenGL SPIR-V specialization failed: %s",
                                        infoLog);
                        glDeleteShader(shader);
                        return nullptr;
                    }
                }
                else if (!desc.sourceCode.empty())
                {
                    // GLSL source compilation (stage macro, defines, #version clamp applied)
                    const std::string prepared = PrepareGLSLSource(desc);
                    const char* src = prepared.c_str();
                    glShaderSource(shader, 1, &src, nullptr);
                    glCompileShader(shader);

                    GLint success;
                    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
                    if (!success)
                    {
                        char infoLog[1024];
                        glGetShaderInfoLog(shader, sizeof(infoLog), nullptr, infoLog);
                        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "OpenGL shader compilation failed: %s", infoLog);
                        glDeleteShader(shader);
                        return nullptr;
                    }
                }
                else
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                    "OpenGL CreateShader: no source code or SPIR-V bytecode provided");
                    glDeleteShader(shader);
                    return nullptr;
                }

                return std::make_unique<GLShader>(desc, shader, desc.sourceCode);
            }

            std::string GLDevice::PrepareGLSLSource(const RHIShaderDesc& desc) const
            {
                // Shipped multi-stage files select their stage with #ifdef VERTEX_SHADER / FRAGMENT_SHADER,
                // and variants with desc.defines ("NAME" or "NAME=VALUE"); both must follow #version.
                const char* stageMacro = nullptr;
                switch (desc.stage)
                {
                case RHIShaderStage::Vertex:
                    stageMacro = "VERTEX_SHADER";
                    break;
                case RHIShaderStage::Pixel:
                    stageMacro = "FRAGMENT_SHADER";
                    break;
                case RHIShaderStage::Geometry:
                    stageMacro = "GEOMETRY_SHADER";
                    break;
                case RHIShaderStage::Hull:
                    stageMacro = "TESS_CONTROL_SHADER";
                    break;
                case RHIShaderStage::Domain:
                    stageMacro = "TESS_EVALUATION_SHADER";
                    break;
                case RHIShaderStage::Compute:
                    stageMacro = "COMPUTE_SHADER";
                    break;
                default:
                    break;
                }

                std::string preamble;
                if (stageMacro)
                    preamble += std::string("#define ") + stageMacro + "\n";
                for (const std::string& define : desc.defines)
                {
                    const size_t eq = define.find('=');
                    if (eq == std::string::npos)
                        preamble += "#define " + define + "\n";
                    else
                        preamble += "#define " + define.substr(0, eq) + " " + define.substr(eq + 1) + "\n";
                }

                const std::string& source = desc.sourceCode;
                const size_t versionPos = source.find("#version");
                if (versionPos == std::string::npos)
                    return preamble + source;

                size_t lineEnd = source.find('\n', versionPos);
                if (lineEnd == std::string::npos)
                    lineEnd = source.size();
                std::string versionLine = source.substr(versionPos, lineEnd - versionPos);

                // Mesa llvmpipe and other 4.5 drivers reject "#version 460". The shipped shaders use
                // no 4.6-only features, so clamp; genuine 4.6 usage still fails to compile explicitly.
                std::istringstream versionStream(versionLine.substr(8));
                int requested = 0;
                std::string profile;
                versionStream >> requested >> profile;
                if (requested > m_maxGLSLVersion)
                {
                    SPARK_LOG_INFO(Spark::LogCategory::Graphics,
                                   "GL: clamping GLSL #version %d to %d for '%s' (driver maximum)", requested,
                                   m_maxGLSLVersion,
                                   desc.debugName.empty() ? desc.filePath.c_str() : desc.debugName.c_str());
                    versionLine =
                        "#version " + std::to_string(m_maxGLSLVersion) + (profile.empty() ? "" : " " + profile);
                }

                const size_t afterVersion = lineEnd < source.size() ? lineEnd + 1 : lineEnd;
                return source.substr(0, versionPos) + versionLine + "\n" + preamble + "#line 2\n" +
                       source.substr(afterVersion);
            }

            std::unique_ptr<IRHISampler> GLDevice::CreateSampler(const RHISamplerDesc& desc)
            {
                GLuint sampler;
                glCreateSamplers(1, &sampler);

                // Min filter
                GLenum minFilter;
                if (desc.minFilter == RHIFilterMode::Nearest)
                    minFilter = (desc.mipFilter == RHIFilterMode::Nearest) ? GL_NEAREST_MIPMAP_NEAREST
                                                                           : GL_NEAREST_MIPMAP_LINEAR;
                else
                    minFilter =
                        (desc.mipFilter == RHIFilterMode::Nearest) ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR;

                glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, minFilter);
                glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER,
                                    desc.magFilter == RHIFilterMode::Nearest ? GL_NEAREST : GL_LINEAR);

                glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, ConvertAddressMode(desc.addressU));
                glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, ConvertAddressMode(desc.addressV));
                glSamplerParameteri(sampler, GL_TEXTURE_WRAP_R, ConvertAddressMode(desc.addressW));

                glSamplerParameterf(sampler, GL_TEXTURE_LOD_BIAS, desc.mipLodBias);
                glSamplerParameterf(sampler, GL_TEXTURE_MIN_LOD, desc.minLod);
                glSamplerParameterf(sampler, GL_TEXTURE_MAX_LOD, desc.maxLod);

                if (m_hasAnisotropicFiltering &&
                    (desc.minFilter == RHIFilterMode::Anisotropic || desc.magFilter == RHIFilterMode::Anisotropic))
                {
                    // GL rejects values below 1 (D3D callers often leave 0 meaning "off")
                    const float aniso = std::clamp(static_cast<float>(desc.maxAnisotropy), 1.0f,
                                                   std::max(1.0f, m_capabilities.maxAnisotropy));
                    glSamplerParameterf(sampler, GL_TEXTURE_MAX_ANISOTROPY, aniso);
                }

                glSamplerParameterfv(sampler, GL_TEXTURE_BORDER_COLOR, desc.borderColor);

                return std::make_unique<GLSampler>(desc, sampler);
            }

            std::unique_ptr<IRHIPipelineState> GLDevice::CreatePipelineState(const RHIPipelineStateDesc& desc,
                                                                             IRHIShader* vertexShader,
                                                                             IRHIShader* pixelShader)
            {
                if (!vertexShader || !pixelShader)
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                    "OpenGL CreatePipelineState: null shader (VS=%p, PS=%p)",
                                    static_cast<void*>(vertexShader), static_cast<void*>(pixelShader));
                    return nullptr;
                }

                auto* glVS = static_cast<GLShader*>(vertexShader);
                auto* glPS = static_cast<GLShader*>(pixelShader);

                // Create and link program
                GLuint program = glCreateProgram();
                glAttachShader(program, glVS->GetGLShader());
                glAttachShader(program, glPS->GetGLShader());
                glLinkProgram(program);

                GLint success;
                glGetProgramiv(program, GL_LINK_STATUS, &success);
                if (!success)
                {
                    char infoLog[1024];
                    glGetProgramInfoLog(program, sizeof(infoLog), nullptr, infoLog);
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "OpenGL program link failed: %s", infoLog);
                    glDeleteProgram(program);
                    return nullptr;
                }

                // Create VAO from input layout
                GLuint vao;
                glCreateVertexArrays(1, &vao);

                std::array<uint32_t, kMaxVertexBufferSlots> slotStrides{};
                for (size_t i = 0; i < desc.inputLayout.elements.size(); ++i)
                {
                    const auto& elem = desc.inputLayout.elements[i];
                    const GLuint index = static_cast<GLuint>(i);

                    GLint numComponents = 3;
                    GLenum type = GL_FLOAT;
                    bool isInteger = false;
                    GLboolean normalized = GL_FALSE;
                    switch (elem.format)
                    {
                    case RHIVertexFormat::Float1:
                    case RHIVertexFormat::Float2:
                    case RHIVertexFormat::Float3:
                    case RHIVertexFormat::Float4:
                        numComponents =
                            1 + static_cast<GLint>(elem.format) - static_cast<GLint>(RHIVertexFormat::Float1);
                        break;
                    case RHIVertexFormat::Int1:
                    case RHIVertexFormat::Int2:
                    case RHIVertexFormat::Int3:
                    case RHIVertexFormat::Int4:
                        numComponents = 1 + static_cast<GLint>(elem.format) - static_cast<GLint>(RHIVertexFormat::Int1);
                        type = GL_INT;
                        isInteger = true;
                        break;
                    case RHIVertexFormat::UInt1:
                    case RHIVertexFormat::UInt2:
                    case RHIVertexFormat::UInt3:
                    case RHIVertexFormat::UInt4:
                        numComponents =
                            1 + static_cast<GLint>(elem.format) - static_cast<GLint>(RHIVertexFormat::UInt1);
                        type = GL_UNSIGNED_INT;
                        isInteger = true;
                        break;
                    case RHIVertexFormat::UNorm8x4:
                        numComponents = 4;
                        type = GL_UNSIGNED_BYTE;
                        normalized = GL_TRUE;
                        break;
                    case RHIVertexFormat::SNorm8x4:
                        numComponents = 4;
                        type = GL_BYTE;
                        normalized = GL_TRUE;
                        break;
                    }
                    const uint32_t elemSize =
                        (type == GL_UNSIGNED_BYTE || type == GL_BYTE) ? 4u : 4u * static_cast<uint32_t>(numComponents);

                    if (elem.inputSlot >= kMaxVertexBufferSlots)
                    {
                        SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                        "OpenGL CreatePipelineState: input slot %u exceeds %u ('%s')", elem.inputSlot,
                                        kMaxVertexBufferSlots, elem.semanticName.c_str());
                        glDeleteVertexArrays(1, &vao);
                        glDeleteProgram(program);
                        return nullptr;
                    }
                    slotStrides[elem.inputSlot] = std::max(slotStrides[elem.inputSlot], elem.byteOffset + elemSize);

                    glEnableVertexArrayAttrib(vao, index);
                    glVertexArrayAttribBinding(vao, index, elem.inputSlot);
                    // Integer inputs (ivec/uvec in GLSL) must use the I-format entry point or the
                    // shader reads float-converted garbage
                    if (isInteger)
                        glVertexArrayAttribIFormat(vao, index, numComponents, type, elem.byteOffset);
                    else
                        glVertexArrayAttribFormat(vao, index, numComponents, type, normalized, elem.byteOffset);

                    // GL divisor 0 means per-vertex, so a per-instance element always steps at least once
                    if (elem.perInstance)
                        glVertexArrayBindingDivisor(vao, elem.inputSlot, std::max(1u, elem.instanceStepRate));
                }

                auto pipeline = std::make_unique<GLPipelineState>(desc, program, vao);
                pipeline->SetSlotStrides(slotStrides);
                return pipeline;
            }


            void* GLDevice::MapBuffer(IRHIBuffer* buffer)
            {
                auto* glBuf = static_cast<GLBuffer*>(buffer);
                // Map access must be a subset of the immutable storage flags chosen in CreateBuffer
                GLbitfield access = GL_MAP_WRITE_BIT;
                switch (glBuf->GetDesc().access)
                {
                case RHIBufferAccess::Dynamic:
                    access = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
                    break;
                case RHIBufferAccess::Staging:
                    access = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT;
                    break;
                case RHIBufferAccess::ReadBack:
                    access = GL_MAP_READ_BIT;
                    break;
                case RHIBufferAccess::Static:
                    break;
                }
                void* mapped =
                    glMapNamedBufferRange(glBuf->GetGLBuffer(), 0, static_cast<GLsizeiptr>(glBuf->GetSize()), access);
                if (!mapped)
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                    "GLDevice::MapBuffer: glMapNamedBuffer returned null");
                }
                return mapped;
            }

            void GLDevice::UnmapBuffer(IRHIBuffer* buffer)
            {
                auto* glBuf = static_cast<GLBuffer*>(buffer);
                glUnmapNamedBuffer(glBuf->GetGLBuffer());
            }

            void GLDevice::UpdateBuffer(IRHIBuffer* buffer, const void* data, size_t size, size_t offset)
            {
                auto* glBuf = static_cast<GLBuffer*>(buffer);
                glNamedBufferSubData(glBuf->GetGLBuffer(), offset, size, data);
            }

            void GLDevice::UpdateTexture(IRHITexture* texture, const void* data, uint32_t mipLevel, uint32_t arraySlice)
            {
                if (!texture || !data)
                    return;
                auto* glTex = static_cast<GLTexture*>(texture);
                const PixelFormat pixelFormat = glTex->GetFormat();
                const RHITextureType type = glTex->GetDesc().type;

                const uint32_t w = std::max(1u, glTex->GetWidth() >> mipLevel);
                const uint32_t h = std::max(1u, glTex->GetHeight() >> mipLevel);
                // Array layers and cube faces are the z coordinate of a DSA 3D upload
                const bool layered = type == RHITextureType::Texture2DArray || type == RHITextureType::TextureCube ||
                                     type == RHITextureType::TextureCubeArray;
                const GLint layer = layered ? static_cast<GLint>(arraySlice) : 0;

                // Rows of RHI uploads are tightly packed; GL's default 4-byte alignment skews
                // odd-width R8/RG8 rows
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

                if (IsCompressedFormat(pixelFormat))
                {
                    const GLsizei imageSize =
                        static_cast<GLsizei>(((w + 3) / 4) * ((h + 3) / 4) * GetFormatSize(pixelFormat));
                    const GLenum internalFormat = ConvertInternalFormat(pixelFormat);
                    if (layered)
                        glCompressedTextureSubImage3D(glTex->GetGLTexture(), mipLevel, 0, 0, layer, w, h, 1,
                                                      internalFormat, imageSize, data);
                    else
                        glCompressedTextureSubImage2D(glTex->GetGLTexture(), mipLevel, 0, 0, w, h, internalFormat,
                                                      imageSize, data);
                }
                else
                {
                    const GLenum format = ConvertFormat(pixelFormat);
                    const GLenum formatType = ConvertFormatType(pixelFormat);
                    if (layered)
                        glTextureSubImage3D(glTex->GetGLTexture(), mipLevel, 0, 0, layer, w, h, 1, format, formatType,
                                            data);
                    else
                        glTextureSubImage2D(glTex->GetGLTexture(), mipLevel, 0, 0, w, h, format, formatType, data);
                }
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            }

            void GLDevice::GenerateMips(IRHITexture* texture)
            {
                if (!texture)
                    return;
                auto* glTex = static_cast<GLTexture*>(texture);
                glGenerateTextureMipmap(glTex->GetGLTexture());
            }

            IRHICommandList* GLDevice::GetImmediateCommandList()
            {
                return m_immediateCommandList.get();
            }

            std::unique_ptr<IRHICommandList> GLDevice::CreateDeferredCommandList()
            {
                return std::make_unique<GLCommandList>(false, &m_statistics);
            }

            void GLDevice::ExecuteCommandList(IRHICommandList*) {}

            void GLDevice::BeginFrame()
            {
                ResetStatistics();
                // Phase Z Theme 3B: pump the transient allocator each frame.
                m_transientBuffers.BeginFrame(this);
            }
            void GLDevice::EndFrame()
            {
                // Phase Z Theme 3B: release the frame's transient mapping.
                m_transientBuffers.EndFrame(this);
            }
            void GLDevice::WaitForIdle()
            {
                glFinish();
            }

            std::string GLDevice::GetDeviceInfo() const
            {
                std::string info = "=== OpenGL Device Info ===\n";
                info += "Renderer: " + m_capabilities.deviceName + "\n";
                info += "Vendor: " + m_capabilities.vendorName + "\n";
                info += "Version: " + m_capabilities.apiVersion + "\n";
                info += "Max Texture Size: " + std::to_string(m_capabilities.maxTextureSize) + "\n";
                info += "Max Color Attachments: " + std::to_string(m_capabilities.maxRenderTargets) + "\n";
                info += "Max Anisotropy: " + std::to_string(m_capabilities.maxAnisotropy) + "\n";
                return info;
            }

            // ============================================================================
            // FORMAT CONVERSION HELPERS
            // ============================================================================

            GLenum GLDevice::ConvertFormat(PixelFormat format) const
            {
                switch (format)
                {
                case PixelFormat::R8_UNORM:
                case PixelFormat::R8_SNORM:
                    return GL_RED;
                case PixelFormat::R8_UINT:
                    return GL_RED_INTEGER;
                case PixelFormat::R8G8_UNORM:
                    return GL_RG;
                case PixelFormat::R8G8B8A8_UNORM:
                case PixelFormat::R8G8B8A8_UNORM_SRGB:
                case PixelFormat::R8G8B8A8_SNORM:
                    return GL_RGBA;
                case PixelFormat::B8G8R8A8_UNORM:
                case PixelFormat::B8G8R8A8_UNORM_SRGB:
                    return GL_BGRA;
                case PixelFormat::R10G10B10A2_UNORM:
                    return GL_RGBA;
                case PixelFormat::R11G11B10_FLOAT:
                    return GL_RGB;
                case PixelFormat::R16_FLOAT:
                    return GL_RED;
                case PixelFormat::R16_UINT:
                    return GL_RED_INTEGER;
                case PixelFormat::R16G16_FLOAT:
                    return GL_RG;
                case PixelFormat::R16G16B16A16_FLOAT:
                case PixelFormat::R16G16B16A16_UNORM:
                    return GL_RGBA;
                case PixelFormat::R32_FLOAT:
                    return GL_RED;
                case PixelFormat::R32_UINT:
                    return GL_RED_INTEGER;
                case PixelFormat::R32G32_FLOAT:
                    return GL_RG;
                case PixelFormat::R32G32B32_FLOAT:
                    return GL_RGB;
                case PixelFormat::R32G32B32A32_FLOAT:
                    return GL_RGBA;
                case PixelFormat::D16_UNORM:
                case PixelFormat::D32_FLOAT:
                    return GL_DEPTH_COMPONENT;
                case PixelFormat::D24_UNORM_S8_UINT:
                case PixelFormat::D32_FLOAT_S8_UINT:
                    return GL_DEPTH_STENCIL;
                default:
                    return GL_RGBA;
                }
            }

            GLenum GLDevice::ConvertInternalFormat(PixelFormat format) const
            {
                switch (format)
                {
                case PixelFormat::R8_UNORM:
                    return GL_R8;
                case PixelFormat::R8_SNORM:
                    return GL_R8_SNORM;
                case PixelFormat::R8_UINT:
                    return GL_R8UI;
                case PixelFormat::R8G8_UNORM:
                    return GL_RG8;
                case PixelFormat::R8G8B8A8_UNORM:
                    return GL_RGBA8;
                case PixelFormat::R8G8B8A8_UNORM_SRGB:
                    return GL_SRGB8_ALPHA8;
                case PixelFormat::R8G8B8A8_SNORM:
                    return GL_RGBA8_SNORM;
                case PixelFormat::B8G8R8A8_UNORM:
                    return GL_RGBA8; // GL doesn't distinguish BGRA in internal format
                case PixelFormat::B8G8R8A8_UNORM_SRGB:
                    return GL_SRGB8_ALPHA8;
                case PixelFormat::R10G10B10A2_UNORM:
                    return GL_RGB10_A2;
                case PixelFormat::R11G11B10_FLOAT:
                    return GL_R11F_G11F_B10F;
                case PixelFormat::R16_FLOAT:
                    return GL_R16F;
                case PixelFormat::R16_UINT:
                    return GL_R16UI;
                case PixelFormat::R16G16_FLOAT:
                    return GL_RG16F;
                case PixelFormat::R16G16B16A16_FLOAT:
                    return GL_RGBA16F;
                case PixelFormat::R16G16B16A16_UNORM:
                    return GL_RGBA16;
                case PixelFormat::R32_FLOAT:
                    return GL_R32F;
                case PixelFormat::R32_UINT:
                    return GL_R32UI;
                case PixelFormat::R32G32_FLOAT:
                    return GL_RG32F;
                case PixelFormat::R32G32B32_FLOAT:
                    return GL_RGB32F;
                case PixelFormat::R32G32B32A32_FLOAT:
                    return GL_RGBA32F;
                case PixelFormat::BC1_UNORM:
                    return GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
                case PixelFormat::BC1_UNORM_SRGB:
                    return GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT;
                case PixelFormat::BC2_UNORM:
                    return GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
                case PixelFormat::BC3_UNORM:
                    return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
                case PixelFormat::BC3_UNORM_SRGB:
                    return GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT;
                case PixelFormat::BC4_UNORM:
                    return GL_COMPRESSED_RED_RGTC1;
                case PixelFormat::BC5_UNORM:
                    return GL_COMPRESSED_RG_RGTC2;
                case PixelFormat::BC6H_UF16:
                    return GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT;
                case PixelFormat::BC7_UNORM:
                    return GL_COMPRESSED_RGBA_BPTC_UNORM;
                case PixelFormat::BC7_UNORM_SRGB:
                    return GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM;
                case PixelFormat::D16_UNORM:
                    return GL_DEPTH_COMPONENT16;
                case PixelFormat::D24_UNORM_S8_UINT:
                    return GL_DEPTH24_STENCIL8;
                case PixelFormat::D32_FLOAT:
                    return GL_DEPTH_COMPONENT32F;
                case PixelFormat::D32_FLOAT_S8_UINT:
                    return GL_DEPTH32F_STENCIL8;
                default:
                    return GL_RGBA8;
                }
            }

            GLenum GLDevice::ConvertFormatType(PixelFormat format) const
            {
                switch (format)
                {
                case PixelFormat::R8_SNORM:
                case PixelFormat::R8G8B8A8_SNORM:
                    return GL_BYTE;
                case PixelFormat::R8_UNORM:
                case PixelFormat::R8_UINT:
                case PixelFormat::R8G8_UNORM:
                case PixelFormat::R8G8B8A8_UNORM:
                case PixelFormat::R8G8B8A8_UNORM_SRGB:
                case PixelFormat::B8G8R8A8_UNORM:
                case PixelFormat::B8G8R8A8_UNORM_SRGB:
                    return GL_UNSIGNED_BYTE;
                case PixelFormat::R16_FLOAT:
                case PixelFormat::R16G16_FLOAT:
                case PixelFormat::R16G16B16A16_FLOAT:
                    return GL_HALF_FLOAT;
                case PixelFormat::R16_UINT:
                    return GL_UNSIGNED_SHORT;
                case PixelFormat::R16G16B16A16_UNORM:
                    return GL_UNSIGNED_SHORT;
                case PixelFormat::R32_FLOAT:
                case PixelFormat::R32G32_FLOAT:
                case PixelFormat::R32G32B32_FLOAT:
                case PixelFormat::R32G32B32A32_FLOAT:
                case PixelFormat::D32_FLOAT:
                    return GL_FLOAT;
                case PixelFormat::R32_UINT:
                    return GL_UNSIGNED_INT;
                case PixelFormat::R10G10B10A2_UNORM:
                    return GL_UNSIGNED_INT_2_10_10_10_REV;
                case PixelFormat::R11G11B10_FLOAT:
                    return GL_UNSIGNED_INT_10F_11F_11F_REV;
                case PixelFormat::D16_UNORM:
                    return GL_UNSIGNED_SHORT;
                case PixelFormat::D24_UNORM_S8_UINT:
                    return GL_UNSIGNED_INT_24_8;
                case PixelFormat::D32_FLOAT_S8_UINT:
                    return GL_FLOAT_32_UNSIGNED_INT_24_8_REV;
                default:
                    return GL_UNSIGNED_BYTE;
                }
            }

            GLenum GLDevice::ConvertAddressMode(RHIAddressMode mode) const
            {
                switch (mode)
                {
                case RHIAddressMode::Wrap:
                    return GL_REPEAT;
                case RHIAddressMode::Clamp:
                    return GL_CLAMP_TO_EDGE;
                case RHIAddressMode::Mirror:
                    return GL_MIRRORED_REPEAT;
                case RHIAddressMode::Border:
                    return GL_CLAMP_TO_BORDER;
                case RHIAddressMode::MirrorOnce:
                    return GL_MIRROR_CLAMP_TO_EDGE;
                default:
                    return GL_REPEAT;
                }
            }

            GLenum GLDevice::GetTextureTarget(const RHITextureDesc& desc) const
            {
                switch (desc.type)
                {
                case RHITextureType::Texture1D:
                    return GL_TEXTURE_1D;
                case RHITextureType::Texture3D:
                    return GL_TEXTURE_3D;
                case RHITextureType::TextureCube:
                    return GL_TEXTURE_CUBE_MAP;
                case RHITextureType::Texture2DArray:
                    return GL_TEXTURE_2D_ARRAY;
                case RHITextureType::TextureCubeArray:
                    return GL_TEXTURE_CUBE_MAP_ARRAY;
                case RHITextureType::Texture2D:
                default:
                    if (desc.sampleCount > 1)
                        return GL_TEXTURE_2D_MULTISAMPLE;
                    return GL_TEXTURE_2D;
                }
            }

            GLenum GLDevice::GetDepthAttachmentType(PixelFormat format) const
            {
                switch (format)
                {
                case PixelFormat::D24_UNORM_S8_UINT:
                case PixelFormat::D32_FLOAT_S8_UINT:
                    return GL_DEPTH_STENCIL_ATTACHMENT;
                case PixelFormat::D16_UNORM:
                case PixelFormat::D32_FLOAT:
                default:
                    return GL_DEPTH_ATTACHMENT;
                }
            }

            // ============================================================================
            // FACTORY FUNCTION
            // ============================================================================

            std::unique_ptr<IRHIDevice> CreateOpenGLDevice()
            {
                return std::make_unique<GLDevice>();
            }

        } // namespace OpenGL
    } // namespace RHI
} // namespace Spark

#endif // SPARK_OPENGL_SUPPORT
