/**
 * @file VulkanFormatHelpers.cpp
 * @brief Vulkan format conversion utilities
 *
 * Split from VulkanDevice.cpp for maintainability.
 */

#ifdef SPARK_VULKAN_SUPPORT

#include "VulkanDevice.h"

namespace Spark
{
    namespace RHI
    {
        namespace Vulkan
        {

            // ============================================================================
            // FORMAT CONVERSION HELPERS
            // ============================================================================

            uint32_t VulkanDevice::GetVertexFormatSize(RHIVertexFormat format) const
            {
                switch (format)
                {
                case RHIVertexFormat::Float1:
                case RHIVertexFormat::Int1:
                case RHIVertexFormat::UInt1:
                    return 4;
                case RHIVertexFormat::Float2:
                case RHIVertexFormat::Int2:
                case RHIVertexFormat::UInt2:
                    return 8;
                case RHIVertexFormat::Float3:
                case RHIVertexFormat::Int3:
                case RHIVertexFormat::UInt3:
                    return 12;
                case RHIVertexFormat::Float4:
                case RHIVertexFormat::Int4:
                case RHIVertexFormat::UInt4:
                    return 16;
                case RHIVertexFormat::UNorm8x4:
                case RHIVertexFormat::SNorm8x4:
                    return 4;
                default:
                    return 4;
                }
            }

            VkBorderColor VulkanDevice::ConvertBorderColor(const float borderColor[4]) const
            {
                // Vulkan only supports specific border color enum values
                if (borderColor[3] == 0.0f)
                {
                    if (borderColor[0] == 0.0f && borderColor[1] == 0.0f && borderColor[2] == 0.0f)
                        return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
                }
                if (borderColor[0] == 0.0f && borderColor[1] == 0.0f && borderColor[2] == 0.0f &&
                    borderColor[3] == 1.0f)
                    return VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
                if (borderColor[0] == 1.0f && borderColor[1] == 1.0f && borderColor[2] == 1.0f &&
                    borderColor[3] == 1.0f)
                    return VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
                // Default to transparent black for unsupported custom border colors
                return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
            }

            VkFormat VulkanDevice::ConvertFormat(PixelFormat format) const
            {
                switch (format)
                {
                case PixelFormat::R8_UNORM:
                    return VK_FORMAT_R8_UNORM;
                case PixelFormat::R8_SNORM:
                    return VK_FORMAT_R8_SNORM;
                case PixelFormat::R8_UINT:
                    return VK_FORMAT_R8_UINT;
                case PixelFormat::R8G8_UNORM:
                    return VK_FORMAT_R8G8_UNORM;
                case PixelFormat::R8G8B8A8_UNORM:
                    return VK_FORMAT_R8G8B8A8_UNORM;
                case PixelFormat::R8G8B8A8_UNORM_SRGB:
                    return VK_FORMAT_R8G8B8A8_SRGB;
                case PixelFormat::R8G8B8A8_SNORM:
                    return VK_FORMAT_R8G8B8A8_SNORM;
                case PixelFormat::B8G8R8A8_UNORM:
                    return VK_FORMAT_B8G8R8A8_UNORM;
                case PixelFormat::B8G8R8A8_UNORM_SRGB:
                    return VK_FORMAT_B8G8R8A8_SRGB;
                case PixelFormat::R10G10B10A2_UNORM:
                    return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
                case PixelFormat::R11G11B10_FLOAT:
                    return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
                case PixelFormat::R16_FLOAT:
                    return VK_FORMAT_R16_SFLOAT;
                case PixelFormat::R16_UINT:
                    return VK_FORMAT_R16_UINT;
                case PixelFormat::R16G16_FLOAT:
                    return VK_FORMAT_R16G16_SFLOAT;
                case PixelFormat::R16G16B16A16_FLOAT:
                    return VK_FORMAT_R16G16B16A16_SFLOAT;
                case PixelFormat::R16G16B16A16_UNORM:
                    return VK_FORMAT_R16G16B16A16_UNORM;
                case PixelFormat::R32_FLOAT:
                    return VK_FORMAT_R32_SFLOAT;
                case PixelFormat::R32_UINT:
                    return VK_FORMAT_R32_UINT;
                case PixelFormat::R32G32_FLOAT:
                    return VK_FORMAT_R32G32_SFLOAT;
                case PixelFormat::R32G32B32_FLOAT:
                    return VK_FORMAT_R32G32B32_SFLOAT;
                case PixelFormat::R32G32B32A32_FLOAT:
                    return VK_FORMAT_R32G32B32A32_SFLOAT;
                case PixelFormat::BC1_UNORM:
                    return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
                case PixelFormat::BC1_UNORM_SRGB:
                    return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
                case PixelFormat::BC2_UNORM:
                    return VK_FORMAT_BC2_UNORM_BLOCK;
                case PixelFormat::BC3_UNORM:
                    return VK_FORMAT_BC3_UNORM_BLOCK;
                case PixelFormat::BC3_UNORM_SRGB:
                    return VK_FORMAT_BC3_SRGB_BLOCK;
                case PixelFormat::BC4_UNORM:
                    return VK_FORMAT_BC4_UNORM_BLOCK;
                case PixelFormat::BC5_UNORM:
                    return VK_FORMAT_BC5_UNORM_BLOCK;
                case PixelFormat::BC6H_UF16:
                    return VK_FORMAT_BC6H_UFLOAT_BLOCK;
                case PixelFormat::BC7_UNORM:
                    return VK_FORMAT_BC7_UNORM_BLOCK;
                case PixelFormat::BC7_UNORM_SRGB:
                    return VK_FORMAT_BC7_SRGB_BLOCK;
                case PixelFormat::D16_UNORM:
                    return VK_FORMAT_D16_UNORM;
                case PixelFormat::D24_UNORM_S8_UINT:
                    return VK_FORMAT_D24_UNORM_S8_UINT;
                case PixelFormat::D32_FLOAT:
                    return VK_FORMAT_D32_SFLOAT;
                case PixelFormat::D32_FLOAT_S8_UINT:
                    return VK_FORMAT_D32_SFLOAT_S8_UINT;
                default:
                    return VK_FORMAT_UNDEFINED;
                }
            }

