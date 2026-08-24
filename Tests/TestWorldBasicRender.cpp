/**
 * @file TestWorldBasicRender.cpp
 * @brief GPU-headless smoke test proving GraphicsEngine::InitializeFromDevice()
 *        lets Spark::RenderWorldBasic() draw real geometry into an offscreen
 *        render target on a caller-owned D3D11 device (no swapchain).
 *
 * This is the verifiable core of the editor-attach path (task B1a): the
 * editor owns its own D3D11 device and wants to drive the shared
 * WorldBasicRenderer through a GraphicsEngine attached to it. Task B1b wires
 * this into the actual editor viewport.
 */
#include "TestFramework.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Components/CoreComponents.h"
#include "Graphics/GraphicsEngine.h"
#include "Graphics/Mesh.h"
#include "Graphics/WorldBasicRenderer.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <cstdint>
#include <cstdlib>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

TEST(WorldBasicRender_DrawsGeometryIntoOffscreenRTV)
{
    // 1) Create a headless D3D11 device (no swapchain).
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev,
                                   &fl, &ctx);
    if (FAILED(hr)) // fall back to WARP (test agents may lack a GPU)
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl,
                               &ctx);
    EXPECT_TRUE(SUCCEEDED(hr));
    if (FAILED(hr))
        return;

    // 2) Offscreen 256x256 RGBA render target.
    const UINT W = 256, H = 256;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = W;
    td.Height = H;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> rt;
    EXPECT_TRUE(SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &rt)));
    ComPtr<ID3D11RenderTargetView> rtv;
    EXPECT_TRUE(SUCCEEDED(dev->CreateRenderTargetView(rt.Get(), nullptr, &rtv)));
    // Depth buffer (basic shader path likely needs a bound DSV; create one).
    D3D11_TEXTURE2D_DESC dd = td;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth;
    dev->CreateTexture2D(&dd, nullptr, &depth);
    ComPtr<ID3D11DepthStencilView> dsv;
    if (depth)
        dev->CreateDepthStencilView(depth.Get(), nullptr, &dsv);

    // 3) Attach a GraphicsEngine to this device. InitializeFromDevice() now
    // also creates the same rasterizer/depth-stencil/blend state objects the
    // windowed path uses, and Spark::RenderWorldBasic() binds them itself via
    // GraphicsEngine::ApplyBasicRenderStates() — so the test no longer needs
    // to create or bind ANY pipeline state before drawing. This is the
    // self-sufficiency this test proves: RenderWorldBasic is safe to call
    // with nothing but a bound render target + viewport.
    GraphicsEngine g;
    EXPECT_TRUE(SUCCEEDED(g.InitializeFromDevice(dev.Get(), ctx.Get())));

    // 4) Bind the RTV + viewport, clear to a known background, draw a World.
    const float bg[4] = {0.10f, 0.12f, 0.15f, 1.0f};
    ID3D11RenderTargetView* rtvs[1] = {rtv.Get()};
    ctx->OMSetRenderTargets(1, rtvs, dsv.Get());
    ctx->ClearRenderTargetView(rtv.Get(), bg);
    if (dsv)
        ctx->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    D3D11_VIEWPORT vp{};
    vp.Width = (float)W;
    vp.Height = (float)H;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    World w;
    EntityID e = w.CreateEntity("Cube");
    w.AddComponent<Transform>(e); // identity at origin
    MeshRenderer& mr = w.AddComponent<MeshRenderer>(e);
    // Non-empty but non-existent path: WorldMeshCache::GetOrLoad early-returns
    // on an EMPTY path (so "" would draw nothing), but for a non-existent file
    // it proceeds into LoadOrPlaceholderMesh, whose file-load fails and falls
    // back to a procedural unit cube at the origin — guaranteed geometry with
    // no asset-file dependency.
    mr.meshPath = "__placeholder_unit_cube__.obj";

    XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(2, 2, -3, 1), XMVectorSet(0, 0, 0, 1), XMVectorSet(0, 1, 0, 0));
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(60.f), (float)W / H, 0.1f, 100.f);
    Spark::WorldMeshCache cache;
    Spark::RenderWorldBasic(w, g, cache, view, proj);
    ctx->Flush();

    // 5) Read back and assert some pixels differ from the clear color (geometry drew).
    D3D11_TEXTURE2D_DESC sd = td;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    EXPECT_TRUE(SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &staging)));
    ctx->CopyResource(staging.Get(), rt.Get());
    D3D11_MAPPED_SUBRESOURCE ms{};
    EXPECT_TRUE(SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &ms)));
    int differing = 0;
    const uint8_t bgR = (uint8_t)(bg[0] * 255), bgG = (uint8_t)(bg[1] * 255), bgB = (uint8_t)(bg[2] * 255);
    for (UINT y = 0; y < H; ++y)
    {
        const uint8_t* row = (const uint8_t*)ms.pData + y * ms.RowPitch;
        for (UINT x = 0; x < W; ++x)
        {
            const uint8_t* px = row + x * 4;
            if (abs(px[0] - bgR) > 16 || abs(px[1] - bgG) > 16 || abs(px[2] - bgB) > 16)
                ++differing;
        }
    }
    ctx->Unmap(staging.Get(), 0);
    // A unit cube filling a chunk of a 256x256 view should color thousands of pixels.
    EXPECT_GT(differing, 500);
}

TEST(WorldMeshCache_ReservedPrimitivesUseDistinctProceduralTopology)
{
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL featureLevel{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                   &dev, &featureLevel, &ctx);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev,
                               &featureLevel, &ctx);
    EXPECT_TRUE(SUCCEEDED(hr));
    if (FAILED(hr))
        return;

    GraphicsEngine graphics;
    EXPECT_TRUE(SUCCEEDED(graphics.InitializeFromDevice(dev.Get(), ctx.Get())));
    Spark::WorldMeshCache cache;

    Mesh* cube = cache.GetOrLoad(graphics, "__spark_primitive_Cube.obj");
    Mesh* sphere = cache.GetOrLoad(graphics, "__spark_primitive_Sphere.obj");
    Mesh* cylinder = cache.GetOrLoad(graphics, "__spark_primitive_Cylinder.obj");
    Mesh* plane = cache.GetOrLoad(graphics, "__spark_primitive_Plane.obj");

    EXPECT_TRUE(cube != nullptr);
    EXPECT_TRUE(sphere != nullptr);
    EXPECT_TRUE(cylinder != nullptr);
    EXPECT_TRUE(plane != nullptr);
    if (!cube || !sphere || !cylinder || !plane)
        return;

    EXPECT_GT(cube->GetIndexCount(), 0u);
    EXPECT_GT(sphere->GetIndexCount(), cube->GetIndexCount());
    EXPECT_GT(cylinder->GetIndexCount(), cube->GetIndexCount());
    EXPECT_TRUE(plane->GetIndexCount() != cube->GetIndexCount());
    EXPECT_TRUE(sphere->GetIndexCount() != cylinder->GetIndexCount());
}
