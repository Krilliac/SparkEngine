/**
 * @file TestPostProcessingPipelineD3D11.cpp
 * @brief Live D3D11 regression coverage for post-process target routing.
 */
#include "TestFramework.h"
#include "Graphics/PostProcessingPipeline.h"

#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdint>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace Spark::Graphics;

namespace
{
    struct D3D11Fixture
    {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        ComPtr<ID3D11InfoQueue> infoQueue;
    };

    bool CreateFixture(D3D11Fixture& fixture)
    {
        D3D_FEATURE_LEVEL featureLevel{};
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0,
                                       D3D11_SDK_VERSION, fixture.device.GetAddressOf(), &featureLevel,
                                       fixture.context.GetAddressOf());
        if (FAILED(hr))
        {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                   fixture.device.GetAddressOf(), &featureLevel, fixture.context.GetAddressOf());
        }
        if (FAILED(hr))
            return false;

        fixture.device.As(&fixture.infoQueue);
        return true;
    }

    bool CreateColorTarget(ID3D11Device* device, UINT width, UINT height, ComPtr<ID3D11Texture2D>& texture,
                           ComPtr<ID3D11RenderTargetView>& rtv, ComPtr<ID3D11ShaderResourceView>& srv)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, texture.GetAddressOf())) &&
               SUCCEEDED(device->CreateRenderTargetView(texture.Get(), nullptr, rtv.GetAddressOf())) &&
               SUCCEEDED(device->CreateShaderResourceView(texture.Get(), nullptr, srv.GetAddressOf()));
    }

    bool ReadCenterPixel(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture,
                         uint8_t (&rgba)[4])
    {
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;

        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf())))
            return false;
        context->CopyResource(staging.Get(), texture);
        context->Flush();

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            return false;
        const auto* row = static_cast<const uint8_t*>(mapped.pData) + (desc.Height / 2) * mapped.RowPitch;
        const auto* pixel = row + (desc.Width / 2) * 4;
        for (int i = 0; i < 4; ++i)
            rgba[i] = pixel[i];
        context->Unmap(staging.Get(), 0);
        return true;
    }

    int CountBindingHazards(ID3D11InfoQueue* infoQueue)
    {
        if (!infoQueue)
            return 0;
        int hazards = 0;
        const UINT64 messageCount = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
        for (UINT64 i = 0; i < messageCount; ++i)
        {
            SIZE_T size = 0;
            infoQueue->GetMessage(i, nullptr, &size);
            std::vector<uint8_t> storage(size);
            auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
            if (SUCCEEDED(infoQueue->GetMessage(i, message, &size)) &&
                (message->ID == D3D11_MESSAGE_ID_DEVICE_PSSETSHADERRESOURCES_HAZARD ||
                 message->ID == D3D11_MESSAGE_ID_DEVICE_OMSETRENDERTARGETS_HAZARD))
            {
                ++hazards;
            }
        }
        return hazards;
    }

    bool NearByte(uint8_t actual, float expected)
    {
        return std::abs(static_cast<int>(actual) - static_cast<int>(expected * 255.0f)) <= 4;
    }
} // namespace

