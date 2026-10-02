/**
 * @file TestLegacyGameObjectMaterial.cpp
 * @brief The legacy GameObject draw must bind a confined authored basic material, and a basic
 *        material whose declared texture is rejected, missing or undecodable must say so.
 */
#include "TestFramework.h"
#include "ScopedLoggerBaseline.h"
#include "Core/EngineContext.h"
#include "Game/PlaneObject.h"
#include "Graphics/GraphicsEngine.h"
#include "Utils/Logger.h"

#include <d3d11.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <objbase.h>
#include <string>
#include <system_error>
#include <thread>
#include <vector>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

TEST(LegacyGameObject_AuthoredMaterialChangesBasicDrawBinding)
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                   &featureLevel, &context);
    EXPECT_TRUE(SUCCEEDED(hr));
    if (FAILED(hr))
        return;

    GraphicsEngine graphics;
    hr = graphics.InitializeFromDevice(device.Get(), context.Get());
    EXPECT_TRUE(SUCCEEDED(hr));
    if (FAILED(hr))
        return;

    // The basic image loader uses WIC, whose factory requires a COM apartment.
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    EXPECT_TRUE(SUCCEEDED(comResult) || comResult == RPC_E_CHANGED_MODE);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE)
        return;

    EngineContext engineContext(&graphics, nullptr, nullptr);
    EngineContext* previousContext = EngineContext::Get();
    EngineContext::SetInjected(&engineContext);

    {
        PlaneObject plane(2.0f, 2.0f);
        hr = plane.Initialize(device.Get(), context.Get());
        EXPECT_TRUE(SUCCEEDED(hr));
        if (SUCCEEDED(hr))
        {
            const std::string projectRoot = SPARK_TEST_SOURCE_DIR;
            EXPECT_TRUE(plane.SetMaterialProjectRoot(projectRoot));

            graphics.SetBasicShaders();
            ComPtr<ID3D11ShaderResourceView> defaultTexture;
            context->PSGetShaderResources(0, 1, defaultTexture.GetAddressOf());
            EXPECT_TRUE(defaultTexture != nullptr);

            plane.SetMaterialPath("Assets/Materials/Terrain_Dirt.json");
            plane.Render(DirectX::XMMatrixIdentity(), DirectX::XMMatrixIdentity());
            ComPtr<ID3D11ShaderResourceView> boundTexture;
            context->PSGetShaderResources(0, 1, boundTexture.GetAddressOf());

            const auto* material = graphics.GetOrLoadBasicMaterial("Assets/Materials/Terrain_Dirt.json", projectRoot);
            EXPECT_TRUE(material != nullptr);
            if (material)
            {
                EXPECT_TRUE(material->srv != nullptr);
                EXPECT_TRUE(boundTexture.Get() == material->srv.Get());
                EXPECT_TRUE(boundTexture.Get() != defaultTexture.Get());
            }

            // A malformed or non-material reference must not inherit the prior
            // draw's texture or turn into an arbitrary file load.
            for (const char* rejected : {"Assets/Materials/../Materials/Terrain_Dirt.json",
                                         "Assets/Textures/Terrain/dirt.png", "C:/outside/material.json", ""})
            {
                plane.SetMaterialPath(rejected);
                plane.Render(DirectX::XMMatrixIdentity(), DirectX::XMMatrixIdentity());
                ComPtr<ID3D11ShaderResourceView> afterRejected;
                context->PSGetShaderResources(0, 1, afterRejected.GetAddressOf());
                EXPECT_TRUE(afterRejected.Get() == defaultTexture.Get());
            }

            const std::string noAlbedoRoot = projectRoot + "/Tests/Fixtures/LegacyBasicMaterial";
            EXPECT_TRUE(plane.SetMaterialProjectRoot(noAlbedoRoot));
            plane.SetMaterialPath("Assets/Materials/NoAlbedo.json");
            const auto* noAlbedo = graphics.GetOrLoadBasicMaterial("Assets/Materials/NoAlbedo.json", noAlbedoRoot);
            EXPECT_TRUE(noAlbedo != nullptr);
            if (noAlbedo)
            {
                EXPECT_TRUE(noAlbedo->srv == nullptr);
                EXPECT_TRUE(noAlbedo->roughnessSrv != nullptr);
                plane.Render(DirectX::XMMatrixIdentity(), DirectX::XMMatrixIdentity());

                ComPtr<ID3D11ShaderResourceView> boundAlbedo;
                ComPtr<ID3D11ShaderResourceView> boundRoughness;
                context->PSGetShaderResources(0, 1, boundAlbedo.GetAddressOf());
                context->PSGetShaderResources(2, 1, boundRoughness.GetAddressOf());
                EXPECT_TRUE(boundAlbedo.Get() == defaultTexture.Get());
                EXPECT_TRUE(boundRoughness.Get() == noAlbedo->roughnessSrv.Get());

                ComPtr<ID3D11Buffer> perObject;
                context->VSGetConstantBuffers(0, 1, perObject.GetAddressOf());
                EXPECT_TRUE(perObject != nullptr);
                if (perObject)
                {
                    D3D11_BUFFER_DESC desc{};
                    perObject->GetDesc(&desc);
                    desc.Usage = D3D11_USAGE_STAGING;
                    desc.BindFlags = 0;
                    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    desc.MiscFlags = 0;
                    ComPtr<ID3D11Buffer> staging;
                    EXPECT_TRUE(SUCCEEDED(device->CreateBuffer(&desc, nullptr, staging.GetAddressOf())));
                    if (staging)
                    {
                        context->CopyResource(staging.Get(), perObject.Get());
                        D3D11_MAPPED_SUBRESOURCE mapped{};
                        EXPECT_TRUE(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
                        if (mapped.pData)
                        {
                            const auto* constants = static_cast<const PerObjectConstants*>(mapped.pData);
                            EXPECT_TRUE(constants->UVTiling.x == 4.0f && constants->UVTiling.y == 3.0f);
                            context->Unmap(staging.Get(), 0);
                        }
                    }
                }
            }

            std::error_code relativeError;
            const auto relativeRoot = std::filesystem::relative(std::filesystem::path(projectRoot),
                                                                std::filesystem::current_path(), relativeError);
            EXPECT_TRUE(!relativeError && !relativeRoot.empty() && !relativeRoot.is_absolute());
            if (!relativeError && !relativeRoot.empty() && !relativeRoot.is_absolute())
            {
                const std::u8string relativeUtf8 = relativeRoot.generic_u8string();
                const std::string relativeBytes(reinterpret_cast<const char*>(relativeUtf8.data()),
                                                relativeUtf8.size());
                EXPECT_TRUE(!plane.SetMaterialProjectRoot(relativeBytes));
            }

            EXPECT_TRUE(!plane.SetMaterialProjectRoot(""));
            plane.SetMaterialPath("Assets/Materials/Terrain_Dirt.json");
            plane.Render(DirectX::XMMatrixIdentity(), DirectX::XMMatrixIdentity());
            ComPtr<ID3D11ShaderResourceView> afterInvalidRoot;
            context->PSGetShaderResources(0, 1, afterInvalidRoot.GetAddressOf());
            EXPECT_TRUE(afterInvalidRoot.Get() == defaultTexture.Get());
        }
    }

    EngineContext::SetInjected(previousContext == EngineContext::GetOwned().get() ? nullptr : previousContext);
    if (comResult == S_OK || comResult == S_FALSE)
        CoUninitialize();
}

