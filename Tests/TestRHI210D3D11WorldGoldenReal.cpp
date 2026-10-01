/**
 * @file TestRHI210D3D11WorldGoldenReal.cpp
 * @brief RHI-210: the ECS world renderer's opaque and transparent passes on the
 *        d3d11-warp golden row.
 *
 * Spark::RenderWorldBasic (WorldBasicRenderer.cpp) is the renderer the runtime's
 * -scene mode and both editor viewports use for an ECS World: an opaque pass over
 * Transform + MeshRenderer entities, then a transparent pass over Transform +
 * SpriteRenderer entities (alpha blending, read-only LESS_EQUAL depth, sorted by
 * layer, order and entity). The scene below drives it through a GraphicsEngine
 * attached to a WARP device (InitializeFromDevice, the editor's attach path) into
 * a 320x240 offscreen target, reads the target back and compares it with the
 * committed baseline (reviewed thresholds and SHA-256 in
 * Tests/GoldenImages/manifest.json, fail-closed).
 *
 * Scene World_OpaqueAndSprites: a reserved-primitive floor, cube and sphere, and
 * three tinted half-transparent sprites: two overlapping each other in front of
 * the floor and one partly behind the cube.
 *
 * Every interior pixel of the floor and cube, every sprite pixel over them, and
 * the background are checked against a CPU ray cast that shades with the basic
 * pixel shader formula and blends the sprites in draw order with 8-bit
 * intermediate targets, so a broken pass cannot become a baseline.
 */

#include "TestFramework.h"

#ifdef _WIN32

#include "RHI210D3D11GoldenSupport.h"

#include "Engine/ECS/Components.h"
#include "Engine/ECS/Components/CoreComponents.h"
#include "Graphics/GraphicsEngine.h"
#include "Graphics/WorldBasicRenderer.h"

#include <DirectXMath.h>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <deque>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using RHI210Golden::Color3;
using RHI210Golden::Instance;
using RHI210Golden::SurfaceHit;
using RHI210Golden::TriangleMesh;

namespace
{
    constexpr const char* kRow = "d3d11-warp";

    /// Every scene this file certifies on kRow. Other d3d11-warp lanes certify
    /// other scenes; Tests/Tools/test_golden_manifest.py checks the union.
    const std::array<const char*, 1> kScenes = {"World_OpaqueAndSprites"};

    constexpr uint32_t kWidth = 320;
    constexpr uint32_t kHeight = 240;
    /// Exact 8-bit clear colour (24, 30, 38), so no rounding tie is involved.
    constexpr std::array<float, 4> kClear = {24.0f / 255.0f, 30.0f / 255.0f, 38.0f / 255.0f, 1.0f};
    constexpr Color3 kWhite = {1.0f, 1.0f, 1.0f};

    struct OffscreenTarget
    {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        ComPtr<ID3D11Texture2D> color;
        ComPtr<ID3D11RenderTargetView> colorView;
        ComPtr<ID3D11Texture2D> depth;
        ComPtr<ID3D11DepthStencilView> depthView;
    };

