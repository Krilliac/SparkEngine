/**
 * @file TextureSystemLinuxTexture.cpp
 * @brief Texture class implementation for Linux (stb_image/tinyexr CPU-side image loading)
 *
 * Split from TextureSystemLinux.cpp, which keeps the TextureSystem manager,
 * console operations, and format utility functions. The Windows counterpart
 * lives in TextureSystemWindows.cpp.
 */

#include "../Core/Platform.h"
#ifndef SPARK_PLATFORM_WINDOWS

#include "TextureSystem.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

#if SPARK_HAS_STB_IMAGE
#include <stb_image.h>
#endif

#if SPARK_HAS_TINYEXR
#include "EXRLoader.h"
#endif

// ============================================================================
// Texture class (Linux — stb_image/tinyexr for real image loading)
// ============================================================================

Texture::Texture(const std::string& name, const TextureDesc& desc) : m_name(name), m_desc(desc) {}

HRESULT Texture::CreateFromFile(const std::string& filePath, ID3D11Device* /*device*/)
{
#if SPARK_HAS_TINYEXR
    // Check for EXR format — tinyexr handles OpenEXR natively
    {
        std::string ext = std::filesystem::path(filePath).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".exr")
        {
            // Route through the bounded EXRLoader (16 MiB input, 16384 px per
            // side, R/G/B[/A] half/float, 128 MiB working set) instead of raw
            // tinyexr, whose own limits let a small header drive multi-GB
            // allocations. A rejected EXR fails closed: no stb fallback can
            // decode it, and "loaded" with estimated size would hide the refusal.
            constexpr uintmax_t kMaxExrFileBytes = 16ull * 1024ull * 1024ull;
            std::error_code ec;
            const uintmax_t fileBytes = std::filesystem::file_size(filePath, ec);
            Spark::Graphics::EXRImage image;
            bool decoded = false;
            if (!ec && fileBytes > 0 && fileBytes <= kMaxExrFileBytes)
            {
                std::vector<uint8_t> bytes(static_cast<size_t>(fileBytes));
                std::ifstream file(filePath, std::ios::binary);
                file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                decoded = file && static_cast<uintmax_t>(file.gcount()) == fileBytes &&
                          Spark::Graphics::EXRLoader::Load(bytes.data(), bytes.size(), image);
            }
            if (!decoded)
            {
                fprintf(stderr, "[TextureSystem] rejected EXR: %s\n", filePath.c_str());
                m_loaded = false;
                m_memoryUsage = 0;
                return E_FAIL;
            }

            m_desc.width = image.width;
            m_desc.height = image.height;
            m_desc.format = TextureFormat::R32G32B32A32_FLOAT;
            m_memoryUsage = static_cast<size_t>(static_cast<uint64_t>(image.width) * image.height * 16u);
            m_loaded = true;
            return S_OK;
        }
    }
#endif // SPARK_HAS_TINYEXR

#if SPARK_HAS_STB_IMAGE
    // Check for HDR format first (stbi_loadf returns float data)
    if (stbi_is_hdr(filePath.c_str()))
    {
        int width = 0, height = 0, channels = 0;
        float* hdrPixels = stbi_loadf(filePath.c_str(), &width, &height, &channels, 4);
        if (hdrPixels)
        {
            m_desc.width = static_cast<uint32_t>(width);
            m_desc.height = static_cast<uint32_t>(height);
            m_desc.format = TextureFormat::R32G32B32A32_FLOAT;
            m_memoryUsage = static_cast<size_t>(static_cast<uint64_t>(width) * static_cast<uint64_t>(height) *
                                                16u); // 4 floats per pixel
            m_loaded = true;
            stbi_image_free(hdrPixels);
            return S_OK;
        }
    }

    // Standard LDR image loading (PNG, JPG, BMP, TGA)
    int width = 0, height = 0, channels = 0;
    stbi_uc* pixels = stbi_load(filePath.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (pixels)
    {
        m_desc.width = static_cast<uint32_t>(width);
        m_desc.height = static_cast<uint32_t>(height);
        m_memoryUsage = static_cast<size_t>(static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 4u);
        m_loaded = true;
        stbi_image_free(pixels);
        return S_OK;
    }
    fprintf(stderr, "[TextureSystem] stb_image failed to load: %s (%s)\n", filePath.c_str(), stbi_failure_reason());
#endif
    // A missing, corrupt, truncated or unsupported file fails closed. Reporting
    // it as loaded (with a size estimated from the requested desc) would let
    // callers cache and bind a texture that holds no decoded pixels.
    m_loaded = false;
    m_memoryUsage = 0;
    return E_FAIL;
}

HRESULT Texture::CreateFromData(const void* data, size_t dataSize, ID3D11Device* /*device*/)
{
#if SPARK_HAS_STB_IMAGE
    // If data looks like an encoded image (not raw pixel data), try decoding it
    if (data && dataSize > 4)
    {
        const auto* bytes = static_cast<const unsigned char*>(data);
        // Check for common image signatures: PNG, JPEG, BMP
        bool isEncodedImage = (bytes[0] == 0x89 && bytes[1] == 'P') ||  // PNG
                              (bytes[0] == 0xFF && bytes[1] == 0xD8) || // JPEG
                              (bytes[0] == 'B' && bytes[1] == 'M');     // BMP
        if (isEncodedImage)
        {
            int width = 0, height = 0, channels = 0;
            stbi_uc* pixels =
                stbi_load_from_memory(bytes, static_cast<int>(dataSize), &width, &height, &channels, STBI_rgb_alpha);
            if (pixels)
            {
                m_desc.width = static_cast<uint32_t>(width);
                m_desc.height = static_cast<uint32_t>(height);
                m_memoryUsage = static_cast<size_t>(static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 4u);
                m_loaded = true;
                stbi_image_free(pixels);
                return S_OK;
            }
        }
    }
#endif
    m_loaded = true;
    m_memoryUsage = dataSize;
    return S_OK;
}

HRESULT Texture::CreateRenderTarget(ID3D11Device* /*device*/)
{
    m_loaded = true;
    m_memoryUsage = static_cast<size_t>(static_cast<uint64_t>(m_desc.width) * m_desc.height * 4u);
    return S_OK;
}

HRESULT Texture::CreateDepthStencil(ID3D11Device* /*device*/)
{
    m_loaded = true;
    m_memoryUsage = static_cast<size_t>(static_cast<uint64_t>(m_desc.width) * m_desc.height * 4u);
    return S_OK;
}

void Texture::Release()
{
    m_loaded = false;
    m_memoryUsage = 0;
}

void Texture::Bind(ID3D11DeviceContext* /*context*/, uint32_t /*slot*/)
{
    // No-op on Linux
}

void Texture::UnBind(ID3D11DeviceContext* /*context*/, uint32_t /*slot*/)
{
    // No-op on Linux
}

HRESULT Texture::CreateViews(ID3D11Device* /*device*/)
{
    return S_OK;
}

DXGI_FORMAT Texture::GetDXGIFormat(TextureFormat /*format*/) const
{
    return static_cast<DXGI_FORMAT>(0);
}

#endif // !SPARK_PLATFORM_WINDOWS