TEST(BasicMaterial_LoadsTextureOnFreshRenderThreadWithoutCallerCom)
{
    // "Without caller COM" means this thread never called CoInitializeEx. A
    // fresh thread reports CO_E_NOTINITIALIZED only while no other thread in
    // the process holds the MTA; once any thread does (other tests, OS or
    // driver worker threads), Windows places every uninitialized thread in the
    // *implicit* MTA. Both states mean the caller established no apartment;
    // an explicit STA/MTA (qualifier NONE) would still fail this check.
    const auto hasNoCallerApartment = []
    {
        APTTYPE apartmentType{};
        APTTYPEQUALIFIER qualifier{};
        const HRESULT result = CoGetApartmentType(&apartmentType, &qualifier);
        return result == CO_E_NOTINITIALIZED ||
               (result == S_OK && apartmentType == APTTYPE_MTA && qualifier == APTTYPEQUALIFIER_IMPLICIT_MTA);
    };

    bool noApartmentBeforeDevice = false;
    bool noApartmentBeforeLoad = false;
    HRESULT deviceResult = E_FAIL;
    HRESULT graphicsResult = E_FAIL;
    bool loaded = false;

    std::thread renderThread(
        [&]
        {
            noApartmentBeforeDevice = hasNoCallerApartment();

            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> context;
            D3D_FEATURE_LEVEL featureLevel{};
            deviceResult = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                             &device, &featureLevel, &context);
            if (FAILED(deviceResult))
                return;

            GraphicsEngine graphics;
            graphicsResult = graphics.InitializeFromDevice(device.Get(), context.Get());
            if (FAILED(graphicsResult))
                return;

            noApartmentBeforeLoad = hasNoCallerApartment();
            const auto* material =
                graphics.GetOrLoadBasicMaterial("Assets/Materials/Terrain_Dirt.json", SPARK_TEST_SOURCE_DIR);
            loaded = material != nullptr && material->srv != nullptr;
        });
    renderThread.join();

    EXPECT_TRUE(noApartmentBeforeDevice);
    EXPECT_TRUE(SUCCEEDED(deviceResult));
    EXPECT_TRUE(SUCCEEDED(graphicsResult));
    EXPECT_TRUE(noApartmentBeforeLoad);
    EXPECT_TRUE(loaded);
}

