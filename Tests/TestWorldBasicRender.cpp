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
#include "Graphics/Shader.h"
#include "Graphics/WorldBasicRenderer.h"
#include "Game/PlaceholderMesh.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <objbase.h>
#include <thread>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace
{
    std::string RenderTestPathUtf8(const std::filesystem::path& path)
    {
        const std::u8string utf8 = path.generic_u8string();
        return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
    }

    bool WriteTwoBandBmp(const std::filesystem::path& path)
    {
        std::array<uint8_t, 70> bmp{};
        bmp[0] = 'B';
        bmp[1] = 'M';
        bmp[2] = 70;
        bmp[10] = 54;
        bmp[14] = 40;
        bmp[18] = 2;
        bmp[22] = 2;
        bmp[26] = 1;
        bmp[28] = 32;
        bmp[34] = 16;
        // Positive-height BMP rows are bottom-up: green bottom, red top.
        const uint8_t pixels[16] = {0, 255, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 0, 0, 255, 255};
        std::memcpy(bmp.data() + 54, pixels, sizeof(pixels));
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bmp.data()), static_cast<std::streamsize>(bmp.size()));
        return output.good();
    }
} // namespace

TEST(PlaceholderMesh_LoaderPathPreservesPlatformNativeUnicode)
{
    const std::filesystem::path native =
        std::filesystem::temp_directory_path() / std::filesystem::u8path("Spark-Caf\xC3\xA9.obj");
#ifdef SPARK_PLATFORM_WINDOWS
    const std::wstring loaderPath = native.native();
#else
    const std::string bytes = native.string();
    const std::wstring loaderPath(bytes.begin(), bytes.end());
#endif
    EXPECT_TRUE(MeshFilesystemPathFromLoaderPath(loaderPath) == native);
}

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
    if (FAILED(hr))
    {
        std::cout << "[ INFO   ] D3D11 render smoke skipped: no hardware or WARP device is available.\n";
        return;
    }
    EXPECT_TRUE(SUCCEEDED(hr));

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
    // Reserved procedural geometry is the only mesh category that intentionally
    // requires no project root or asset-file dependency.
    mr.meshPath = "__spark_primitive_Cube.obj";

    XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(2, 2, -3, 1), XMVectorSet(0, 0, 0, 1), XMVectorSet(0, 1, 0, 0));
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(60.f), (float)W / H, 0.1f, 100.f);
    Spark::WorldMeshCache cache;
    Spark::RenderWorldBasic(w, g, cache, view, proj, {});
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
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev,
                                   &featureLevel, &ctx);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev,
                               &featureLevel, &ctx);
    if (FAILED(hr))
    {
        std::cout << "[ INFO   ] D3D11 primitive smoke skipped: no hardware or WARP device is available.\n";
        return;
    }
    EXPECT_TRUE(SUCCEEDED(hr));

    GraphicsEngine graphics;
    EXPECT_TRUE(SUCCEEDED(graphics.InitializeFromDevice(dev.Get(), ctx.Get())));
    Spark::WorldMeshCache cache;

    Mesh* cube = cache.GetOrLoad(graphics, "__spark_primitive_Cube.obj", {});
    Mesh* sphere = cache.GetOrLoad(graphics, "__spark_primitive_Sphere.obj", {});
    Mesh* cylinder = cache.GetOrLoad(graphics, "__spark_primitive_Cylinder.obj", {});
    Mesh* plane = cache.GetOrLoad(graphics, "__spark_primitive_Plane.obj", {});

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

