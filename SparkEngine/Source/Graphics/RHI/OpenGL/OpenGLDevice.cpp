/**
 * @file OpenGLDevice.cpp
 * @brief OpenGL 4.6 Core Profile RHI backend implementation
 * @author Spark Engine Team
 * @date 2025
 *
 * Modern OpenGL implementation using DSA (Direct State Access),
 * SPIR-V shader support, and GL 4.6 Core Profile features.
 */

#ifdef SPARK_OPENGL_SUPPORT

#include "OpenGLDevice.h"
#include <cassert>
#include <cstring>
#include <iostream>

namespace Spark
{
    namespace RHI
    {
        namespace OpenGL
        {

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

            GLTexture::GLTexture(const RHITextureDesc& desc, GLuint texture, GLuint framebuffer)
                : m_desc(desc), m_texture(texture), m_framebuffer(framebuffer)
            {
            }

            GLTexture::~GLTexture()
            {
                if (m_framebuffer != 0)
                {
                    glDeleteFramebuffers(1, &m_framebuffer);
                }
                if (m_texture != 0)
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
                // Depth test
                if (m_desc.depthStencil.depthEnable)
                {
                    glEnable(GL_DEPTH_TEST);

                    GLenum depthFunc = GL_LESS;
                    switch (m_desc.depthStencil.depthFunc)
                    {
                    case RHICompareOp::Never:
                        depthFunc = GL_NEVER;
                        break;
                    case RHICompareOp::Less:
                        depthFunc = GL_LESS;
                        break;
                    case RHICompareOp::Equal:
                        depthFunc = GL_EQUAL;
                        break;
                    case RHICompareOp::LessEqual:
                        depthFunc = GL_LEQUAL;
                        break;
                    case RHICompareOp::Greater:
                        depthFunc = GL_GREATER;
                        break;
                    case RHICompareOp::NotEqual:
                        depthFunc = GL_NOTEQUAL;
                        break;
                    case RHICompareOp::GreaterEqual:
                        depthFunc = GL_GEQUAL;
                        break;
                    case RHICompareOp::Always:
                        depthFunc = GL_ALWAYS;
                        break;
                    }
                    glDepthFunc(depthFunc);
                }
                else
                {
                    glDisable(GL_DEPTH_TEST);
                }

                // Depth write
                glDepthMask(m_desc.depthStencil.depthWrite ? GL_TRUE : GL_FALSE);

                // Stencil test
                if (m_desc.depthStencil.stencilEnable)
                {
                    glEnable(GL_STENCIL_TEST);
                    glStencilMask(m_desc.depthStencil.stencilWriteMask);
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

            GLSwapChain::GLSwapChain(const RHISwapChainDesc& desc) : m_desc(desc)
            {
                // Create a texture wrapper for the default framebuffer
                RHITextureDesc texDesc;
                texDesc.width = desc.width;
                texDesc.height = desc.height;
                texDesc.format = desc.format;
                texDesc.usage = RHITextureUsage::RenderTarget;
                texDesc.debugName = "DefaultFramebuffer";

                m_backBuffer = std::make_unique<GLTexture>(texDesc, 0, 0); // FBO 0 = default

#ifdef _WIN32
                HWND hwnd = static_cast<HWND>(desc.windowHandle);
                m_hdc = GetDC(hwnd);

                PIXELFORMATDESCRIPTOR pfd = {};
                pfd.nSize = sizeof(pfd);
                pfd.nVersion = 1;
                pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
                pfd.iPixelType = PFD_TYPE_RGBA;
                pfd.cColorBits = 32;
                pfd.cDepthBits = 24;
                pfd.cStencilBits = 8;

                int pixelFormat = ChoosePixelFormat(m_hdc, &pfd);
                SetPixelFormat(m_hdc, pixelFormat, &pfd);

                m_hglrc = wglCreateContext(m_hdc);
                wglMakeCurrent(m_hdc, m_hglrc);
#endif
            }

            GLSwapChain::~GLSwapChain()
            {
#ifdef _WIN32
                if (m_hglrc)
                {
                    wglMakeCurrent(nullptr, nullptr);
                    wglDeleteContext(m_hglrc);
                }
#endif
            }

            bool GLSwapChain::Present(bool)
            {
#ifdef _WIN32
                if (m_hdc)
                {
                    SwapBuffers(m_hdc);
                    return true;
                }
#endif
                return false;
            }

            bool GLSwapChain::Resize(uint32_t width, uint32_t height)
            {
                m_desc.width = width;
                m_desc.height = height;
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

            GLCommandList::GLCommandList(bool isImmediate) : m_isImmediate(isImmediate) {}

            void GLCommandList::Begin() {}
            void GLCommandList::End()
            {
                glFlush();
            }
            void GLCommandList::Reset() {}

            void GLCommandList::SetRenderTargets(IRHITexture** renderTargets, uint32_t count, IRHITexture* depthStencil)
            {
                if (count == 0 || !renderTargets[0])
                {
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    return;
                }

                auto* glTex = static_cast<GLTexture*>(renderTargets[0]);
                GLuint fbo = glTex->GetGLFramebuffer();
                glBindFramebuffer(GL_FRAMEBUFFER, fbo);

                // Set draw buffers
                std::vector<GLenum> drawBuffers(count);
                for (uint32_t i = 0; i < count; ++i)
                    drawBuffers[i] = GL_COLOR_ATTACHMENT0 + i;
                glDrawBuffers(count, drawBuffers.data());
            }

            void GLCommandList::ClearRenderTarget(IRHITexture*, const float color[4])
            {
                glClearColor(color[0], color[1], color[2], color[3]);
                glClear(GL_COLOR_BUFFER_BIT);
            }

            void GLCommandList::ClearDepthStencil(IRHITexture*, float depth, uint8_t stencil)
            {
                glClearDepth(depth);
                glClearStencil(stencil);
                glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
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
                    return;
                auto* glPSO = static_cast<GLPipelineState*>(pipelineState);

                m_currentProgram = glPSO->GetGLProgram();
                m_currentVAO = glPSO->GetGLVAO();

                glUseProgram(m_currentProgram);
                glBindVertexArray(m_currentVAO);

                glPSO->ApplyRasterizerState();
                glPSO->ApplyDepthStencilState();
                glPSO->ApplyBlendState();
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

            void GLCommandList::SetVertexBuffer(IRHIBuffer* buffer, uint32_t, uint32_t)
            {
                if (!buffer)
                    return;
                auto* glBuf = static_cast<GLBuffer*>(buffer);
                glBindBuffer(GL_ARRAY_BUFFER, glBuf->GetGLBuffer());
            }

            void GLCommandList::SetIndexBuffer(IRHIBuffer* buffer, uint32_t)
            {
                if (!buffer)
                    return;
                auto* glBuf = static_cast<GLBuffer*>(buffer);
                m_boundIndexBuffer = glBuf->GetGLBuffer();
                m_indexStride = glBuf->GetStride();
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_boundIndexBuffer);
            }

            void GLCommandList::SetConstantBuffer(RHIShaderStage, uint32_t slot, IRHIBuffer* buffer)
            {
                if (!buffer)
                    return;
                auto* glBuf = static_cast<GLBuffer*>(buffer);
                glBindBufferBase(GL_UNIFORM_BUFFER, slot, glBuf->GetGLBuffer());
            }

            void GLCommandList::SetShaderResource(RHIShaderStage, uint32_t slot, IRHITexture* texture)
            {
                if (texture)
                {
                    auto* glTex = static_cast<GLTexture*>(texture);
                    glActiveTexture(GL_TEXTURE0 + slot);
                    glBindTexture(GL_TEXTURE_2D, glTex->GetGLTexture());
                }
                else
                {
                    glActiveTexture(GL_TEXTURE0 + slot);
                    glBindTexture(GL_TEXTURE_2D, 0);
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
            }

            void GLCommandList::DrawIndexed(uint32_t indexCount, uint32_t startIndex, int32_t baseVertex)
            {
                GLenum indexType = (m_indexStride == 4) ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
                size_t offset = startIndex * m_indexStride;
                glDrawElementsBaseVertex(m_currentTopology, indexCount, indexType, reinterpret_cast<void*>(offset),
                                         baseVertex);
            }

            void GLCommandList::DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex,
                                              uint32_t startInstance)
            {
                glDrawArraysInstancedBaseInstance(m_currentTopology, startVertex, vertexCount, instanceCount,
                                                  startInstance);
            }

            void GLCommandList::DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t startIndex,
                                                     int32_t baseVertex, uint32_t startInstance)
            {
                GLenum indexType = (m_indexStride == 4) ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
                size_t offset = startIndex * m_indexStride;
                glDrawElementsInstancedBaseVertexBaseInstance(m_currentTopology, indexCount, indexType,
                                                              reinterpret_cast<void*>(offset), instanceCount,
                                                              baseVertex, startInstance);
            }

            void GLCommandList::Dispatch(uint32_t x, uint32_t y, uint32_t z)
            {
                glDispatchCompute(x, y, z);
                glMemoryBarrier(GL_ALL_BARRIER_BITS);
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
                m_debugEnabled = desc.enableDebugLayer;

                // GLAD must be loaded before any GL calls
                // In a real implementation, context creation happens first
                if (!gladLoadGL())
                {
                    return false;
                }

                if (m_debugEnabled)
                {
                    glEnable(GL_DEBUG_OUTPUT);
                    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
                }

                QueryCapabilities();

                m_immediateCommandList = std::make_unique<GLCommandList>(true);

                return true;
            }

            void GLDevice::Shutdown()
            {
                m_immediateCommandList.reset();
            }

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

                GLfloat maxAniso;
                glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maxAniso);
                m_capabilities.maxAnisotropy = maxAniso;

                m_capabilities.tessellationSupport = true;
                m_capabilities.computeShaderSupport = true;
                m_capabilities.geometryShaderSupport = true;
            }