// ---------------------------------------------------------------------------------------------
// ENG-220: a declared texture that cannot be used fails actionably. A rejected (rooted, absolute
// or escaping) path is malformed content and fails the material; a missing or undecodable file
// logs an error naming the material, the key, the path and the reason. Before, all three loaded
// a material with a null srv and at most a texture-level warning that never named the material.
// ---------------------------------------------------------------------------------------------
namespace
{
    /// Records every Error-level message while installed; the baseline logger is restored afterwards.
    class ErrorLogCapture
    {
      public:
        ErrorLogCapture()
        {
            auto lines = m_lines;
            Spark::Logger::Get().AddSink(std::make_unique<Spark::CallbackSink>(
                [lines](const Spark::LogMessage& msg)
                {
                    if (msg.level >= Spark::LogLevel::Error)
                        lines->push_back(msg.message);
                }));
        }

        /// True when one error line contains every needle.
        bool HasErrorWith(const std::vector<std::string>& needles) const
        {
            for (const auto& line : *m_lines)
            {
                bool all = true;
                for (const auto& needle : needles)
                    all = all && line.find(needle) != std::string::npos;
                if (all)
                    return true;
            }
            return false;
        }

      private:
        ScopedLoggerBaseline m_baseline; // declared first: restores before the sink is added
        std::shared_ptr<std::vector<std::string>> m_lines = std::make_shared<std::vector<std::string>>();
    };

    std::string PathUtf8(const std::filesystem::path& path)
    {
        const std::u8string utf8 = path.generic_u8string();
        return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    }

    /// A throwaway project root holding one material JSON (and optionally a texture file).
    struct MaterialProject
    {
        MaterialProject(const char* name, const std::string& materialJson)
            : root(std::filesystem::temp_directory_path() / (std::string("spark_basic_material_") + name))
        {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
            std::filesystem::create_directories(root / "Assets" / "Materials", ec);
            std::filesystem::create_directories(root / "Assets" / "Textures", ec);
            std::ofstream(root / "Assets" / "Materials" / "Broken.json", std::ios::binary) << materialJson;
        }

        MaterialProject(const MaterialProject&) = delete;
        MaterialProject& operator=(const MaterialProject&) = delete;

        ~MaterialProject()
        {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
        }

