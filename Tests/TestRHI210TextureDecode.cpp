/** @file TestRHI210TextureDecode.cpp
 * @brief Windows/WARP regression for bounded PNG decoding and real uploaded texels.
 */
#include "TestFramework.h"
#include "Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include "Graphics/AssetPipeline.h"
#include "RHI210D3D11EngineFixture.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace
{
    class TextureFixture
    {
      public:
        TextureFixture()
        {
            static std::atomic<unsigned int> sequence{0};
            m_directory = std::filesystem::temp_directory_path() /
                          ("spark-rhi210-" + std::to_string(GetCurrentProcessId()) + "-" +
                           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                           std::to_string(sequence.fetch_add(1)));
            if (!std::filesystem::create_directory(m_directory))
            {
                throw std::runtime_error("RHI210 texture fixture directory collision");
            }
        }
        ~TextureFixture()
        {
            std::error_code error;
            std::filesystem::remove_all(m_directory, error);
        }
        TextureFixture(const TextureFixture&) = delete;
        TextureFixture& operator=(const TextureFixture&) = delete;

        std::filesystem::path Write(const std::vector<uint8_t>& bytes) const
        {
            const auto path = m_directory / "fixture.png";
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            output.close();
            if (!output)
            {
                throw std::runtime_error("RHI210 texture fixture write failed");
            }
            return path;
        }

      private:
        std::filesystem::path m_directory;
    };

    std::vector<uint8_t> TinyPng()
    {
        // Generated with Python stdlib zlib; RGBA pixels red then green.
        return {
            0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
            0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0xF4, 0x22, 0x7F, 0x8A, 0x00, 0x00, 0x00,
            0x0E, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0xF8, 0xCF, 0xC0, 0xF0, 0x1F, 0x04, 0x01, 0x10, 0xF8, 0x03,
            0xFD, 0x4E, 0x95, 0xC1, 0x6F, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
        };
    }

    uint32_t HeaderCrc(const std::vector<uint8_t>& bytes)
    {
        uint32_t crc = 0xFFFFFFFFu;
        for (size_t index = 12; index < 29; ++index)
        {
            crc ^= bytes[index];
            for (int bit = 0; bit < 8; ++bit)
            {
                crc = (crc >> 1) ^ ((crc & 1u) != 0u ? 0xEDB88320u : 0u);
            }
        }
        return ~crc;
    }
} // namespace

TEST(RHI210_TextureAsset_LoadsPngDimensionsAndTexels)
{
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                            &device, nullptr, &context)));
    TextureFixture fixture;
    TextureAsset texture(fixture.Write(TinyPng()).string());
    ASSERT_TRUE(SUCCEEDED(texture.Load(device.Get())));
    ASSERT_EQ(texture.GetWidth(), 2u);
    ASSERT_EQ(texture.GetHeight(), 1u); // old fallback was 2x2
    ASSERT_TRUE(texture.GetSRV() != nullptr);
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;
    texture.GetSRV()->GetResource(&resource);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> source;
    ASSERT_TRUE(SUCCEEDED(resource.As(&source)));
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)));
    context->CopyResource(staging.Get(), source.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    ASSERT_TRUE(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
    const auto* pixels = static_cast<const uint8_t*>(mapped.pData);
    EXPECT_EQ(pixels[0], 255u);
    EXPECT_EQ(pixels[1], 0u);
    EXPECT_EQ(pixels[2], 0u);
    EXPECT_EQ(pixels[3], 255u);
    EXPECT_EQ(pixels[4], 0u);
    EXPECT_EQ(pixels[5], 255u);
    EXPECT_EQ(pixels[6], 0u);
    EXPECT_EQ(pixels[7], 255u);
    context->Unmap(staging.Get(), 0);
}

TEST(RHI210_TextureAsset_RejectsOversizedPngHeader)
{
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                            &device, nullptr, nullptr)));
    auto bytes = TinyPng();
    bytes[18] = 0x40;
    bytes[19] = 0x01; // width 16385 exceeds D3D11's 16384 maximum
    const uint32_t crc = HeaderCrc(bytes);
    for (unsigned int byte = 0; byte < 4; ++byte)
    {
        bytes[29 + byte] = static_cast<uint8_t>(crc >> (24u - byte * 8u));
    }
    TextureFixture fixture;
    TextureAsset texture(fixture.Write(bytes).string());
    ASSERT_TRUE(SUCCEEDED(texture.Load(device.Get())));
    EXPECT_EQ(texture.GetWidth(), 2u);
    EXPECT_EQ(texture.GetHeight(), 2u);
}

TEST(RHI210_TextureAsset_DrawListUsesUploadedTextureCoordinates)
{
    // The draw list uploads MeshAssetData::Vertex, whose UV follows the tangent.
    // Reading the legacy mesh's UV offset instead samples a constant texel even
    // though texture decoding, the SRV and D3D11 validation all succeed.
    TextureFixture fixture;
    const std::string texturePath = fixture.Write(TinyPng()).string();
    const auto meshFile = std::filesystem::path(texturePath).parent_path() / "quad.obj";
    {
        std::ofstream output(meshFile, std::ios::binary);
        output << "v -1 -1 0\nv 1 -1 0\nv 1 1 0\nv -1 1 0\n"
                  "vt 0 1\nvt 1 1\nvt 1 0\nvt 0 0\n"
                  "vn 0 0 -1\n"
                  "f 1/1/1 4/4/1 3/3/1\nf 1/1/1 3/3/1 2/2/1\n";
        ASSERT_TRUE(output.good());
    }
    const std::string meshPath = meshFile.string();
    RHI210::WarpGraphicsEngine warp(64, 64);
    ASSERT_TRUE(warp.Ready());
    GraphicsEngine& engine = warp.Engine();
    AssetPipeline* assets = engine.GetAssetPipeline();
    ASSERT_TRUE(assets != nullptr);
    ASSERT_TRUE(assets->LoadMesh(meshPath) != nullptr);
    auto texture = assets->LoadTexture(texturePath);
    ASSERT_TRUE(texture != nullptr);
    ASSERT_EQ(texture->GetWidth(), 2u);
    ASSERT_EQ(texture->GetHeight(), 1u);

    using namespace DirectX;
    const XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(0, 0, -2, 1), XMVectorZero(), XMVectorSet(0, 1, 0, 0));
    const XMMATRIX projection = XMMatrixOrthographicLH(2, 2, 0.1f, 10);
    RHI210::Frame frame;
    for (int pass = 0; pass < 2; ++pass)
    {
        engine.SubmitMeshForRendering(meshPath, texturePath, XMMatrixIdentity(), false);
        frame = RHI210::RenderObjectsFrame(engine, view, projection, {});
    }
    ASSERT_EQ(frame.width, 64u);
    ASSERT_EQ(frame.height, 64u);
    // Probe both texel interiors, away from the bilinear transition and edges.
    // A constant UV cannot satisfy the opposite red/green channel dominance.
    size_t redPixels = 0;
    size_t greenPixels = 0;
    for (uint32_t y = 8; y < 56; ++y)
    {
        for (uint32_t x = 12; x < 20; ++x)
        {
            const auto* red = frame.At(x, y);
            const auto* green = frame.At(x + 32, y);
            redPixels += red[0] > 100 && red[0] > 3u * red[1] && red[2] < 8;
            greenPixels += green[1] > 100 && green[1] > 3u * green[0] && green[2] < 8;
        }
    }
    EXPECT_EQ(redPixels, size_t(384));
    EXPECT_EQ(greenPixels, size_t(384));
}
#endif // SPARK_PLATFORM_WINDOWS