            std::unique_ptr<IRHISwapChain> GLDevice::CreateSwapChain(const RHISwapChainDesc& desc)
            {
                return std::make_unique<GLSwapChain>(desc);
            }

            IRHIBuffer* GLDevice::CreateBuffer(const RHIBufferDesc& desc)
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
                    usage = GL_DYNAMIC_DRAW;
                    flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
                    break;
                case RHIBufferAccess::Staging:
                    flags = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT;
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

                return new GLBuffer(desc, buffer);
            }

            IRHITexture* GLDevice::CreateTexture(const RHITextureDesc& desc)
            {
                GLuint texture;
                glCreateTextures(GL_TEXTURE_2D, 1, &texture);

                GLenum internalFormat = ConvertInternalFormat(desc.format);

                glTextureStorage2D(texture, desc.mipLevels, internalFormat, desc.width, desc.height);

                // Create framebuffer if render target
                GLuint fbo = 0;
                if (desc.usage & RHITextureUsage::RenderTarget)
                {
                    glCreateFramebuffers(1, &fbo);
                    glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0, texture, 0);
                }
                else if (desc.usage & RHITextureUsage::DepthStencil)
                {
                    glCreateFramebuffers(1, &fbo);
                    glNamedFramebufferTexture(fbo, GL_DEPTH_STENCIL_ATTACHMENT, texture, 0);
                }