TEST(PostProcessingD3D11_ZeroAndOnePassRouteWithoutViewHazards)
{
    constexpr UINT width = 16;
    constexpr UINT height = 16;
    D3D11Fixture fixture;
    EXPECT_TRUE(CreateFixture(fixture));
    if (!fixture.device)
        return;

    ComPtr<ID3D11Texture2D> inputTexture, outputTexture;
    ComPtr<ID3D11RenderTargetView> inputRTV, outputRTV;
    ComPtr<ID3D11ShaderResourceView> inputSRV, outputSRV;
    EXPECT_TRUE(CreateColorTarget(fixture.device.Get(), width, height, inputTexture, inputRTV, inputSRV));
    EXPECT_TRUE(CreateColorTarget(fixture.device.Get(), width, height, outputTexture, outputRTV, outputSRV));
    if (!inputTexture || !outputTexture)
        return;

    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MaxDepth = 1.0f;
    fixture.context->RSSetViewports(1, &viewport);

    const float sceneColor[4] = {0.2f, 0.4f, 0.6f, 1.0f};
    fixture.context->ClearRenderTargetView(inputRTV.Get(), sceneColor);
    if (fixture.infoQueue)
        fixture.infoQueue->ClearStoredMessages();

    PostProcessingPipeline pipeline;
    pipeline.SetDevice(fixture.device.Get(), fixture.context.Get());
    EXPECT_TRUE(pipeline.Initialize(width, height));

    // The windowed engine renders directly into a shader-readable backbuffer,
    // so its zero-pass path uses the same resource for input and output. This
    // must be a true no-op, never a simultaneous SRV/RTV bind.
    pipeline.SetInputSRV(inputSRV.Get());
    pipeline.SetOutputRTV(inputRTV.Get());
    pipeline.Process(1.0f / 60.0f);
    pipeline.Render();
    uint8_t pixel[4]{};
    EXPECT_TRUE(ReadCenterPixel(fixture.device.Get(), fixture.context.Get(), inputTexture.Get(), pixel));
    EXPECT_TRUE(NearByte(pixel[0], sceneColor[0]));
    EXPECT_TRUE(NearByte(pixel[1], sceneColor[1]));
    EXPECT_TRUE(NearByte(pixel[2], sceneColor[2]));

    // A distinct destination receives an identity composite for callers that
    // keep their scene color and presentation target separate.
    pipeline.SetInputSRV(inputSRV.Get());
    pipeline.SetOutputRTV(outputRTV.Get());
    pipeline.Process(1.0f / 60.0f);
    EXPECT_TRUE(pipeline.GetOutputSRV() == inputSRV.Get());
    EXPECT_TRUE(pipeline.GetOutputRTV() == nullptr);
    pipeline.Render();

    EXPECT_TRUE(ReadCenterPixel(fixture.device.Get(), fixture.context.Get(), outputTexture.Get(), pixel));
    EXPECT_TRUE(NearByte(pixel[0], sceneColor[0]));
    EXPECT_TRUE(NearByte(pixel[1], sceneColor[1]));
    EXPECT_TRUE(NearByte(pixel[2], sceneColor[2]));

    // One identity-strength sharpen pass must write ping-pong target zero,
    // then composite that distinct SRV into the explicit output RTV.
    pipeline.SetEffectEnabled(PostProcessPass::Sharpen, true);
    pipeline.GetSharpenSettings().amount = 0.0f;
    pipeline.GetSharpenSettings().threshold = 0.0f;
    pipeline.GetSharpenSettings().adaptiveSharpening = false;
    pipeline.SetInputSRV(inputSRV.Get());
    pipeline.SetOutputRTV(outputRTV.Get());
    pipeline.Process(1.0f / 60.0f);
    EXPECT_TRUE(pipeline.GetOutputSRV() != nullptr);
    EXPECT_TRUE(pipeline.GetOutputSRV() != inputSRV.Get());
    EXPECT_TRUE(pipeline.GetOutputRTV() != nullptr);
    pipeline.Render();

    EXPECT_TRUE(ReadCenterPixel(fixture.device.Get(), fixture.context.Get(), outputTexture.Get(), pixel));
    EXPECT_TRUE(NearByte(pixel[0], sceneColor[0]));
    EXPECT_TRUE(NearByte(pixel[1], sceneColor[1]));
    EXPECT_TRUE(NearByte(pixel[2], sceneColor[2]));
    EXPECT_EQ(CountBindingHazards(fixture.infoQueue.Get()), 0);

    pipeline.Resize(width * 2, height * 2);
    pipeline.Shutdown();
    EXPECT_TRUE(pipeline.GetOutputSRV() == nullptr);
    EXPECT_TRUE(pipeline.GetOutputRTV() == nullptr);
}