TEST(WorldMeshCache_MissingPlaceholderReloadsWhenImportedAssetAppears)
{
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL featureLevel{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev,
                                   &featureLevel, &ctx);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev,
                               &featureLevel, &ctx);
    if (FAILED(hr))
        return;

    GraphicsEngine graphics;
    EXPECT_TRUE(SUCCEEDED(graphics.InitializeFromDevice(dev.Get(), ctx.Get())));
    Spark::WorldMeshCache cache;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "spark_mesh_cache_missing_then_imported";
    const std::filesystem::path meshPath = root / "Assets" / "Meshes" / "arriving.obj";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(meshPath.parent_path(), ec);

    Mesh* placeholder = cache.GetOrLoad(graphics, "Assets/Meshes/arriving.obj", root.string());
    EXPECT_TRUE(placeholder != nullptr);
    EXPECT_TRUE(placeholder && placeholder->IsPlaceholder());

    {
        std::ofstream obj(meshPath, std::ios::binary);
        obj << "v -0.5 0 0\nv 0.5 0 0\nv 0 1 0\n"
               "vn 0 0 -1\n"
               "f 1//1 2//1 3//1\n";
    }
    Mesh* imported = cache.GetOrLoad(graphics, "Assets/Meshes/arriving.obj", root.string());
    EXPECT_TRUE(imported != nullptr);
    EXPECT_FALSE(imported && imported->IsPlaceholder());

    std::filesystem::remove_all(root, ec);
}

TEST(MeshObjLoader_PreservesMtlColorRangesForBasicRenderer)
{
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL featureLevel{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                   &dev, &featureLevel, &ctx);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev,
                               &featureLevel, &ctx);
    if (FAILED(hr))
        return;

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "spark_obj_mtl_color_ranges";
    const std::filesystem::path objPath = root / "two_colors.obj";
    const std::filesystem::path mtlPath = root / "two_colors.mtl";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    {
        std::ofstream mtl(mtlPath, std::ios::binary | std::ios::trunc);
        mtl << "newmtl Red\nKd 0.8 0.1 0.2\n\nnewmtl Blue\nKd 0.1 0.2 0.9\n";
        std::ofstream obj(objPath, std::ios::binary | std::ios::trunc);
        obj << "mtllib two_colors.mtl\n"
               "v -1 0 0\nv 0 0 0\nv -1 1 0\nv 1 0 0\nv 1 1 0\n"
               "vn 0 0 -1\n"
               "usemtl Red\nf 1//1 2//1 3//1\n"
               "usemtl Blue\nf 2//1 4//1 5//1\n";
    }

    Mesh mesh;
    EXPECT_TRUE(SUCCEEDED(mesh.Initialize(dev.Get(), ctx.Get())));
    EXPECT_TRUE(mesh.LoadFromFile(objPath.native()));
    const auto& submeshes = mesh.GetSubmeshes();
    EXPECT_EQ(submeshes.size(), 2u);
    if (submeshes.size() == 2)
    {
        EXPECT_EQ(submeshes[0].indexStart, 0u);
        EXPECT_EQ(submeshes[0].indexCount, 3u);
        EXPECT_EQ(submeshes[1].indexStart, 3u);
        EXPECT_EQ(submeshes[1].indexCount, 3u);
        EXPECT_GT(submeshes[0].diffuseColor.x, submeshes[0].diffuseColor.z);
        EXPECT_GT(submeshes[1].diffuseColor.z, submeshes[1].diffuseColor.x);
    }
    std::filesystem::remove_all(root, ec);
}