    bool CreateTarget(OffscreenTarget& target)
    {
        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG, &level, 1,
                                     D3D11_SDK_VERSION, &target.device, nullptr, &target.context)))
        {
            return false;
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(target.device->CreateTexture2D(&desc, nullptr, &target.color)) ||
            FAILED(target.device->CreateRenderTargetView(target.color.Get(), nullptr, &target.colorView)))
        {
            return false;
        }
        desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        return SUCCEEDED(target.device->CreateTexture2D(&desc, nullptr, &target.depth)) &&
               SUCCEEDED(target.device->CreateDepthStencilView(target.depth.Get(), nullptr, &target.depthView));
    }

    std::vector<uint8_t> ReadTarget(OffscreenTarget& target)
    {
        D3D11_TEXTURE2D_DESC desc{};
        target.color->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        std::vector<uint8_t> pixels;
        if (FAILED(target.device->CreateTexture2D(&desc, nullptr, &staging)))
        {
            return pixels;
        }
        target.context->CopyResource(staging.Get(), target.color.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(target.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        {
            return pixels;
        }
        pixels.resize(size_t(kWidth) * kHeight * 4);
        for (uint32_t y = 0; y < kHeight; ++y)
        {
            const auto* row = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
            std::copy_n(row, size_t(kWidth) * 4, pixels.begin() + static_cast<std::ptrdiff_t>(y * kWidth * 4));
        }
        target.context->Unmap(staging.Get(), 0);
        return pixels;
    }

    struct SpriteSpec
    {
        const char* name;
        XMFLOAT3 position;
        XMFLOAT4 color;
        int orderInLayer;
    };

    /// Sprites in draw order (orderInLayer ascending within layer 0).
    const std::array<SpriteSpec, 3> kSprites = {{
        {"Sprite_Back", {0.75f, 0.75f, 1.35f}, {0.95f, 0.85f, 0.2f, 0.6f}, 0},
        {"Sprite_Red", {-0.55f, 0.8f, -0.55f}, {1.0f, 0.35f, 0.2f, 0.55f}, 1},
        {"Sprite_Blue", {-0.1f, 1.0f, -0.85f}, {0.2f, 0.6f, 1.0f, 0.5f}, 2},
    }};

    /// WorldBasicRenderer's sprite quad: a CreatePlane(1, 1) quad scaled to the sprite
    /// size, rotated +90 degrees about X so it stands up, at the entity transform.
    XMMATRIX SpriteWorld(const XMMATRIX& entityWorld, float sizeX, float sizeY)
    {
        return XMMatrixScaling(sizeX, 1.0f, sizeY) * XMMatrixRotationX(XM_PIDIV2) * entityWorld;
    }

    std::array<float, 3> Quantize(const Color3& color)
    {
        return {RHI210Golden::ToUnorm8(color[0]) / 255.0f, RHI210Golden::ToUnorm8(color[1]) / 255.0f,
                RHI210Golden::ToUnorm8(color[2]) / 255.0f};
    }
} // namespace

// ----------------------------------------------------------------------------
// Every scene this lane certifies has a reviewed software-row entry and PNG.
// ----------------------------------------------------------------------------
TEST(D3D11WorldGolden_ManifestHasEveryScene)
{
    std::vector<Spark::GoldenManifestEntry> entries;
    std::string error;
    ASSERT_TRUE(Spark::GoldenManifest::Load(RHI210Golden::GoldenDir() / "manifest.json", entries, error));
    for (const char* scene : kScenes)
    {
        const auto entry = std::find_if(entries.begin(), entries.end(), [&](const Spark::GoldenManifestEntry& e)
                                        { return e.backendRow == kRow && e.scene == scene; });
        if (entry == entries.end())
        {
            std::printf("[RHI-210 GOLDEN] scene without a manifest entry: %s\n", scene);
        }
        ASSERT_TRUE(entry != entries.end());
        EXPECT_TRUE(entry->software);
        EXPECT_TRUE(std::filesystem::is_regular_file(RHI210Golden::GoldenDir() / kRow / (std::string(scene) + ".png")));
    }
}

// ----------------------------------------------------------------------------
// Opaque MeshRenderer pass + transparent SpriteRenderer pass.
// ----------------------------------------------------------------------------
TEST(D3D11WorldGolden_OpaqueAndSprites)
{
    OffscreenTarget target;
    ASSERT_TRUE(CreateTarget(target));
    GraphicsEngine graphics;
    ASSERT_TRUE(SUCCEEDED(graphics.InitializeFromDevice(target.device.Get(), target.context.Get())));

    World world;
    auto addMesh = [&world](const char* name, const char* mesh, const XMFLOAT3& position, const XMFLOAT3& rotation,
                            const XMFLOAT3& scale)
    {
        const EntityID entity = world.CreateEntity(name);
        Transform& transform = world.AddComponent<Transform>(entity);
        transform.position = position;
        transform.rotation = rotation;
        transform.scale = scale;
        world.AddComponent<MeshRenderer>(entity).meshPath = mesh;
        return entity;
    };
    const EntityID floor =
        addMesh("Floor", "__spark_primitive_Plane.obj", {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {6.0f, 1.0f, 6.0f});
    const EntityID cube =
        addMesh("Cube", "__spark_primitive_Cube.obj", {0.6f, 0.5f, 0.8f}, {0.0f, 20.0f, 0.0f}, {1.0f, 1.0f, 1.0f});
    const EntityID sphere =
        addMesh("Sphere", "__spark_primitive_Sphere.obj", {-1.3f, 0.5f, 0.9f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f});

    std::vector<EntityID> sprites;
    for (const SpriteSpec& spec : kSprites)
    {
        const EntityID entity = world.CreateEntity(spec.name);
        world.AddComponent<Transform>(entity).position = spec.position;
        SpriteRenderer& sprite = world.AddComponent<SpriteRenderer>(entity);
        sprite.color = spec.color;
        sprite.textureWidth = 100;
        sprite.textureHeight = 60;
        sprite.pixelsPerUnit = 100.0f;
        sprite.orderInLayer = spec.orderInLayer;
        sprites.push_back(entity);
    }

    const XMFLOAT3 eye = {1.8f, 2.4f, -3.6f};
    const XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(eye.x, eye.y, eye.z, 1.0f), XMVectorSet(0.0f, 0.6f, 0.2f, 1.0f),
                                           XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
    const XMMATRIX projection =
        XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), float(kWidth) / float(kHeight), 0.1f, 100.0f);

    ID3D11RenderTargetView* views[] = {target.colorView.Get()};
    target.context->OMSetRenderTargets(1, views, target.depthView.Get());
    target.context->ClearRenderTargetView(target.colorView.Get(), kClear.data());
    target.context->ClearDepthStencilView(target.depthView.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    D3D11_VIEWPORT viewport{};
    viewport.Width = float(kWidth);
    viewport.Height = float(kHeight);
    viewport.MaxDepth = 1.0f;
    target.context->RSSetViewports(1, &viewport);

    Spark::WorldMeshCache cache;
    Spark::WorldBasicRenderStats stats{};
    if (!RHI210Golden::PassDisabled("world"))
    {
        stats = Spark::RenderWorldBasic(world, graphics, cache, view, projection, {});
    }
    EXPECT_EQ(stats.drawn, 6u);
    EXPECT_EQ(stats.rejected, 0u);
    const std::vector<uint8_t> frame = ReadTarget(target);
    ASSERT_EQ(frame.size(), size_t(kWidth) * kHeight * 4);
    EXPECT_TRUE(Spark::GoldenImageTestRunner::FrameHasRenderedContent(frame, 0.6));

    ComPtr<ID3D11InfoQueue> infoQueue;
    ASSERT_TRUE(SUCCEEDED(target.device.As(&infoQueue)));
    GraphicsEngine::ValidationCounts validationCounts;
    GraphicsEngine::AccumulateValidationMessages(infoQueue.Get(), validationCounts);
    EXPECT_EQ(infoQueue->GetNumMessagesDiscardedByMessageCountLimit(), 0u);
    EXPECT_EQ(validationCounts.corruption, 0u);
    EXPECT_EQ(validationCounts.errors, 0u);
    EXPECT_EQ(validationCounts.warnings, 0u);

    // CPU reference: the exact reserved primitive meshes, and the sprite quads.
    const entt::registry& registry = world.GetRegistry();
    std::deque<TriangleMesh> meshes;
    meshes.push_back(RHI210Golden::ProceduralPlane(1.0f, 1.0f));
    meshes.push_back(RHI210Golden::ProceduralCube(1.0f));
    meshes.push_back(RHI210Golden::ProceduralSphere(0.5f, 24, 16));
    std::vector<Instance> opaque;
    opaque.push_back(
        RHI210Golden::MakeInstance(meshes[0], world.GetComponent<Transform>(floor)->GetWorldMatrix(registry)));
    opaque.push_back(
        RHI210Golden::MakeInstance(meshes[1], world.GetComponent<Transform>(cube)->GetWorldMatrix(registry)));
    opaque.push_back(
        RHI210Golden::MakeInstance(meshes[2], world.GetComponent<Transform>(sphere)->GetWorldMatrix(registry)));
    std::vector<Instance> spriteShapes;
    for (const EntityID entity : sprites)
    {
        meshes.push_back(RHI210Golden::ProceduralPlane(1.0f, 1.0f));
        Instance instance = RHI210Golden::MakeInstance(
            meshes.back(), SpriteWorld(world.GetComponent<Transform>(entity)->GetWorldMatrix(registry), 1.0f, 0.6f));
        instance.twoSided = true;
        spriteShapes.push_back(instance);
    }

    // The layers a ray meets: the opaque hit, then each sprite in front of it.
    struct Layers
    {
        std::optional<SurfaceHit> opaque;
        std::array<std::optional<SurfaceHit>, kSprites.size()> sprites;
        bool operator==(const Layers& other) const
        {
            if (opaque.has_value() != other.opaque.has_value() ||
                (opaque && !RHI210Golden::SameSurface(*opaque, *other.opaque)))
            {
                return false;
            }
            for (size_t i = 0; i < sprites.size(); ++i)
            {
                if (sprites[i].has_value() != other.sprites[i].has_value())
                {
                    return false;
                }
                if (sprites[i] && !RHI210Golden::SameSurface(*sprites[i], *other.sprites[i]))
                {
                    return false;
                }
            }
            return true;
        }
    };
    auto layersAt = [&](float x, float y)
    {
        const RHI210Golden::Ray ray = RHI210Golden::ViewportRay(x, y, kWidth, kHeight, view, projection);
        Layers layers;
        layers.opaque = RHI210Golden::CastRay(ray, opaque);
        for (size_t i = 0; i < spriteShapes.size(); ++i)
        {
            const std::optional<SurfaceHit> hit = RHI210Golden::CastRay(ray, {spriteShapes[i]});
            if (hit && (!layers.opaque || hit->distance < layers.opaque->distance))
            {
                layers.sprites[i] = hit;
            }
        }
        return layers;
    };

    int failures = 0;
    int probes = 0;
    int spriteProbes = 0;
    int overlapProbes = 0;
    for (uint32_t py = 0; py < kHeight; ++py)
    {
        for (uint32_t px = 0; px < kWidth; ++px)
        {
            const float cx = float(px) + 0.5f;
            const float cy = float(py) + 0.5f;
            const Layers centre = layersAt(cx, cy);
            if (!(layersAt(cx - 0.75f, cy) == centre && layersAt(cx + 0.75f, cy) == centre &&
                  layersAt(cx, cy - 0.75f) == centre && layersAt(cx, cy + 0.75f) == centre))
            {
                continue;
            }
            if (centre.opaque && centre.opaque->instance == 2)
            {
                continue; // The faceted sphere is not modelled.
            }
            Color3 colour = {kClear[0], kClear[1], kClear[2]};
            if (centre.opaque)
            {
                colour =
                    Quantize(RHI210Golden::ShadeBasic(kWhite, centre.opaque->normal, centre.opaque->position, eye));
            }
            int covering = 0;
            for (size_t i = 0; i < kSprites.size(); ++i)
            {
                if (!centre.sprites[i])
                {
                    continue;
                }
                const XMFLOAT4& tint = kSprites[i].color;
                const Color3 lit = RHI210Golden::ShadeBasic({tint.x, tint.y, tint.z}, centre.sprites[i]->normal,
                                                            centre.sprites[i]->position, eye);
                for (int c = 0; c < 3; ++c)
                {
                    colour[c] = lit[c] * tint.w + colour[c] * (1.0f - tint.w);
                }
                colour = Quantize(colour);
                ++covering;
            }
            ++probes;
            spriteProbes += covering > 0 ? 1 : 0;
            overlapProbes += covering > 1 ? 1 : 0;
            const auto pixel = RHI210Golden::PixelAt(frame, kWidth, px, py);
            if (RHI210Golden::ChannelError(pixel, colour) > 2)
            {
                if (failures < 8)
                {
                    std::printf("[RHI-210 GOLDEN] World_OpaqueAndSprites probe (%u,%u) = (%d,%d,%d), expected "
                                "(%d,%d,%d), %d sprite(s)\n",
                                px, py, pixel[0], pixel[1], pixel[2], RHI210Golden::ToUnorm8(colour[0]),
                                RHI210Golden::ToUnorm8(colour[1]), RHI210Golden::ToUnorm8(colour[2]), covering);
                }
                ++failures;
            }
        }
    }
    std::printf("[RHI-210 GOLDEN] World_OpaqueAndSprites CPU probes: %d (%d under sprites, %d under two), %d outside "
                "tolerance\n",
                probes, spriteProbes, overlapProbes, failures);
    EXPECT_GT(probes, 40000);
    EXPECT_GT(spriteProbes, 2000);
    EXPECT_GT(overlapProbes, 200);
    EXPECT_EQ(failures, 0);
    EXPECT_TRUE(RHI210Golden::MatchesGolden(kRow, "World_OpaqueAndSprites", frame, kWidth, kHeight));
}

#endif // _WIN32