                return new GLTexture(desc, texture, fbo);
            }

            IRHIShader* GLDevice::CreateShader(const RHIShaderDesc& desc)
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
                    glSpecializeShader(shader, desc.entryPoint.c_str(), 0, nullptr, nullptr);
                }
                else if (!desc.sourceCode.empty())
                {
                    // GLSL source compilation
                    const char* src = desc.sourceCode.c_str();
                    glShaderSource(shader, 1, &src, nullptr);
                    glCompileShader(shader);

                    GLint success;
                    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
                    if (!success)
                    {
                        char infoLog[1024];
                        glGetShaderInfoLog(shader, sizeof(infoLog), nullptr, infoLog);
                        std::cerr << "[OpenGL] Shader compilation failed: " << infoLog << std::endl;
                        glDeleteShader(shader);
                        return nullptr;
                    }
                }
                else
                {
                    glDeleteShader(shader);
                    return nullptr;
                }

                return new GLShader(desc, shader, desc.sourceCode);
            }

            IRHISampler* GLDevice::CreateSampler(const RHISamplerDesc& desc)
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

                if (desc.minFilter == RHIFilterMode::Anisotropic || desc.magFilter == RHIFilterMode::Anisotropic)
                {
                    glSamplerParameterf(sampler, GL_TEXTURE_MAX_ANISOTROPY, static_cast<float>(desc.maxAnisotropy));
                }

                glSamplerParameterfv(sampler, GL_TEXTURE_BORDER_COLOR, desc.borderColor);

                return new GLSampler(desc, sampler);
            }

            IRHIPipelineState* GLDevice::CreatePipelineState(const RHIPipelineStateDesc& desc, IRHIShader* vertexShader,
                                                             IRHIShader* pixelShader)
            {
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
                    std::cerr << "[OpenGL] Program link failed: " << infoLog << std::endl;
                    glDeleteProgram(program);
                    return nullptr;
                }

                // Create VAO from input layout
                GLuint vao;
                glCreateVertexArrays(1, &vao);

                uint32_t totalStride = 0;
                for (const auto& elem : desc.inputLayout.elements)
                {
                    uint32_t elemSize = 0;
                    switch (elem.format)
                    {
                    case RHIVertexFormat::Float1:
                        elemSize = 4;
                        break;
                    case RHIVertexFormat::Float2:
                        elemSize = 8;
                        break;
                    case RHIVertexFormat::Float3:
                        elemSize = 12;
                        break;
                    case RHIVertexFormat::Float4:
                        elemSize = 16;
                        break;
                    default:
                        elemSize = 4;
                        break;
                    }
                    totalStride = std::max(totalStride, elem.byteOffset + elemSize);
                }

                for (size_t i = 0; i < desc.inputLayout.elements.size(); ++i)
                {
                    const auto& elem = desc.inputLayout.elements[i];
                    GLuint index = static_cast<GLuint>(i);

                    glEnableVertexArrayAttrib(vao, index);
                    glVertexArrayAttribBinding(vao, index, elem.inputSlot);

                    GLint numComponents = 3;
                    GLenum type = GL_FLOAT;

                    switch (elem.format)
                    {
                    case RHIVertexFormat::Float1:
                        numComponents = 1;
                        type = GL_FLOAT;
                        break;
                    case RHIVertexFormat::Float2:
                        numComponents = 2;
                        type = GL_FLOAT;
                        break;
                    case RHIVertexFormat::Float3:
                        numComponents = 3;
                        type = GL_FLOAT;
                        break;
                    case RHIVertexFormat::Float4:
                        numComponents = 4;
                        type = GL_FLOAT;
                        break;
                    case RHIVertexFormat::Int1:
                        numComponents = 1;
                        type = GL_INT;
                        break;
                    case RHIVertexFormat::Int2:
                        numComponents = 2;
                        type = GL_INT;
                        break;
                    case RHIVertexFormat::Int4:
                        numComponents = 4;
                        type = GL_INT;
                        break;
                    case RHIVertexFormat::UInt1:
                        numComponents = 1;
                        type = GL_UNSIGNED_INT;
                        break;
                    case RHIVertexFormat::UNorm8x4:
                        numComponents = 4;
                        type = GL_UNSIGNED_BYTE;
                        break;
                    default:
                        break;
                    }

                    glVertexArrayAttribFormat(vao, index, numComponents, type, GL_FALSE, elem.byteOffset);
                }

                return new GLPipelineState(desc, program, vao);
            }

            void GLDevice::DestroyBuffer(IRHIBuffer* buffer)
            {
                delete buffer;
            }
            void GLDevice::DestroyTexture(IRHITexture* texture)
            {
                delete texture;
            }
            void GLDevice::DestroyShader(IRHIShader* shader)
            {
                delete shader;
            }
            void GLDevice::DestroySampler(IRHISampler* sampler)
            {
                delete sampler;
            }
            void GLDevice::DestroyPipelineState(IRHIPipelineState* state)
            {
                delete state;
            }

            void* GLDevice::MapBuffer(IRHIBuffer* buffer)
            {
                auto* glBuf = static_cast<GLBuffer*>(buffer);
                return glMapNamedBuffer(glBuf->GetGLBuffer(), GL_WRITE_ONLY);
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

            void GLDevice::UpdateTexture(IRHITexture* texture, const void* data, uint32_t mipLevel, uint32_t)
            {
                auto* glTex = static_cast<GLTexture*>(texture);
                GLenum format = ConvertFormat(glTex->GetFormat());
                GLenum type = ConvertFormatType(glTex->GetFormat());

                uint32_t w = std::max(1u, glTex->GetWidth() >> mipLevel);
                uint32_t h = std::max(1u, glTex->GetHeight() >> mipLevel);

                glTextureSubImage2D(glTex->GetGLTexture(), mipLevel, 0, 0, w, h, format, type, data);
            }

            IRHICommandList* GLDevice::GetImmediateCommandList()
            {
                return m_immediateCommandList.get();
            }

            IRHICommandList* GLDevice::CreateDeferredCommandList()
            {
                return new GLCommandList(false);
            }

            void GLDevice::ExecuteCommandList(IRHICommandList*) {}
            void GLDevice::DestroyCommandList(IRHICommandList* commandList)
            {
                delete commandList;
            }

            void GLDevice::BeginFrame()
            {
                ResetStatistics();
            }
            void GLDevice::EndFrame() {}
            void GLDevice::WaitForIdle()
            {
                glFinish();
            }

            void GLDevice::ResetStatistics()
            {
                m_statistics = {};
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
                    return GL_RED;
                case PixelFormat::R8G8_UNORM:
                    return GL_RG;
                case PixelFormat::R8G8B8A8_UNORM:
                    return GL_RGBA;
                case PixelFormat::R8G8B8A8_UNORM_SRGB:
                    return GL_RGBA;
                case PixelFormat::R16_FLOAT:
                    return GL_RED;
                case PixelFormat::R16G16_FLOAT:
                    return GL_RG;
                case PixelFormat::R16G16B16A16_FLOAT:
                    return GL_RGBA;
                case PixelFormat::R32_FLOAT:
                    return GL_RED;
                case PixelFormat::R32G32_FLOAT:
                    return GL_RG;
                case PixelFormat::R32G32B32_FLOAT:
                    return GL_RGB;
                case PixelFormat::R32G32B32A32_FLOAT:
                    return GL_RGBA;
                case PixelFormat::D16_UNORM:
                    return GL_DEPTH_COMPONENT;
                case PixelFormat::D24_UNORM_S8_UINT:
                    return GL_DEPTH_STENCIL;
                case PixelFormat::D32_FLOAT:
                    return GL_DEPTH_COMPONENT;
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
                case PixelFormat::R8G8_UNORM:
                    return GL_RG8;
                case PixelFormat::R8G8B8A8_UNORM:
                    return GL_RGBA8;
                case PixelFormat::R8G8B8A8_UNORM_SRGB:
                    return GL_SRGB8_ALPHA8;
                case PixelFormat::R10G10B10A2_UNORM:
                    return GL_RGB10_A2;
                case PixelFormat::R11G11B10_FLOAT:
                    return GL_R11F_G11F_B10F;
                case PixelFormat::R16_FLOAT:
                    return GL_R16F;
                case PixelFormat::R16G16_FLOAT:
                    return GL_RG16F;
                case PixelFormat::R16G16B16A16_FLOAT:
                    return GL_RGBA16F;
                case PixelFormat::R32_FLOAT:
                    return GL_R32F;
                case PixelFormat::R32G32_FLOAT:
                    return GL_RG32F;
                case PixelFormat::R32G32B32_FLOAT:
                    return GL_RGB32F;
                case PixelFormat::R32G32B32A32_FLOAT:
                    return GL_RGBA32F;
                case PixelFormat::D16_UNORM:
                    return GL_DEPTH_COMPONENT16;
                case PixelFormat::D24_UNORM_S8_UINT:
                    return GL_DEPTH24_STENCIL8;
                case PixelFormat::D32_FLOAT:
                    return GL_DEPTH_COMPONENT32F;
                default:
                    return GL_RGBA8;
                }
            }

            GLenum GLDevice::ConvertFormatType(PixelFormat format) const
            {
                switch (format)
                {
                case PixelFormat::R8_UNORM:
                case PixelFormat::R8G8_UNORM:
                case PixelFormat::R8G8B8A8_UNORM:
                case PixelFormat::R8G8B8A8_UNORM_SRGB:
                    return GL_UNSIGNED_BYTE;
                case PixelFormat::R16_FLOAT:
                case PixelFormat::R16G16_FLOAT:
                case PixelFormat::R16G16B16A16_FLOAT:
                    return GL_HALF_FLOAT;
                case PixelFormat::R32_FLOAT:
                case PixelFormat::R32G32_FLOAT:
                case PixelFormat::R32G32B32_FLOAT:
                case PixelFormat::R32G32B32A32_FLOAT:
                case PixelFormat::D32_FLOAT:
                    return GL_FLOAT;
                case PixelFormat::D24_UNORM_S8_UINT:
                    return GL_UNSIGNED_INT_24_8;
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

        } // namespace OpenGL
    } // namespace RHI
} // namespace Spark

#endif // SPARK_OPENGL_SUPPORT
