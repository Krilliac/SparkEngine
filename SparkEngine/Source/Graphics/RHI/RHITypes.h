/**
 * @file RHITypes.h
 * @brief Core types and enumerations for the Rendering Hardware Interface
 * @author Spark Engine Team
 * @date 2025
 *
 * Platform-agnostic type definitions used across all graphics API backends.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <memory>
#include <functional>

namespace Spark
{
    namespace RHI
    {

        // ============================================================================
        // GRAPHICS API ENUM
        // ============================================================================

        enum class GraphicsBackend
        {
            D3D11,
            D3D12,
            Vulkan,
            OpenGL,
            Metal,
            Auto, // Auto-detect best available
            None  // No backend available
        };

        // ============================================================================
        // FORMAT ENUMS
        // ============================================================================

        enum class PixelFormat
        {
            Unknown = 0,

            // 8-bit per channel
            R8_UNORM,
            R8_SNORM,
            R8_UINT,
            R8G8_UNORM,
            R8G8B8A8_UNORM,
            R8G8B8A8_UNORM_SRGB,
            R8G8B8A8_SNORM,
            B8G8R8A8_UNORM,
            B8G8R8A8_UNORM_SRGB,

            // 10/11-bit packed
            R10G10B10A2_UNORM,
            R11G11B10_FLOAT,

            // 16-bit per channel
            R16_FLOAT,
            R16_UINT,
            R16G16_FLOAT,
            R16G16B16A16_FLOAT,
            R16G16B16A16_UNORM,

            // 32-bit per channel
            R32_FLOAT,
            R32_UINT,
            R32G32_FLOAT,
            R32G32B32_FLOAT,
            R32G32B32A32_FLOAT,

            // Compressed formats
            BC1_UNORM,
            BC1_UNORM_SRGB,
            BC2_UNORM,
            BC3_UNORM,
            BC3_UNORM_SRGB,
            BC4_UNORM,
            BC5_UNORM,
            BC6H_UF16,
            BC7_UNORM,
            BC7_UNORM_SRGB,

            // Depth/Stencil
            D16_UNORM,
            D24_UNORM_S8_UINT,
            D32_FLOAT,
            D32_FLOAT_S8_UINT,

            Count
        };

        // ============================================================================
        // SHADER TYPES
        // ============================================================================

        enum class RHIShaderStage
        {
            Vertex,
            Pixel,
            Geometry,
            Hull,
            Domain,
            Compute
        };

        enum class ShaderLanguage
        {
            HLSL,
            GLSL,
            SPIRV,
            Auto
        };

        // ============================================================================
        // BUFFER TYPES
        // ============================================================================

        enum class RHIBufferUsage : uint32_t
        {
            Vertex = 1 << 0,
            Index = 1 << 1,
            Constant = 1 << 2,
            Structured = 1 << 3,
            IndirectArgs = 1 << 4,
            Storage = 1 << 5,
            CopySrc = 1 << 6,
            CopyDst = 1 << 7
        };

        inline RHIBufferUsage operator|(RHIBufferUsage a, RHIBufferUsage b)
        {
            return static_cast<RHIBufferUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
        }
        inline bool operator&(RHIBufferUsage a, RHIBufferUsage b)
        {
            return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0;
        }

        enum class RHIBufferAccess
        {
            Static,  // GPU only, immutable after creation
            Dynamic, // CPU write, GPU read (every frame)
            Staging, // CPU read/write
            ReadBack // GPU write, CPU read
        };

        // ============================================================================
        // TEXTURE TYPES
        // ============================================================================

        enum class RHITextureType
        {
            Texture1D,
            Texture2D,
            Texture3D,
            TextureCube,
            Texture2DArray,
            TextureCubeArray
        };

        enum class RHITextureUsage : uint32_t
        {
            ShaderResource = 1 << 0,
            RenderTarget = 1 << 1,
            DepthStencil = 1 << 2,
            UnorderedAccess = 1 << 3,
            TransferSrc = 1 << 4,
            TransferDst = 1 << 5
        };

        inline RHITextureUsage operator|(RHITextureUsage a, RHITextureUsage b)
        {
            return static_cast<RHITextureUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
        }
        inline bool operator&(RHITextureUsage a, RHITextureUsage b)
        {
            return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0;
        }

        // ============================================================================
        // SAMPLER TYPES
        // ============================================================================

        enum class RHIFilterMode
        {
            Nearest,
            Linear,
            Anisotropic
        };

        enum class RHIAddressMode
        {
            Wrap,
            Clamp,
            Mirror,
            Border,
            MirrorOnce
        };

        enum class RHICompareOp
        {
            Never,
            Less,
            Equal,
            LessEqual,
            Greater,
            NotEqual,
            GreaterEqual,
            Always
        };

        // ============================================================================
        // PIPELINE STATE TYPES
        // ============================================================================

        enum class RHIPrimitiveTopology
        {
            PointList,
            LineList,
            LineStrip,
            TriangleList,
            TriangleStrip,
            PatchList
        };

        enum class RHIFillMode
        {
            Solid,
            Wireframe
        };

        enum class RHICullMode
        {
            None,
            Front,
            Back
        };

        enum class RHIBlendFactor
        {
            Zero,
            One,
            SrcColor,
            InvSrcColor,
            SrcAlpha,
            InvSrcAlpha,
            DstAlpha,
            InvDstAlpha,
            DstColor,
            InvDstColor
        };

        enum class RHIBlendOp
        {
            Add,
            Subtract,
            RevSubtract,
            Min,
            Max
        };

        enum class RHIStencilOp
        {
            Keep,
            Zero,
            Replace,
            IncrSat,
            DecrSat,
            Invert,
            IncrWrap,
            DecrWrap
        };

        // ============================================================================
        // DESCRIPTOR STRUCTURES
        // ============================================================================

        struct RHIBufferDesc
        {
            uint64_t size = 0;
            uint32_t stride = 0;
            RHIBufferUsage usage = RHIBufferUsage::Vertex;
            RHIBufferAccess access = RHIBufferAccess::Static;
            const void* initialData = nullptr;
            std::string debugName;
        };

        struct RHITextureDesc
        {
            uint32_t width = 1;
            uint32_t height = 1;
            uint32_t depth = 1;
            uint32_t mipLevels = 1;
            uint32_t arraySize = 1;
            uint32_t sampleCount = 1;
            PixelFormat format = PixelFormat::R8G8B8A8_UNORM;
            RHITextureType type = RHITextureType::Texture2D;
            RHITextureUsage usage = RHITextureUsage::ShaderResource;
            float clearColor[4] = {0, 0, 0, 1};
            float clearDepth = 1.0f;
            uint8_t clearStencil = 0;
            std::string debugName;
        };

        struct RHISamplerDesc
        {
            RHIFilterMode minFilter = RHIFilterMode::Linear;
            RHIFilterMode magFilter = RHIFilterMode::Linear;
            RHIFilterMode mipFilter = RHIFilterMode::Linear;
            RHIAddressMode addressU = RHIAddressMode::Wrap;
            RHIAddressMode addressV = RHIAddressMode::Wrap;
            RHIAddressMode addressW = RHIAddressMode::Wrap;
            float mipLodBias = 0.0f;
            uint32_t maxAnisotropy = 16;
            RHICompareOp compareOp = RHICompareOp::Never;
            float borderColor[4] = {0, 0, 0, 0};
            float minLod = 0.0f;
            float maxLod = 1000.0f;
        };

        struct RHIShaderDesc
        {
            RHIShaderStage stage = RHIShaderStage::Vertex;
            const void* bytecode = nullptr;
            size_t bytecodeSize = 0;
            std::string sourceCode;
            std::string entryPoint = "main";
            std::string filePath;
            ShaderLanguage language = ShaderLanguage::Auto;
            std::vector<std::string> defines;
            std::string debugName;
        };

        // ============================================================================
        // INPUT LAYOUT
        // ============================================================================

        enum class RHIVertexFormat
        {
            Float1,
            Float2,
            Float3,
            Float4,
            Int1,
            Int2,
            Int3,
            Int4,
            UInt1,
            UInt2,
            UInt3,
            UInt4,
            UNorm8x4,
            SNorm8x4
        };

        struct RHIInputElement
        {
            std::string semanticName;
            uint32_t semanticIndex = 0;
            RHIVertexFormat format = RHIVertexFormat::Float3;
            uint32_t inputSlot = 0;
            uint32_t byteOffset = 0;
            bool perInstance = false;
            uint32_t instanceStepRate = 0;
        };

        struct RHIInputLayoutDesc
        {
            std::vector<RHIInputElement> elements;
        };

        // ============================================================================
        // PIPELINE STATE
        // ============================================================================

        struct RHIRasterizerDesc
        {
            RHIFillMode fillMode = RHIFillMode::Solid;
            RHICullMode cullMode = RHICullMode::Back;
            bool frontCounterClockwise = false;
            int32_t depthBias = 0;
            float depthBiasClamp = 0.0f;
            float slopeScaledDepthBias = 0.0f;
            bool depthClipEnable = true;
            bool scissorEnable = false;
            bool multisampleEnable = false;
            bool antialiasedLineEnable = false;
        };

        struct RHIBlendTargetDesc
        {
            bool blendEnable = false;
            RHIBlendFactor srcBlend = RHIBlendFactor::One;
            RHIBlendFactor dstBlend = RHIBlendFactor::Zero;
            RHIBlendOp blendOp = RHIBlendOp::Add;
            RHIBlendFactor srcBlendAlpha = RHIBlendFactor::One;
            RHIBlendFactor dstBlendAlpha = RHIBlendFactor::Zero;
            RHIBlendOp blendOpAlpha = RHIBlendOp::Add;
            uint8_t writeMask = 0x0F;
        };

        struct RHIBlendDesc
        {
            bool alphaToCoverageEnable = false;
            bool independentBlendEnable = false;
            RHIBlendTargetDesc renderTargets[8];
        };

        struct RHIDepthStencilOpDesc
        {
            RHIStencilOp stencilFail = RHIStencilOp::Keep;
            RHIStencilOp stencilDepthFail = RHIStencilOp::Keep;
            RHIStencilOp stencilPass = RHIStencilOp::Keep;
            RHICompareOp stencilFunc = RHICompareOp::Always;
        };

        struct RHIDepthStencilDesc
        {
            bool depthEnable = true;
            bool depthWrite = true;
            RHICompareOp depthFunc = RHICompareOp::Less;
            bool stencilEnable = false;
            uint8_t stencilReadMask = 0xFF;
            uint8_t stencilWriteMask = 0xFF;
            RHIDepthStencilOpDesc frontFace;
            RHIDepthStencilOpDesc backFace;
        };

        struct RHIPipelineStateDesc
        {
            RHIInputLayoutDesc inputLayout;
            RHIRasterizerDesc rasterizer;
            RHIBlendDesc blend;
            RHIDepthStencilDesc depthStencil;
            RHIPrimitiveTopology topology = RHIPrimitiveTopology::TriangleList;
            uint32_t sampleCount = 1;
            PixelFormat renderTargetFormats[8] = {};
            uint32_t numRenderTargets = 1;
            PixelFormat depthStencilFormat = PixelFormat::D24_UNORM_S8_UINT;
            std::string debugName;
        };

        // ============================================================================
        // VIEWPORT AND SCISSOR
        // ============================================================================

        struct RHIViewport
        {
            float x = 0.0f;
            float y = 0.0f;
            float width = 0.0f;
            float height = 0.0f;
            float minDepth = 0.0f;
            float maxDepth = 1.0f;
        };

        struct RHIScissorRect
        {
            int32_t left = 0;
            int32_t top = 0;
            int32_t right = 0;
            int32_t bottom = 0;
        };

        // ============================================================================
        // DEVICE CAPABILITIES
        // ============================================================================

        struct RHIDeviceCapabilities
        {
            std::string deviceName;
            std::string vendorName;
            std::string apiVersion;
            GraphicsBackend backend = GraphicsBackend::Auto;

            uint64_t dedicatedVideoMemory = 0;
            uint64_t sharedSystemMemory = 0;

            uint32_t maxTextureSize = 16384;
            uint32_t maxRenderTargets = 8;
            uint32_t maxSamplers = 16;
            uint32_t maxConstantBuffers = 14;
            uint32_t maxVertexAttributes = 32;

            bool tessellationSupport = false;
            bool computeShaderSupport = false;
            bool geometryShaderSupport = false;
            bool multiDrawIndirectSupport = false;
            bool conservativeRasterSupport = false;
            bool meshShaderSupport = false;
            bool rayTracingSupport = false;
            bool variableRateShadingSupport = false;
            bool bindlessResourceSupport = false;

            uint32_t maxMSAASamples = 8;
            float maxAnisotropy = 16.0f;
        };

        // ============================================================================
        // STATISTICS
        // ============================================================================

        struct RHIStatistics
        {
            uint32_t drawCalls = 0;
            uint32_t dispatchCalls = 0;
            uint32_t trianglesRendered = 0;
            uint32_t verticesProcessed = 0;
            uint32_t textureBinds = 0;
            uint32_t bufferBinds = 0;
            uint32_t pipelineChanges = 0;
            uint32_t renderTargetChanges = 0;
            size_t gpuMemoryUsed = 0;
            float gpuFrameTime = 0.0f;
        };

        // ============================================================================
        // HANDLE TYPES
        // ============================================================================

        using RHIBufferHandle = uint64_t;
        using RHITextureHandle = uint64_t;
        using RHIShaderHandle = uint64_t;
        using RHISamplerHandle = uint64_t;
        using RHIPipelineHandle = uint64_t;

        constexpr uint64_t RHI_INVALID_HANDLE = 0;

    } // namespace RHI
} // namespace Spark