TEST(WorldBasicRender_SpritesResolveUVPivotSortAndRestoreOutputMergerState)
{
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL featureLevel{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev,
                                   &featureLevel, &ctx);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev,
                               &featureLevel, &ctx);
    if (FAILED(hr))
    {
        std::cout << "[ INFO   ] D3D11 sprite semantics skipped: no hardware or WARP device is available.\n";
        return;
    }

    constexpr UINT width = 256;
    constexpr UINT height = 256;
    D3D11_TEXTURE2D_DESC targetDesc{};
    targetDesc.Width = width;
    targetDesc.Height = height;
    targetDesc.MipLevels = 1;
    targetDesc.ArraySize = 1;
    targetDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    targetDesc.SampleDesc.Count = 1;
    targetDesc.Usage = D3D11_USAGE_DEFAULT;
    targetDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target;
    EXPECT_TRUE(SUCCEEDED(dev->CreateTexture2D(&targetDesc, nullptr, &target)));
    if (!target)
        return;
    ComPtr<ID3D11RenderTargetView> targetView;
    EXPECT_TRUE(SUCCEEDED(dev->CreateRenderTargetView(target.Get(), nullptr, &targetView)));
    if (!targetView)
        return;

    D3D11_TEXTURE2D_DESC depthDesc = targetDesc;
    depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth;
    EXPECT_TRUE(SUCCEEDED(dev->CreateTexture2D(&depthDesc, nullptr, &depth)));
    ComPtr<ID3D11DepthStencilView> depthView;
    if (depth)
        EXPECT_TRUE(SUCCEEDED(dev->CreateDepthStencilView(depth.Get(), nullptr, &depthView)));
    if (!depthView)
        return;

    GraphicsEngine graphics;
    const HRESULT initializeResult = graphics.InitializeFromDevice(dev.Get(), ctx.Get());
    EXPECT_TRUE(SUCCEEDED(initializeResult));
    if (FAILED(initializeResult))
        return;

    ID3D11RenderTargetView* targetViews[] = {targetView.Get()};
    ctx->OMSetRenderTargets(1, targetViews, depthView.Get());
    const float background[4] = {0.04f, 0.05f, 0.06f, 1.0f};
    ctx->ClearRenderTargetView(targetView.Get(), background);
    if (depthView)
        ctx->ClearDepthStencilView(depthView.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &viewport);

    World world;
    const EntityID firstTie = world.CreateEntity("First tie");
    world.AddComponent<Transform>(firstTie);
    SpriteRenderer& firstTieSprite = world.AddComponent<SpriteRenderer>(firstTie);
    firstTieSprite.color = {1.0f, 0.0f, 0.0f, 0.5f};
    firstTieSprite.sortingLayer = 10;
    firstTieSprite.orderInLayer = 0;

    const EntityID expectedLast = world.CreateEntity("Expected last");
    world.AddComponent<Transform>(expectedLast);
    SpriteRenderer& expectedLastSprite = world.AddComponent<SpriteRenderer>(expectedLast);
    expectedLastSprite.color = {0.0f, 1.0f, 0.0f, 0.5f};
    expectedLastSprite.sourceRect = {-0.25f, 0.25f, 0.75f, 1.25f};
    expectedLastSprite.pivot = {0.25f, 0.75f};
    expectedLastSprite.textureWidth = 200;
    expectedLastSprite.textureHeight = 100;
    expectedLastSprite.pixelsPerUnit = 100.0f;
    expectedLastSprite.flipX = true;
    expectedLastSprite.flipY = true;
    expectedLastSprite.sortingLayer = 10;
    expectedLastSprite.orderInLayer = 0;

    const EntityID lowerLayer = world.CreateEntity("Lower layer despite high order");
    world.AddComponent<Transform>(lowerLayer);
    SpriteRenderer& lowerLayerSprite = world.AddComponent<SpriteRenderer>(lowerLayer);
    lowerLayerSprite.color = {0.0f, 0.0f, 1.0f, 0.5f};
    lowerLayerSprite.sortingLayer = -10;
    lowerLayerSprite.orderInLayer = 1000;

    const EntityID lowerOrder = world.CreateEntity("Lower order despite later entity");
    world.AddComponent<Transform>(lowerOrder);
    SpriteRenderer& lowerOrderSprite = world.AddComponent<SpriteRenderer>(lowerOrder);
    lowerOrderSprite.color = {1.0f, 1.0f, 0.0f, 0.5f};
    lowerOrderSprite.sortingLayer = 10;
    lowerOrderSprite.orderInLayer = -100;

    const EntityID invalidRect = world.CreateEntity("Invalid source rectangle");
    world.AddComponent<Transform>(invalidRect);
    SpriteRenderer& invalidSprite = world.AddComponent<SpriteRenderer>(invalidRect);
    invalidSprite.color = {1.0f, 0.0f, 1.0f, 1.0f};
    invalidSprite.sourceRect = {0.8f, 0.2f, 0.2f, 0.9f};
    invalidSprite.sortingLayer = 100;

    const EntityID inactive = world.CreateEntity("Inactive sprite");
    world.AddComponent<Transform>(inactive);
    SpriteRenderer& inactiveSprite = world.AddComponent<SpriteRenderer>(inactive);
    inactiveSprite.color = {1.0f, 0.0f, 1.0f, 1.0f};
    inactiveSprite.sortingLayer = 1000;
    ActiveComponent& inactiveState = world.AddComponent<ActiveComponent>(inactive);
    inactiveState.active = false;

    const XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(0.0f, 0.0f, -3.0f, 1.0f), XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f),
                                           XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
    const XMMATRIX projection = XMMatrixOrthographicLH(4.0f, 4.0f, 0.1f, 10.0f);

    // RenderWorldBasic owns the baseline basic state, but its transparent
    // sprite sub-pass must restore that exact baseline rather than replacing
    // it with a merely equivalent lazily-created state object.
    graphics.ApplyBasicRenderStates();
    ComPtr<ID3D11BlendState> blendBefore;
    float blendFactorBefore[4]{};
    UINT sampleMaskBefore = 0;
    ctx->OMGetBlendState(blendBefore.GetAddressOf(), blendFactorBefore, &sampleMaskBefore);
    ComPtr<ID3D11DepthStencilState> depthBefore;
    UINT stencilBefore = 0;
    ctx->OMGetDepthStencilState(depthBefore.GetAddressOf(), &stencilBefore);

    Spark::WorldMeshCache cache;
    const Spark::WorldBasicRenderStats stats = Spark::RenderWorldBasic(world, graphics, cache, view, projection, {});
    ctx->Flush();

    EXPECT_EQ(stats.candidates, 6u);
    EXPECT_EQ(stats.visible, 5u);
    EXPECT_EQ(stats.drawn, 4u);
    EXPECT_EQ(stats.rejected, 1u);

    ComPtr<ID3D11BlendState> blendAfter;
    float blendFactorAfter[4]{};
    UINT sampleMaskAfter = 0;
    ctx->OMGetBlendState(blendAfter.GetAddressOf(), blendFactorAfter, &sampleMaskAfter);
    ComPtr<ID3D11DepthStencilState> depthAfter;
    UINT stencilAfter = 0;
    ctx->OMGetDepthStencilState(depthAfter.GetAddressOf(), &stencilAfter);
    EXPECT_TRUE(blendBefore.Get() == blendAfter.Get());
    EXPECT_TRUE(depthBefore.Get() == depthAfter.Get());
    EXPECT_EQ(sampleMaskBefore, sampleMaskAfter);
    EXPECT_EQ(stencilBefore, stencilAfter);
    for (int i = 0; i < 4; ++i)
        EXPECT_NEAR(blendFactorBefore[i], blendFactorAfter[i], 0.00001f);

    // The per-object buffer contains the final submitted sprite. Its color
    // proves (layer, order, entity) sorting, while its UVs and world matrix
    // prove clamping, UV-based flips, source sizing, and pivot placement.
    ComPtr<ID3D11Buffer> objectBuffer;
    ctx->VSGetConstantBuffers(0, 1, objectBuffer.GetAddressOf());
    EXPECT_TRUE(objectBuffer.Get() != nullptr);
    if (!objectBuffer)
        return;
    D3D11_BUFFER_DESC objectBufferDesc{};
    objectBuffer->GetDesc(&objectBufferDesc);
    D3D11_BUFFER_DESC readbackDesc = objectBufferDesc;
    readbackDesc.Usage = D3D11_USAGE_STAGING;
    readbackDesc.BindFlags = 0;
    readbackDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    readbackDesc.MiscFlags = 0;
    ComPtr<ID3D11Buffer> objectReadback;
    EXPECT_TRUE(SUCCEEDED(dev->CreateBuffer(&readbackDesc, nullptr, &objectReadback)));
    if (!objectReadback)
        return;
    ctx->CopyResource(objectReadback.Get(), objectBuffer.Get());
    D3D11_MAPPED_SUBRESOURCE mappedConstants{};
    EXPECT_TRUE(SUCCEEDED(ctx->Map(objectReadback.Get(), 0, D3D11_MAP_READ, 0, &mappedConstants)));
    if (!mappedConstants.pData)
        return;
    PerObjectConstants constants{};
    std::memcpy(&constants, mappedConstants.pData, sizeof(constants));
    ctx->Unmap(objectReadback.Get(), 0);

    EXPECT_NEAR(constants.ObjectColor.x, 0.0f, 0.0001f);
    EXPECT_NEAR(constants.ObjectColor.y, 1.0f, 0.0001f);
    EXPECT_NEAR(constants.ObjectColor.z, 0.0f, 0.0001f);
    EXPECT_NEAR(constants.UVTiling.x, -0.75f, 0.0001f);
    EXPECT_NEAR(constants.UVTiling.y, 0.75f, 0.0001f);
    EXPECT_NEAR(constants.UVTiling.z, 0.75f, 0.0001f);
    EXPECT_NEAR(constants.UVTiling.w, 0.25f, 0.0001f);

    const XMMATRIX expectedWorld =
        XMMatrixScaling(1.5f, 1.0f, 0.75f) * XMMatrixRotationX(XM_PIDIV2) * XMMatrixTranslation(0.375f, 0.1875f, 0.0f);
    XMFLOAT4X4 actualWorld{};
    XMFLOAT4X4 expectedWorldValues{};
    XMStoreFloat4x4(&actualWorld, XMMatrixTranspose(constants.WorldMatrix));
    XMStoreFloat4x4(&expectedWorldValues, expectedWorld);
    const float* actualElements = &actualWorld._11;
    const float* expectedElements = &expectedWorldValues._11;
    for (int i = 0; i < 16; ++i)
        EXPECT_NEAR(actualElements[i], expectedElements[i], 0.0001f);

    // Alpha blending plus read-only LESS_EQUAL depth lets all four coplanar
    // sprites contribute. At least some overlap pixels must therefore contain
    // both red and green; opaque blending or depth writes would leave only one.
    D3D11_TEXTURE2D_DESC stagingDesc = targetDesc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    EXPECT_TRUE(SUCCEEDED(dev->CreateTexture2D(&stagingDesc, nullptr, &staging)));
    if (!staging)
        return;
    ctx->CopyResource(staging.Get(), target.Get());
    D3D11_MAPPED_SUBRESOURCE mappedTarget{};
    EXPECT_TRUE(SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mappedTarget)));
    if (!mappedTarget.pData)
        return;
    const uint8_t backgroundR = static_cast<uint8_t>(background[0] * 255.0f);
    const uint8_t backgroundG = static_cast<uint8_t>(background[1] * 255.0f);
    int mixedPixels = 0;
    for (UINT y = 0; y < height; ++y)
    {
        const uint8_t* row = static_cast<const uint8_t*>(mappedTarget.pData) + y * mappedTarget.RowPitch;
        for (UINT x = 0; x < width; ++x)
        {
            const uint8_t* pixel = row + x * 4;
            if (pixel[0] > backgroundR + 8 && pixel[1] > backgroundG + 8)
                ++mixedPixels;
        }
    }
    ctx->Unmap(staging.Get(), 0);
    EXPECT_GT(mixedPixels, 50);
}