            VkFilter VulkanDevice::ConvertFilter(RHIFilterMode mode) const
            {
                return (mode == RHIFilterMode::Nearest) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
            }

            VkSamplerAddressMode VulkanDevice::ConvertAddressMode(RHIAddressMode mode) const
            {
                switch (mode)
                {
                case RHIAddressMode::Wrap:
                    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
                case RHIAddressMode::Clamp:
                    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                case RHIAddressMode::Mirror:
                    return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
                case RHIAddressMode::Border:
                    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
                case RHIAddressMode::MirrorOnce:
                    return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
                default:
                    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
                }
            }

            VkCompareOp VulkanDevice::ConvertCompareOp(RHICompareOp op) const
            {
                switch (op)
                {
                case RHICompareOp::Never:
                    return VK_COMPARE_OP_NEVER;
                case RHICompareOp::Less:
                    return VK_COMPARE_OP_LESS;
                case RHICompareOp::Equal:
                    return VK_COMPARE_OP_EQUAL;
                case RHICompareOp::LessEqual:
                    return VK_COMPARE_OP_LESS_OR_EQUAL;
                case RHICompareOp::Greater:
                    return VK_COMPARE_OP_GREATER;
                case RHICompareOp::NotEqual:
                    return VK_COMPARE_OP_NOT_EQUAL;
                case RHICompareOp::GreaterEqual:
                    return VK_COMPARE_OP_GREATER_OR_EQUAL;
                case RHICompareOp::Always:
                    return VK_COMPARE_OP_ALWAYS;
                default:
                    return VK_COMPARE_OP_LESS;
                }
            }

            VkStencilOp VulkanDevice::ConvertStencilOp(RHIStencilOp op) const
            {
                switch (op)
                {
                case RHIStencilOp::Keep:
                    return VK_STENCIL_OP_KEEP;
                case RHIStencilOp::Zero:
                    return VK_STENCIL_OP_ZERO;
                case RHIStencilOp::Replace:
                    return VK_STENCIL_OP_REPLACE;
                case RHIStencilOp::IncrSat:
                    return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
                case RHIStencilOp::DecrSat:
                    return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
                case RHIStencilOp::Invert:
                    return VK_STENCIL_OP_INVERT;
                case RHIStencilOp::IncrWrap:
                    return VK_STENCIL_OP_INCREMENT_AND_WRAP;
                case RHIStencilOp::DecrWrap:
                    return VK_STENCIL_OP_DECREMENT_AND_WRAP;
                default:
                    return VK_STENCIL_OP_KEEP;
                }
            }

