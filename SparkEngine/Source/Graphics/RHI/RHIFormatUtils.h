/**
 * @file RHIFormatUtils.h
 * @brief Shared format utility functions for all RHI backends
 * @author Spark Engine Team
 * @date 2026
 *
 * Consolidates API-agnostic format queries (byte sizes, format classification)
 * that were previously duplicated across all five RHI backends. Backend-specific
 * format conversions (PixelFormat → DXGI_FORMAT/VkFormat/GLenum/MTLPixelFormat)
 * remain in each backend since they return different types.
 */

#pragma once

#include "RHITypes.h"
#include <cstdint>

namespace Spark
{
    namespace RHI
    {

        /// @brief Returns the byte size per pixel for an uncompressed format.
        /// Compressed formats (BC1-BC7) return the block size (8 or 16 bytes per 4x4 block).
        [[nodiscard]] inline constexpr uint32_t GetFormatSize(PixelFormat format)
        {
            switch (format)
            {
            case PixelFormat::R8_UNORM:
            case PixelFormat::R8_SNORM:
            case PixelFormat::R8_UINT:
                return 1;

            case PixelFormat::R8G8_UNORM:
            case PixelFormat::R16_FLOAT:
            case PixelFormat::R16_UINT:
            case PixelFormat::D16_UNORM:
                return 2;

            case PixelFormat::R8G8B8A8_UNORM:
            case PixelFormat::R8G8B8A8_UNORM_SRGB:
            case PixelFormat::R8G8B8A8_SNORM:
            case PixelFormat::B8G8R8A8_UNORM:
            case PixelFormat::B8G8R8A8_UNORM_SRGB:
            case PixelFormat::R10G10B10A2_UNORM:
            case PixelFormat::R11G11B10_FLOAT:
            case PixelFormat::R16G16_FLOAT:
            case PixelFormat::R32_FLOAT:
            case PixelFormat::R32_UINT:
            case PixelFormat::D24_UNORM_S8_UINT:
            case PixelFormat::D32_FLOAT:
                return 4;

            case PixelFormat::R16G16B16A16_FLOAT:
            case PixelFormat::R16G16B16A16_UNORM:
            case PixelFormat::R32G32_FLOAT:
            case PixelFormat::D32_FLOAT_S8_UINT:
                return 8;

            // BC1/BC4: 8 bytes per 4x4 block
            case PixelFormat::BC1_UNORM:
            case PixelFormat::BC1_UNORM_SRGB:
            case PixelFormat::BC4_UNORM:
                return 8;

            case PixelFormat::R32G32B32_FLOAT:
                return 12;

            case PixelFormat::R32G32B32A32_FLOAT:
                return 16;

            // BC2/BC3/BC5/BC6H/BC7: 16 bytes per 4x4 block
            case PixelFormat::BC2_UNORM:
            case PixelFormat::BC3_UNORM:
            case PixelFormat::BC3_UNORM_SRGB:
            case PixelFormat::BC5_UNORM:
            case PixelFormat::BC6H_UF16:
            case PixelFormat::BC7_UNORM:
            case PixelFormat::BC7_UNORM_SRGB:
                return 16;

            default:
                return 4;
            }
        }

        /// @brief Returns the byte size for a single vertex element of the given format.
        [[nodiscard]] inline constexpr uint32_t GetVertexFormatSize(RHIVertexFormat format)
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
            case RHIVertexFormat::UNorm8x4:
            case RHIVertexFormat::SNorm8x4:
                return 16;

            default:
                return 12; // Default to Float3 size
            }
        }

        /// @brief Returns true if the format uses block compression (BC1-BC7).
        [[nodiscard]] inline constexpr bool IsCompressedFormat(PixelFormat format)
        {
            switch (format)
            {
            case PixelFormat::BC1_UNORM:
            case PixelFormat::BC1_UNORM_SRGB:
            case PixelFormat::BC2_UNORM:
            case PixelFormat::BC3_UNORM:
            case PixelFormat::BC3_UNORM_SRGB:
            case PixelFormat::BC4_UNORM:
            case PixelFormat::BC5_UNORM:
            case PixelFormat::BC6H_UF16:
            case PixelFormat::BC7_UNORM:
            case PixelFormat::BC7_UNORM_SRGB:
                return true;
            default:
                return false;
            }
        }

        /// @brief Returns true if the format is a depth or depth-stencil format.
        [[nodiscard]] inline constexpr bool IsDepthStencilFormat(PixelFormat format)
        {
            switch (format)
            {
            case PixelFormat::D16_UNORM:
            case PixelFormat::D24_UNORM_S8_UINT:
            case PixelFormat::D32_FLOAT:
            case PixelFormat::D32_FLOAT_S8_UINT:
                return true;
            default:
                return false;
            }
        }

        /// @brief Returns true if the format includes a stencil component.
        [[nodiscard]] inline constexpr bool HasStencilComponent(PixelFormat format)
        {
            return format == PixelFormat::D24_UNORM_S8_UINT || format == PixelFormat::D32_FLOAT_S8_UINT;
        }

        /// @brief Returns true if the format uses sRGB color space.
        [[nodiscard]] inline constexpr bool IsSRGBFormat(PixelFormat format)
        {
            switch (format)
            {
            case PixelFormat::R8G8B8A8_UNORM_SRGB:
            case PixelFormat::B8G8R8A8_UNORM_SRGB:
            case PixelFormat::BC1_UNORM_SRGB:
            case PixelFormat::BC3_UNORM_SRGB:
            case PixelFormat::BC7_UNORM_SRGB:
                return true;
            default:
                return false;
            }
        }

    } // namespace RHI
} // namespace Spark