TEST(WorldBasicRender_SpriteTextureTopIsNotVerticallyMirrored)
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel{};
    HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                       &device, &featureLevel, &context);
    if (FAILED(result))
        result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                   &featureLevel, &context);
    if (FAILED(result))
    {
        std::cout << "[ INFO   ] D3D11 sprite orientation skipped: no hardware or WARP device is available.\n";
        return;
    }

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitializeCom = comResult == S_OK || comResult == S_FALSE;

    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path projectRoot =
        std::filesystem::temp_directory_path() / ("spark-sprite-orientation-" + std::to_string(stamp));
    const std::filesystem::path texturePath = projectRoot / "Assets" / "Textures" / "orientation.bmp";
    std::filesystem::create_directories(texturePath.parent_path());
    EXPECT_TRUE(WriteTwoBandBmp(texturePath));

    constexpr UINT width = 64;
    constexpr UINT height = 64;
    D3D11_TEXTURE2D_DESC targetDesc{};
    targetDesc.Width = width;
    targetDesc.Height = height;
    targetDesc.MipLevels = 1;
    targetDesc.ArraySize = 1;
    targetDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    targetDesc.SampleDesc.Count = 1;
    targetDesc.Usage = D3D11_USAGE_DEFAULT;
    targetDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target;
    EXPECT_TRUE(SUCCEEDED(device->CreateTexture2D(&targetDesc, nullptr, &target)));
    ComPtr<ID3D11RenderTargetView> targetView;
    if (target)
        EXPECT_TRUE(SUCCEEDED(device->CreateRenderTargetView(target.Get(), nullptr, &targetView)));

    D3D11_TEXTURE2D_DESC depthDesc = targetDesc;
    depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth;
    EXPECT_TRUE(SUCCEEDED(device->CreateTexture2D(&depthDesc, nullptr, &depth)));
    ComPtr<ID3D11DepthStencilView> depthView;
    if (depth)
        EXPECT_TRUE(SUCCEEDED(device->CreateDepthStencilView(depth.Get(), nullptr, &depthView)));

    if (targetView && depthView)
    {
        GraphicsEngine graphics;
        EXPECT_TRUE(SUCCEEDED(graphics.InitializeFromDevice(device.Get(), context.Get())));
        ID3D11RenderTargetView* views[] = {targetView.Get()};
        context->OMSetRenderTargets(1, views, depthView.Get());
        const float clear[4]{};
        context->ClearRenderTargetView(targetView.Get(), clear);
        context->ClearDepthStencilView(depthView.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(width);
        viewport.Height = static_cast<float>(height);
        viewport.MaxDepth = 1.0f;
        context->RSSetViewports(1, &viewport);

        World world;
        const EntityID entity = world.CreateEntity("Orientation sprite");
        world.AddComponent<Transform>(entity);
        SpriteRenderer& sprite = world.AddComponent<SpriteRenderer>(entity);
        sprite.texturePath = "Assets/Textures/orientation.bmp";
        sprite.textureWidth = 2;
        sprite.textureHeight = 2;
        sprite.pixelsPerUnit = 2.0f;

        Spark::WorldMeshCache cache;
        const XMMATRIX view =
            XMMatrixLookAtLH(XMVectorSet(0.0f, 0.0f, -3.0f, 1.0f), XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f),
                             XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
        const XMMATRIX projection = XMMatrixOrthographicLH(2.0f, 2.0f, 0.1f, 10.0f);
        const auto stats =
            Spark::RenderWorldBasic(world, graphics, cache, view, projection, RenderTestPathUtf8(projectRoot));
        EXPECT_EQ(stats.drawn, 1u);

        D3D11_TEXTURE2D_DESC stagingDesc = targetDesc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        EXPECT_TRUE(SUCCEEDED(device->CreateTexture2D(&stagingDesc, nullptr, &staging)));
        if (staging)
        {
            context->CopyResource(staging.Get(), target.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            EXPECT_TRUE(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
            if (mapped.pData)
            {
                const auto sample = [&mapped](UINT x, UINT y)
                { return static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch + x * 4; };
                const uint8_t* top = sample(width / 2, height / 2 - 8);
                const uint8_t* bottom = sample(width / 2, height / 2 + 8);
                EXPECT_GT(top[0], top[1]);
                EXPECT_GT(bottom[1], bottom[0]);
                context->Unmap(staging.Get(), 0);
            }
        }
    }

    std::error_code cleanupError;
    std::filesystem::remove_all(projectRoot, cleanupError);
    if (uninitializeCom)
        CoUninitialize();
}

TEST(BasicTextureCache_RetriesTransientComFailureAndInvalidatesDeterministicMissingFile)
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel{};
    HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                       &device, &featureLevel, &context);
    if (FAILED(result))
        result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                   &featureLevel, &context);
    if (FAILED(result))
    {
        std::cout << "[ INFO   ] D3D11 texture-cache semantics skipped: no hardware or WARP device is available.\n";
        return;
    }

    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path projectRoot =
        std::filesystem::temp_directory_path() /
        std::filesystem::path(std::wstring(L"spark-texture-cache-\u8def\u5f84-") + std::to_wstring(stamp));
    const std::filesystem::path texturePath = projectRoot / L"Assets" / L"Textures" / L"cache.bmp";
    std::filesystem::create_directories(texturePath.parent_path());
    const std::string texturePathUtf8 = RenderTestPathUtf8(texturePath);

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitializeCom = comResult == S_OK || comResult == S_FALSE;
    if (SUCCEEDED(comResult) || comResult == RPC_E_CHANGED_MODE)
    {
        GraphicsEngine graphics;
        EXPECT_TRUE(SUCCEEDED(graphics.InitializeFromDevice(device.Get(), context.Get())));

        // Missing files are deterministic and remain negative until the asset
        // importer explicitly invalidates their canonical identity.
        EXPECT_TRUE(graphics.GetOrLoadTextureSRV(texturePathUtf8) == nullptr);
        EXPECT_TRUE(WriteTwoBandBmp(texturePath));
        EXPECT_TRUE(graphics.GetOrLoadTextureSRV(texturePathUtf8) == nullptr);
        EXPECT_TRUE(graphics.InvalidateBasicTexture(texturePathUtf8));
        EXPECT_TRUE(graphics.GetOrLoadTextureSRV(texturePathUtf8) != nullptr);
    }
    else
    {
        // Keep the worker-thread transient-failure check meaningful even if
        // this runner cannot establish a COM apartment on the main thread.
        EXPECT_TRUE(WriteTwoBandBmp(texturePath));
    }

    if (uninitializeCom)
        CoUninitialize();

    // A fresh std::thread has no COM apartment. The first factory creation
    // therefore fails for thread state, not for the image. Initializing COM on
    // that same thread must make the very next attempt succeed without an
    // explicit cache invalidation.
    GraphicsEngine retryGraphics;
    EXPECT_TRUE(SUCCEEDED(retryGraphics.InitializeFromDevice(device.Get(), context.Get())));
    bool failedBeforeComInitialization = false;
    bool loadedAfterComInitialization = false;
    HRESULT workerComResult = E_FAIL;
    std::thread worker(
        [&]
        {
            failedBeforeComInitialization = retryGraphics.GetOrLoadTextureSRV(texturePathUtf8) == nullptr;
            workerComResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (SUCCEEDED(workerComResult) || workerComResult == RPC_E_CHANGED_MODE)
                loadedAfterComInitialization = retryGraphics.GetOrLoadTextureSRV(texturePathUtf8) != nullptr;
            if (workerComResult == S_OK || workerComResult == S_FALSE)
                CoUninitialize();
        });
    worker.join();
    if (!failedBeforeComInitialization)
    {
        // Some compatibility layers implicitly establish a COM apartment.
        // They cannot exercise the pre-initialization half of this regression.
        std::cout << "[ INFO   ] COM transient texture-cache precondition skipped: WIC was already available.\n";
    }
    EXPECT_TRUE(SUCCEEDED(workerComResult) || workerComResult == RPC_E_CHANGED_MODE);
    EXPECT_TRUE(loadedAfterComInitialization);

    std::error_code cleanupError;
    std::filesystem::remove_all(projectRoot, cleanupError);
}
