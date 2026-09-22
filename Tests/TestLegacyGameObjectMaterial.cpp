/**
 * @file TestLegacyGameObjectMaterial.cpp
 * @brief The legacy GameObject draw must bind a confined authored basic material.
 */
#include "TestFramework.h"
#include "Core/EngineContext.h"
#include "Game/PlaneObject.h"
#include "Graphics/GraphicsEngine.h"

#include <d3d11.h>
#include <filesystem>
#include <objbase.h>
#include <thread>
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
    HRESULT apartmentBeforeDevice = E_FAIL;
    HRESULT apartmentBeforeLoad = E_FAIL;
    HRESULT deviceResult = E_FAIL;
    HRESULT graphicsResult = E_FAIL;
    bool loaded = false;

    std::thread renderThread(
        [&]
        {
            APTTYPE apartmentType{};
            APTTYPEQUALIFIER qualifier{};
            apartmentBeforeDevice = CoGetApartmentType(&apartmentType, &qualifier);

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

            apartmentBeforeLoad = CoGetApartmentType(&apartmentType, &qualifier);
            const auto* material =
                graphics.GetOrLoadBasicMaterial("Assets/Materials/Terrain_Dirt.json", SPARK_TEST_SOURCE_DIR);
            loaded = material != nullptr && material->srv != nullptr;
        });
    renderThread.join();

    EXPECT_TRUE(apartmentBeforeDevice == CO_E_NOTINITIALIZED);
    EXPECT_TRUE(SUCCEEDED(deviceResult));
    EXPECT_TRUE(SUCCEEDED(graphicsResult));
    EXPECT_TRUE(apartmentBeforeLoad == CO_E_NOTINITIALIZED);
    EXPECT_TRUE(loaded);
}
