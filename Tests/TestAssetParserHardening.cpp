// TestAssetParserHardening.cpp — SEC-assets regressions for engine-side asset
// parsers that consume untrusted content files: OBJ face indices walked by the
// tinyobjloader consumers, the Windows TGA texture loader, and the non-Windows
// EXR texture path. All tests share the AssetSec_ prefix, pinned in
// Tests/CMakeLists.txt (SparkAssetParserHardeningTests) together with the
// AssetSec_ SparkPak and stb_image tests.

#include "TestFramework.h"

#include "Core/Platform.h"
#include "Game/Model.h"
#include "Graphics/Mesh.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include "Graphics/AssetPipeline.h"
#include <d3d11.h>
#include <wrl/client.h>
#else
#include "Graphics/TextureSystem.h"
#endif

#if defined(SPARK_HAS_TINYEXR) && SPARK_HAS_TINYEXR
#include <tinyexr.h>
#endif

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace
{
    std::filesystem::path AssetSecDir()
    {
        const auto dir = std::filesystem::temp_directory_path() / "spark_assetsec_tests";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    std::filesystem::path WriteFile(const std::string& name, const std::string& contents)
    {
        const auto path = AssetSecDir() / name;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        return path;
    }

    [[maybe_unused]] std::filesystem::path WriteBytes(const std::string& name, const std::vector<uint8_t>& bytes)
    {
        return WriteFile(name, std::string(bytes.begin(), bytes.end()));
    }

    void RemoveAssetSecDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::temp_directory_path() / "spark_assetsec_tests", ec);
    }

#ifdef SPARK_PLATFORM_WINDOWS
    bool CreateTestDevice(Microsoft::WRL::ComPtr<ID3D11Device>& device,
                          Microsoft::WRL::ComPtr<ID3D11DeviceContext>& ctx)
    {
        D3D_FEATURE_LEVEL level{};
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                       &device, &level, &ctx);
        if (FAILED(hr))
        {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                   &device, &level, &ctx);
        }
        return SUCCEEDED(hr);
    }

    std::vector<uint8_t> TgaHeader(uint16_t width, uint16_t height, uint8_t bpp)
    {
        std::vector<uint8_t> header(18, 0);
        header[2] = 2; // uncompressed true-colour
        header[12] = static_cast<uint8_t>(width & 0xFFu);
        header[13] = static_cast<uint8_t>(width >> 8);
        header[14] = static_cast<uint8_t>(height & 0xFFu);
        header[15] = static_cast<uint8_t>(height >> 8);
        header[16] = bpp;
        return header;
    }
#endif
} // namespace

// ============================================================================
// OBJ: positive out-of-range face indices (tinyobjloader only warns)
// ============================================================================

TEST(AssetSec_MeshObjRejectsOutOfRangeFaceIndices)
{
    // One triangle whose third corner is vertex 100000000 of 3. Mesh::LoadFromFile
    // indexed attrib.vertices[3 * idx] directly: a read ~1.2 GB past the array.
    const auto position = WriteFile("oob_position.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 100000000\n");
    const auto normal = WriteFile("oob_normal.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nf 1//1 2//1 3//100000000\n");
    const auto texCoord = WriteFile("oob_texcoord.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nf 1/1 2/1 3/100000000\n");

    for (const auto& path : {position, normal, texCoord})
    {
        Mesh mesh;
        EXPECT_FALSE(mesh.LoadFromFile(path.wstring()));
        EXPECT_EQ(mesh.GetVertexCount(), 0u);
    }
    RemoveAssetSecDir();
}

TEST(AssetSec_ModelObjRejectsOutOfRangeFaceIndices)
{
#ifdef SPARK_PLATFORM_WINDOWS
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;
    if (!CreateTestDevice(device, ctx))
    {
        return; // Model::LoadObj requires a device; no WARP or hardware adapter here.
    }

    const auto hostile = WriteFile("model_oob.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 100000000\n");
    {
        Model model;
        EXPECT_TRUE(FAILED(model.LoadObj(hostile.wstring(), device.Get())));
    }

    // Control: the same triangle with in-range indices still loads.
    const auto valid = WriteFile("model_ok.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    {
        Model model;
        EXPECT_TRUE(SUCCEEDED(model.LoadObj(valid.wstring(), device.Get())));
    }
    RemoveAssetSecDir();
#else
    // Model::LoadObj on non-Windows only counts indices and never walks the
    // attribute arrays; the Linux OBJ consumers are covered by the Mesh test.
    EXPECT_TRUE(true);
#endif
}

// ============================================================================
// Windows TGA texture loader: header-driven allocation
// ============================================================================

