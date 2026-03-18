/**
 * @file RHIPipelineTypes.h
 * @brief Pipeline state types and advanced descriptors for the Rendering Hardware Interface
 * @author Spark Engine Team
 * @date 2025
 *
 * Pipeline-specific type definitions: rasterizer, blend, depth-stencil state,
 * input layout, viewport/scissor, and the top-level pipeline state descriptor.
 * Split from RHITypes.h — include that header for core format/resource types.
 */

#pragma once

#include "RHITypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Spark
{
    namespace RHI
    {

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

    } // namespace RHI
} // namespace Spark