        std::filesystem::path root;
    };

    /// A WARP-backed GraphicsEngine; asserts rather than skips so the lane cannot pass empty.
    struct WarpGraphics
    {
        WarpGraphics()
        {
            D3D_FEATURE_LEVEL featureLevel{};
            ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                                    D3D11_SDK_VERSION, &device, &featureLevel, &context)));
            ASSERT_TRUE(SUCCEEDED(graphics.InitializeFromDevice(device.Get(), context.Get())));
        }

        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        GraphicsEngine graphics;
    };

    constexpr const char* kBrokenMaterial = "Assets/Materials/Broken.json";
} // namespace

TEST(BasicMaterial_MissingAlbedoReportsNamedPath)
{
    MaterialProject project("missing", R"({"albedo":"Textures/NotThere.png","tiling":[2,2]})");
    WarpGraphics warp;
    ErrorLogCapture log;

    const auto* material = warp.graphics.GetOrLoadBasicMaterial(kBrokenMaterial, PathUtf8(project.root));
    // The material stays cached so an editor import of the texture is retried, but the error names
    // the material, the key, the declared texture and why it did not load.
    ASSERT_TRUE(material != nullptr);
    EXPECT_TRUE(material->srv == nullptr);
    EXPECT_TRUE(log.HasErrorWith({kBrokenMaterial, "'albedo'", "NotThere.png", "missing file"}));

    // The retry path picks the texture up once it exists and has been invalidated.
    const std::filesystem::path texture = project.root / "Assets" / "Textures" / "NotThere.png";
    const std::filesystem::path shipped =
        std::filesystem::path(SPARK_TEST_SOURCE_DIR) / "Assets" / "Textures" / "Terrain" / "dirt.png";
    ASSERT_TRUE(std::filesystem::copy_file(shipped, texture));
    EXPECT_TRUE(warp.graphics.InvalidateBasicTexture(PathUtf8(texture)));
    const auto* retried = warp.graphics.GetOrLoadBasicMaterial(kBrokenMaterial, PathUtf8(project.root));
    EXPECT_TRUE(retried == material);
    EXPECT_TRUE(retried && retried->srv != nullptr);
}

TEST(BasicMaterial_RootedTexturePathFails)
{
    for (const char* declared : {"/Textures/dirt.png", "C:/outside/dirt.png", "../../outside/dirt.png"})
    {
        MaterialProject project("rooted", std::string(R"({"normal":")") + declared + R"("})");
        WarpGraphics warp;
        ErrorLogCapture log;

        EXPECT_TRUE(warp.graphics.GetOrLoadBasicMaterial(kBrokenMaterial, PathUtf8(project.root)) == nullptr);
        EXPECT_TRUE(log.HasErrorWith({kBrokenMaterial, "'normal'", declared, "rejected texture path"}));
        // Negative-cached: the second lookup fails too.
        EXPECT_TRUE(warp.graphics.GetOrLoadBasicMaterial(kBrokenMaterial, PathUtf8(project.root)) == nullptr);
    }
}

TEST(BasicMaterial_CorruptTextureReportsDecodeFailure)
{
    MaterialProject project("corrupt", R"({"roughness":"Textures/Truncated.png"})");
    {
        // A PNG signature and the start of an IHDR chunk, then nothing: WIC cannot decode it.
        const unsigned char truncated[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 0, 0, 0, 13, 'I', 'H'};
        std::ofstream file(project.root / "Assets" / "Textures" / "Truncated.png", std::ios::binary);
        file.write(reinterpret_cast<const char*>(truncated), sizeof(truncated));
    }
    WarpGraphics warp;
    ErrorLogCapture log;

    const auto* material = warp.graphics.GetOrLoadBasicMaterial(kBrokenMaterial, PathUtf8(project.root));
    ASSERT_TRUE(material != nullptr);
    EXPECT_TRUE(material->roughnessSrv == nullptr);
    EXPECT_TRUE(log.HasErrorWith({kBrokenMaterial, "'roughness'", "Truncated.png", "decode failure"}));
}