            VkBlendFactor VulkanDevice::ConvertBlendFactor(RHIBlendFactor factor) const
            {
                switch (factor)
                {
                case RHIBlendFactor::Zero:
                    return VK_BLEND_FACTOR_ZERO;
                case RHIBlendFactor::One:
                    return VK_BLEND_FACTOR_ONE;
                case RHIBlendFactor::SrcColor:
                    return VK_BLEND_FACTOR_SRC_COLOR;
                case RHIBlendFactor::InvSrcColor:
                    return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
                case RHIBlendFactor::SrcAlpha:
                    return VK_BLEND_FACTOR_SRC_ALPHA;
                case RHIBlendFactor::InvSrcAlpha:
                    return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                case RHIBlendFactor::DstAlpha:
                    return VK_BLEND_FACTOR_DST_ALPHA;
                case RHIBlendFactor::InvDstAlpha:
                    return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
                case RHIBlendFactor::DstColor:
                    return VK_BLEND_FACTOR_DST_COLOR;
                case RHIBlendFactor::InvDstColor:
                    return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
                default:
                    return VK_BLEND_FACTOR_ZERO;
                }
            }

            VkBlendOp VulkanDevice::ConvertBlendOp(RHIBlendOp op) const
            {
                switch (op)
                {
                case RHIBlendOp::Add:
                    return VK_BLEND_OP_ADD;
                case RHIBlendOp::Subtract:
                    return VK_BLEND_OP_SUBTRACT;
                case RHIBlendOp::RevSubtract:
                    return VK_BLEND_OP_REVERSE_SUBTRACT;
                case RHIBlendOp::Min:
                    return VK_BLEND_OP_MIN;
                case RHIBlendOp::Max:
                    return VK_BLEND_OP_MAX;
                default:
                    return VK_BLEND_OP_ADD;
                }
            }

            VkPrimitiveTopology VulkanDevice::ConvertTopology(RHIPrimitiveTopology topology) const
            {
                switch (topology)
                {
                case RHIPrimitiveTopology::PointList:
                    return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
                case RHIPrimitiveTopology::LineList:
                    return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
                case RHIPrimitiveTopology::LineStrip:
                    return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
                case RHIPrimitiveTopology::TriangleList:
                    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
                case RHIPrimitiveTopology::TriangleStrip:
                    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
                case RHIPrimitiveTopology::PatchList:
                    return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
                default:
                    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
                }
            }

            VkFormat VulkanDevice::ConvertVertexFormat(RHIVertexFormat format) const
            {
                switch (format)
                {
                case RHIVertexFormat::Float1:
                    return VK_FORMAT_R32_SFLOAT;
                case RHIVertexFormat::Float2:
                    return VK_FORMAT_R32G32_SFLOAT;
                case RHIVertexFormat::Float3:
                    return VK_FORMAT_R32G32B32_SFLOAT;
                case RHIVertexFormat::Float4:
                    return VK_FORMAT_R32G32B32A32_SFLOAT;
                case RHIVertexFormat::Int1:
                    return VK_FORMAT_R32_SINT;
                case RHIVertexFormat::Int2:
                    return VK_FORMAT_R32G32_SINT;
                case RHIVertexFormat::Int3:
                    return VK_FORMAT_R32G32B32_SINT;
                case RHIVertexFormat::Int4:
                    return VK_FORMAT_R32G32B32A32_SINT;
                case RHIVertexFormat::UInt1:
                    return VK_FORMAT_R32_UINT;
                case RHIVertexFormat::UInt2:
                    return VK_FORMAT_R32G32_UINT;
                case RHIVertexFormat::UInt3:
                    return VK_FORMAT_R32G32B32_UINT;
                case RHIVertexFormat::UInt4:
                    return VK_FORMAT_R32G32B32A32_UINT;
                case RHIVertexFormat::UNorm8x4:
                    return VK_FORMAT_R8G8B8A8_UNORM;
                case RHIVertexFormat::SNorm8x4:
                    return VK_FORMAT_R8G8B8A8_SNORM;
                default:
                    return VK_FORMAT_R32G32B32_SFLOAT;
                }
            }

        } // namespace Vulkan
    } // namespace RHI

} // namespace Vulkan
} // namespace RHI
} // namespace Spark

#endif // SPARK_VULKAN_SUPPORT