TEST(AssetSec_TextureTgaRejectsHeaderLargerThanFile)
{
#ifdef SPARK_PLATFORM_WINDOWS
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;
    if (!CreateTestDevice(device, ctx))
    {
        return;
    }

    // An 18-byte header declaring 4096x4096x32bpp: the loader allocated 128 MB,
    // accepted the short read as zeros, and built a 4096x4096 texture.
    const auto truncated = WriteBytes("truncated.tga", TgaHeader(4096, 4096, 32));
    {
        TextureAsset texture(truncated.string());
        EXPECT_TRUE(SUCCEEDED(texture.Load(device.Get())));
        EXPECT_EQ(texture.GetWidth(), 2u); // checkerboard fallback
        EXPECT_EQ(texture.GetHeight(), 2u);
    }

    // Over the D3D11 limit with every pixel present: previously both full-size
    // buffers were allocated and CreateTexture2D then failed the whole load.
    std::vector<uint8_t> wide = TgaHeader(16385, 1, 32);
    wide.resize(wide.size() + 16385u * 4u, 0x7F);
    const auto widePath = WriteBytes("wide.tga", wide);
    {
        TextureAsset texture(widePath.string());
        EXPECT_TRUE(SUCCEEDED(texture.Load(device.Get())));
        EXPECT_EQ(texture.GetWidth(), 2u);
    }

    // Control: a complete 2x1 image with a 3-byte image ID field decodes, and
    // the ID bytes are skipped rather than read as pixels.
    std::vector<uint8_t> valid = TgaHeader(2, 1, 24);
    valid[0] = 3;
    const uint8_t idAndPixels[] = {0xEE, 0xEE, 0xEE, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
    valid.insert(valid.end(), std::begin(idAndPixels), std::end(idAndPixels));
    const auto validPath = WriteBytes("valid.tga", valid);
    {
        TextureAsset texture(validPath.string());
        EXPECT_TRUE(SUCCEEDED(texture.Load(device.Get())));
        EXPECT_EQ(texture.GetWidth(), 2u);
        EXPECT_EQ(texture.GetHeight(), 1u);
    }
    RemoveAssetSecDir();
#else
    // The Windows TGA loader is not built here; non-Windows textures decode
    // through the stb_image stub (AssetSec_Stb* tests).
    EXPECT_TRUE(true);
#endif
}

// ============================================================================
// Non-Windows EXR texture path: must use the bounded EXRLoader
// ============================================================================

TEST(AssetSec_TextureExrUsesBoundedLoader)
{
#if !defined(SPARK_PLATFORM_WINDOWS) && defined(SPARK_HAS_TINYEXR) && SPARK_HAS_TINYEXR
    const auto encode = [](int width, int height, std::vector<uint8_t>& out)
    {
        std::vector<float> rgba(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0.5f);
        unsigned char* encoded = nullptr;
        const char* error = nullptr;
        const int size = SaveEXRToMemory(rgba.data(), width, height, 4, 1, &encoded, &error);
        if (error)
        {
            FreeEXRErrorMessage(error);
        }
        if (size <= 0 || !encoded)
        {
            return false;
        }
        out.assign(encoded, encoded + size);
        std::free(encoded);
        return true;
    };

    // 16385 px wide: over EXRLoader's 16384 dimension cap. Raw LoadEXR accepted
    // it (its own threshold is 8M px per side) and the texture reported loaded.
    std::vector<uint8_t> wide;
    EXPECT_TRUE(encode(16385, 1, wide));
    const auto widePath = WriteBytes("wide.exr", wide);
    {
        Texture texture("wide", TextureDesc{});
        EXPECT_TRUE(FAILED(texture.CreateFromFile(widePath.string(), nullptr)));
        EXPECT_FALSE(texture.IsLoaded());
        EXPECT_EQ(texture.GetMemoryUsage(), 0u);
    }

    // Control: a small EXR still loads with its real dimensions.
    std::vector<uint8_t> small;
    EXPECT_TRUE(encode(2, 1, small));
    const auto smallPath = WriteBytes("small.exr", small);
    {
        Texture texture("small", TextureDesc{});
        EXPECT_TRUE(SUCCEEDED(texture.CreateFromFile(smallPath.string(), nullptr)));
        EXPECT_TRUE(texture.IsLoaded());
        EXPECT_EQ(texture.GetMemoryUsage(), static_cast<size_t>(2u * 1u * 16u));
    }
    RemoveAssetSecDir();
#else
    // Windows Texture::CreateFromFile has no EXR branch, and without tinyexr
    // there is no EXR path to bound.
    EXPECT_TRUE(true);
#endif
}
