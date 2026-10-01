/**
 * @file TestRHI210D3D11FrameGoldenReal.cpp
 * @brief RHI-210: the production D3D11 frame passes on the d3d11-warp golden row.
 *
 * Each scene drives a WARP GraphicsEngine in a hidden window through the
 * production frame (BeginFrame / RenderScene / EndFrame, the default Forward
 * pipeline) with a fixed 320x240 back buffer, reads the back buffer back before
 * Present and compares it with the committed baseline under
 * Tests/GoldenImages/d3d11-warp/ (reviewed thresholds and SHA-256 in
 * Tests/GoldenImages/manifest.json, fail-closed).
 *
 * Scenes:
 *   - Frame_ForwardLit: the frame clear and the forward opaque pass of GameObjects
 *     (a PlaneObject floor and three CubeObjects, two rotated, one non-uniformly
 *     scaled) with the default material: directional + ambient + view-fill
 *     lighting and depth testing (the nearest cube is drawn before the cubes it
 *     hides).
 *   - Frame_ForwardMaterial: the forward pass with a basic material JSON: a checker
 *     albedo, a tilted tangent-space normal map and roughness 0.25 (texture
 *     sampling, cotangent-frame normal mapping, specular).
 *   - Frame_DrawList: the ECS draw list (SubmitMeshForRendering -> ProcessDrawList)
 *     with AssetPipeline OBJ meshes (the shipped Assets/Models/Cube.obj and a quad)
 *     and the shipped checkerboard texture as the quad's material.
 *   - Frame_PostChain: Frame_ForwardLit's scene with GTAO, Bloom, ACES tonemapping
 *     and FXAA enabled on the engine's PostProcessingPipeline, so the chain runs
 *     inside the real frame (back buffer -> ping-pong targets -> back buffer, with
 *     the hardware depth buffer as the depth input).
 *
 * GameObject primitives load the staged models (Assets/Models/Cube.obj,
 * Plane.obj) relative to the working directory, as the packaged runtime does from
 * its install directory; CTest runs this lane in the runtime output directory.
 *
 * The forward-lit, material and draw-list scenes compare every interior pixel
 * with a CPU ray cast of the same triangles shaded with the basic pixel shader
 * formula (RHI210D3D11GoldenSupport.h), so a broken render cannot become a
 * baseline.
 *
 * The device is WARP (SPARK_D3D11_DRIVER=warp), so the frames belong to the
 * d3d11-warp row on any Windows host. WARP output can differ between Windows
 * builds; a mismatch on another build is data for owner review.
 */

#include "TestFramework.h"

#ifdef _WIN32

#include "RHI210D3D11EngineFixture.h"
#include "RHI210D3D11GoldenSupport.h"

#include "Game/CubeObject.h"
#include "Game/GameObject.h"
#include "Game/PlaneObject.h"
#include "Graphics/AssetPipeline.h"
#include "Graphics/GraphicsEngine.h"
#include "Graphics/PostProcessingPipeline.h"
#include "Utils/GoldenImageManifest.h"

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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
    const std::array<const char*, 4> kScenes = {"Frame_ForwardLit", "Frame_ForwardMaterial", "Frame_DrawList",
                                                "Frame_PostChain"};

    constexpr uint32_t kWidth = 320;
    constexpr uint32_t kHeight = 240;

    /// GraphicsEngine's default clear colour (0, 0.2, 0.4).
    constexpr Color3 kClear = {0.0f, 0.2f, 0.4f};
    constexpr Color3 kWhite = {1.0f, 1.0f, 1.0f};

    std::string Utf8(const std::filesystem::path& path)
    {
        const std::u8string text = path.generic_u8string();
        return {reinterpret_cast<const char*>(text.data()), text.size()};
    }

    struct Camera
    {
        XMMATRIX view;
        XMMATRIX projection;
        XMFLOAT3 position;
    };

    Camera MakeCamera(const XMFLOAT3& eye, const XMFLOAT3& target)
    {
        Camera camera;
        camera.view = XMMatrixLookAtLH(XMLoadFloat3(&eye), XMLoadFloat3(&target), XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
        camera.projection =
            XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), float(kWidth) / float(kHeight), 0.1f, 100.0f);
        camera.position = eye;
        return camera;
    }

    RHI210::Frame RenderFrame(GraphicsEngine& engine, const Camera& camera, const std::vector<GameObject*>& objects)
    {
        if (RHI210Golden::PassDisabled("forward"))
        {
            return RHI210::RenderObjectsFrame(engine, camera.view, camera.projection, {});
        }
        return RHI210::RenderObjectsFrame(engine, camera.view, camera.projection, objects);
    }

    RHI210Golden::HitBuffer CastFrame(const Camera& camera, const std::vector<Instance>& instances)
    {
        return RHI210Golden::CastFrame(kWidth, kHeight, camera.view, camera.projection, instances);
    }

    template <typename T>
    T* AddObject(GraphicsEngine& engine, std::vector<std::unique_ptr<GameObject>>& owned, std::unique_ptr<T> object,
                 const XMFLOAT3& position, const XMFLOAT3& rotation, const XMFLOAT3& scale)
    {
        if (FAILED(object->Initialize(engine.GetDevice(), engine.GetContext())))
        {
            return nullptr;
        }
        object->SetPosition(position);
        object->SetRotation(rotation);
        object->SetScale(scale);
        T* raw = object.get();
        owned.push_back(std::move(object));
        return raw;
    }

    /// The staged models CubeObject and PlaneObject load (working-directory relative).
    struct PrimitiveModels
    {
        std::optional<TriangleMesh> cube = RHI210Golden::LoadObjAsGameObjectMesh("Assets/Models/Cube.obj");
        std::optional<TriangleMesh> plane = RHI210Golden::LoadObjAsGameObjectMesh("Assets/Models/Plane.obj");

        bool Ok() const { return cube.has_value() && plane.has_value(); }
    };

    // ------------------------------------------------------------------------
    // Test-generated material (so its texels are known exactly)
    // ------------------------------------------------------------------------

    constexpr uint32_t kCheckerSize = 8;
    constexpr std::array<uint8_t, 3> kCheckerA = {230, 92, 58};
    constexpr std::array<uint8_t, 3> kCheckerB = {64, 118, 214};
    /// A strongly tilted constant tangent-space normal.
    constexpr std::array<uint8_t, 3> kTiltTexel = {150, 92, 245};
    constexpr float kMaterialRoughness = 0.25f;
    constexpr float kMaterialTiling = 2.0f;
    constexpr const char* kMaterialPath = "Assets/Materials/RHI210_Checker.json";

    bool CheckerIsA(int tx, int ty)
    {
        return ((tx / 2) + (ty / 2)) % 2 == 0;
    }

    /// Bilinear sample of the 8x8 checker at mip 0 with wrap addressing, or nothing
    /// when the four taps do not share one colour (filter precision is not modelled).
    std::optional<Color3> CheckerAlbedo(float u, float v)
    {
        const float fx = u * float(kCheckerSize) - 0.5f;
        const float fy = v * float(kCheckerSize) - 0.5f;
        const int x0 = static_cast<int>(std::floor(fx));
        const int y0 = static_cast<int>(std::floor(fy));
        auto wrap = [](int i) { return ((i % int(kCheckerSize)) + int(kCheckerSize)) % int(kCheckerSize); };
        const bool a = CheckerIsA(wrap(x0), wrap(y0));
        for (int dy = 0; dy <= 1; ++dy)
        {
            for (int dx = 0; dx <= 1; ++dx)
            {
                if (CheckerIsA(wrap(x0 + dx), wrap(y0 + dy)) != a)
                {
                    return std::nullopt;
                }
            }
        }
        const auto& texel = a ? kCheckerA : kCheckerB;
        return Color3{texel[0] / 255.0f, texel[1] / 255.0f, texel[2] / 255.0f};
    }

    /// A temporary project root holding Assets/Materials/RHI210_Checker.json and its
    /// two textures, written fresh for every run.
    class MaterialProject
    {
      public:
        MaterialProject() : m_root(std::filesystem::temp_directory_path() / "spark-rhi210-golden-material")
        {
            std::error_code ec;
            std::filesystem::remove_all(m_root, ec);
            std::filesystem::create_directories(m_root / "Assets" / "Materials", ec);
            std::filesystem::create_directories(m_root / "Assets" / "Textures", ec);

            std::vector<uint8_t> checker(kCheckerSize * kCheckerSize * 4);
            for (uint32_t y = 0; y < kCheckerSize; ++y)
            {
                for (uint32_t x = 0; x < kCheckerSize; ++x)
                {
                    const auto& texel = CheckerIsA(int(x), int(y)) ? kCheckerA : kCheckerB;
                    uint8_t* out = &checker[(size_t(y) * kCheckerSize + x) * 4];
                    out[0] = texel[0];
                    out[1] = texel[1];
                    out[2] = texel[2];
                    out[3] = 255;
                }
            }
            std::vector<uint8_t> tilt(4 * 4 * 4);
            for (size_t i = 0; i < tilt.size(); i += 4)
            {
                tilt[i + 0] = kTiltTexel[0];
                tilt[i + 1] = kTiltTexel[1];
                tilt[i + 2] = kTiltTexel[2];
                tilt[i + 3] = 255;
            }
            const std::filesystem::path textures = m_root / "Assets" / "Textures";
            m_ok = Spark::GoldenImageTestRunner::SavePNG((textures / "rhi210_checker.png").string(), checker.data(),
                                                         kCheckerSize, kCheckerSize) &&
                   Spark::GoldenImageTestRunner::SavePNG((textures / "rhi210_tilt.png").string(), tilt.data(), 4, 4);

            std::ofstream json(m_root / "Assets" / "Materials" / "RHI210_Checker.json", std::ios::binary);
            json << "{\n  \"albedo\": \"Textures/rhi210_checker.png\",\n"
                    "  \"normal\": \"Textures/rhi210_tilt.png\",\n"
                    "  \"roughness\": 0.25,\n  \"tiling\": [2, 2]\n}\n";
            m_ok = m_ok && json.good();
        }

        ~MaterialProject()
        {
            std::error_code ec;
            std::filesystem::remove_all(m_root, ec);
        }

        MaterialProject(const MaterialProject&) = delete;
        MaterialProject& operator=(const MaterialProject&) = delete;

        bool Ok() const { return m_ok; }
        std::string RootUtf8() const { return Utf8(m_root); }

      private:
        std::filesystem::path m_root;
        bool m_ok = false;
    };

    // ------------------------------------------------------------------------
    // The forward-lit scene (shared with Frame_PostChain)
    // ------------------------------------------------------------------------

    /// A floor and three default-material cubes. The draw order puts the nearest
    /// cube first, so its pixels survive only through depth testing.
    struct LitScene
    {
        std::vector<std::unique_ptr<GameObject>> owned;
        std::vector<GameObject*> drawOrder;
        std::vector<Instance> instances;
        bool ok = false;
    };

    LitScene BuildLitScene(GraphicsEngine& engine, const PrimitiveModels& models)
    {
        LitScene scene;
        auto* ground = AddObject(engine, scene.owned, std::make_unique<PlaneObject>(1.0f, 1.0f), {0.0f, 0.0f, 0.0f},
                                 {0.0f, 0.0f, 0.0f}, {9.0f, 1.0f, 9.0f});
        auto* farLeft = AddObject(engine, scene.owned, std::make_unique<CubeObject>(1.0f), {-1.4f, 0.0f, 0.9f},
                                  {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f});
        auto* tall = AddObject(engine, scene.owned, std::make_unique<CubeObject>(1.0f), {0.9f, 0.0f, 0.4f},
                               {0.0f, XMConvertToRadians(35.0f), 0.0f}, {1.0f, 1.5f, 1.0f});
        auto* nearCube = AddObject(engine, scene.owned, std::make_unique<CubeObject>(1.0f), {0.2f, 0.0f, -1.3f},
                                   {0.0f, XMConvertToRadians(-25.0f), 0.0f}, {0.7f, 0.7f, 0.7f});
        if (!ground || !farLeft || !tall || !nearCube || !models.Ok())
        {
            return scene;
        }
        scene.drawOrder = {ground, nearCube, tall, farLeft};
        scene.instances = {RHI210Golden::MakeInstance(*models.plane, ground->GetWorldMatrix()),
                           RHI210Golden::MakeInstance(*models.cube, nearCube->GetWorldMatrix()),
                           RHI210Golden::MakeInstance(*models.cube, tall->GetWorldMatrix()),
                           RHI210Golden::MakeInstance(*models.cube, farLeft->GetWorldMatrix())};
        scene.ok = true;
        return scene;
    }

    Camera LitCamera()
    {
        return MakeCamera({3.4f, 3.1f, -5.2f}, {0.0f, 0.6f, 0.2f});
    }

    float AcesChannel(float x)
    {
        return std::clamp((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f), 0.0f, 1.0f);
    }
} // namespace

// ----------------------------------------------------------------------------
// Every scene this lane certifies has a reviewed software-row entry and PNG.
// ----------------------------------------------------------------------------
TEST(D3D11FrameGolden_ManifestHasEveryScene)
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
// Clear + forward opaque pass: lighting, rotated/scaled normals, depth test.
// ----------------------------------------------------------------------------
TEST(D3D11FrameGolden_ForwardLit)
{
    const PrimitiveModels models;
    ASSERT_TRUE(models.Ok()); // run from the runtime directory (staged Assets/Models)
    RHI210::WarpGraphicsEngine warp(kWidth, kHeight);
    ASSERT_TRUE(warp.Ready());
    LitScene scene = BuildLitScene(warp.Engine(), models);
    ASSERT_TRUE(scene.ok);
    const Camera camera = LitCamera();

    const RHI210::Frame frame = RenderFrame(warp.Engine(), camera, scene.drawOrder);
    ASSERT_EQ(frame.width, kWidth);
    ASSERT_EQ(frame.height, kHeight);
    EXPECT_TRUE(Spark::GoldenImageTestRunner::FrameHasRenderedContent(frame.rgba, 0.6));

    const RHI210Golden::HitBuffer hits = CastFrame(camera, scene.instances);
    const RHI210Golden::ProbeTally tally =
        RHI210Golden::ProbeFrame("Frame_ForwardLit", frame.rgba, hits, kClear,
                                 [&](const SurfaceHit& hit, uint32_t, uint32_t) -> std::optional<Color3> {
                                     return RHI210Golden::ShadeBasic(kWhite, hit.normal, hit.position, camera.position);
                                 });
    EXPECT_GT(tally.background, 2000);
    EXPECT_GT(tally.surface, 30000);
    EXPECT_EQ(tally.failures, 0);
    EXPECT_TRUE(RHI210Golden::MatchesGolden(kRow, "Frame_ForwardLit", frame.rgba, frame.width, frame.height));
}

// ----------------------------------------------------------------------------
// Forward pass with a basic material: albedo texture, normal map, specular.
// ----------------------------------------------------------------------------
TEST(D3D11FrameGolden_ForwardMaterial)
{
    const PrimitiveModels models;
    ASSERT_TRUE(models.Ok());
    MaterialProject project;
    ASSERT_TRUE(project.Ok());
    RHI210::WarpGraphicsEngine warp(kWidth, kHeight);
    ASSERT_TRUE(warp.Ready());
    GraphicsEngine& engine = warp.Engine();

    std::vector<std::unique_ptr<GameObject>> owned;
    auto* ground = AddObject(engine, owned, std::make_unique<PlaneObject>(1.0f, 1.0f), {0.0f, 0.0f, 0.0f},
                             {0.0f, 0.0f, 0.0f}, {8.0f, 1.0f, 8.0f});
    auto* cube = AddObject(engine, owned, std::make_unique<CubeObject>(1.0f), {-1.5f, 0.0f, -0.9f},
                           {0.0f, XMConvertToRadians(30.0f), 0.0f}, {1.2f, 1.2f, 1.2f});
    ASSERT_TRUE(ground != nullptr && cube != nullptr);
    if (!RHI210Golden::PassDisabled("forward-material"))
    {
        ground->SetMaterialPath(kMaterialPath);
        cube->SetMaterialPath(kMaterialPath);
    }
    ASSERT_TRUE(ground->SetMaterialProjectRoot(project.RootUtf8()));
    ASSERT_TRUE(cube->SetMaterialProjectRoot(project.RootUtf8()));
    const std::vector<Instance> instances = {RHI210Golden::MakeInstance(*models.plane, ground->GetWorldMatrix()),
                                             RHI210Golden::MakeInstance(*models.cube, cube->GetWorldMatrix())};
    const Camera camera = MakeCamera({3.0f, 3.4f, 4.6f}, {0.0f, 0.4f, 0.0f});

    // The material and its textures load on first use inside the frame; capture the
    // second frame so nothing depends on load timing.
    const std::vector<GameObject*> objects = {ground, cube};
    RenderFrame(engine, camera, objects);
    const RHI210::Frame frame = RenderFrame(engine, camera, objects);
    ASSERT_EQ(frame.width, kWidth);
    EXPECT_TRUE(Spark::GoldenImageTestRunner::FrameHasRenderedContent(frame.rgba, 0.5));

    const RHI210Golden::HitBuffer hits = CastFrame(camera, instances);
    int specularProbes = 0;
    const RHI210Golden::ProbeTally tally = RHI210Golden::ProbeFrame(
        "Frame_ForwardMaterial", frame.rgba, hits, kClear,
        [&](const SurfaceHit& hit, uint32_t x, uint32_t y) -> std::optional<Color3>
        {
            // Only where the checker is magnified (the sampler reads mip 0) and the four
            // bilinear taps share one colour.
            if (RHI210Golden::TexelFootprint(hits, x, y, float(kCheckerSize) * kMaterialTiling) >= 0.5f)
            {
                return std::nullopt;
            }
            const std::optional<Color3> albedo = CheckerAlbedo(hit.uv.x * kMaterialTiling, hit.uv.y * kMaterialTiling);
            if (!albedo)
            {
                return std::nullopt;
            }
            const XMFLOAT3 normal = RHI210Golden::MappedNormal(hits, x, y, kTiltTexel, kMaterialTiling);
            const Color3 lit =
                RHI210Golden::ShadeBasic(*albedo, normal, hit.position, camera.position, kMaterialRoughness);
            const Color3 diffuseOnly = RHI210Golden::ShadeBasic(*albedo, normal, hit.position, camera.position);
            if (RHI210Golden::ToUnorm8(lit[1]) - RHI210Golden::ToUnorm8(diffuseOnly[1]) > 8)
            {
                ++specularProbes;
            }
            return lit;
        });
    std::printf("[RHI-210 GOLDEN] Frame_ForwardMaterial probes with a visible specular term: %d\n", specularProbes);
    EXPECT_GT(tally.surface, 10000);
    EXPECT_GT(specularProbes, 50);
    EXPECT_EQ(tally.failures, 0);
    EXPECT_TRUE(RHI210Golden::MatchesGolden(kRow, "Frame_ForwardMaterial", frame.rgba, frame.width, frame.height));
}

// ----------------------------------------------------------------------------
// ECS draw list: AssetPipeline OBJ meshes and a texture material.
// ----------------------------------------------------------------------------
TEST(D3D11FrameGolden_DrawList)
{
    const std::filesystem::path source(SPARK_TEST_SOURCE_DIR);
    const std::filesystem::path quadDir = std::filesystem::temp_directory_path() / "spark-rhi210-golden-drawlist";
    std::error_code ec;
    std::filesystem::remove_all(quadDir, ec);
    std::filesystem::create_directories(quadDir, ec);
    {
        // A 2x2 quad facing +y (front faces wind so cross(p1 - p0, p2 - p0) faces the
        // viewer, as in the shipped Blender exports); OBJ v is flipped on import.
        std::ofstream obj(quadDir / "rhi210_quad.obj", std::ios::binary);
        obj << "v -1 0 -1\nv 1 0 -1\nv 1 0 1\nv -1 0 1\n"
               "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
               "vn 0 1 0\n"
               "f 1/1/1 4/4/1 3/3/1\nf 1/1/1 3/3/1 2/2/1\n";
        ASSERT_TRUE(obj.good());
    }
    // MeshDrawCommand keeps string_views: these strings outlive every frame below.
    const std::string quadPath = (quadDir / "rhi210_quad.obj").string();
    const std::string cubePath = (source / "Assets" / "Models" / "Cube.obj").string();
    const std::string checkerPath = (source / "Assets" / "Textures" / "Default" / "checkerboard.png").string();
    const std::string noMaterial;

    RHI210::WarpGraphicsEngine warp(kWidth, kHeight);
    ASSERT_TRUE(warp.Ready());
    GraphicsEngine& engine = warp.Engine();
    AssetPipeline* assets = engine.GetAssetPipeline();
    ASSERT_TRUE(assets != nullptr);
    const std::shared_ptr<MeshAsset> quadAsset = assets->LoadMesh(quadPath);
    const std::shared_ptr<MeshAsset> cubeAsset = assets->LoadMesh(cubePath);
    ASSERT_TRUE(quadAsset != nullptr && cubeAsset != nullptr);
    ASSERT_TRUE(assets->LoadTexture(checkerPath) != nullptr);
    const TriangleMesh quadMesh = RHI210Golden::FromMeshAssetData(quadAsset->GetMeshData());
    const TriangleMesh cubeMesh = RHI210Golden::FromMeshAssetData(cubeAsset->GetMeshData());

    // The shipped checkerboard: 64x64, 8x8-texel cells of two greys.
    uint32_t textureWidth = 0;
    uint32_t textureHeight = 0;
    const std::vector<uint8_t> checker =
        Spark::GoldenImageTestRunner::LoadPNG(checkerPath, textureWidth, textureHeight);
    ASSERT_EQ(textureWidth, 64u);
    ASSERT_EQ(textureHeight, 64u);

    struct Draw
    {
        const std::string* mesh;
        const std::string* material;
        XMMATRIX world;
    };
    const std::vector<Draw> draws = {
        {&quadPath, &checkerPath, XMMatrixScaling(3.0f, 1.0f, 3.0f)},
        {&cubePath, &noMaterial, XMMatrixTranslation(-1.0f, 0.0f, 0.6f)},
        {&cubePath, &noMaterial,
         XMMatrixScaling(0.8f, 0.8f, 0.8f) * XMMatrixRotationY(XMConvertToRadians(40.0f)) *
             XMMatrixTranslation(1.0f, 0.0f, -0.4f)},
        {&cubePath, &noMaterial, XMMatrixScaling(0.6f, 1.4f, 0.6f) * XMMatrixTranslation(0.1f, 0.0f, 1.6f)},
    };
    std::vector<Instance> instances = {RHI210Golden::MakeInstance(quadMesh, draws[0].world)};
    for (size_t i = 1; i < draws.size(); ++i)
    {
        instances.push_back(RHI210Golden::MakeInstance(cubeMesh, draws[i].world));
    }
    const Camera camera = MakeCamera({2.8f, 3.2f, -4.4f}, {0.0f, 0.4f, 0.3f});

    RHI210::Frame frame;
    for (int pass = 0; pass < 2; ++pass)
    {
        for (const Draw& draw : draws)
        {
            if (!RHI210Golden::PassDisabled("drawlist"))
            {
                engine.SubmitMeshForRendering(*draw.mesh, *draw.material, draw.world, false);
            }
        }
        frame = RenderFrame(engine, camera, {});
    }
    ASSERT_EQ(frame.width, kWidth);
    EXPECT_TRUE(Spark::GoldenImageTestRunner::FrameHasRenderedContent(frame.rgba, 0.6));

    const RHI210Golden::HitBuffer hits = CastFrame(camera, instances);
    int texturedProbes = 0;
    const RHI210Golden::ProbeTally tally = RHI210Golden::ProbeFrame(
        "Frame_DrawList", frame.rgba, hits, kClear,
        [&](const SurfaceHit& hit, uint32_t x, uint32_t y) -> std::optional<Color3>
        {
            if (hit.instance != 0)
            {
                return RHI210Golden::ShadeBasic(kWhite, hit.normal, hit.position, camera.position);
            }
            // The textured quad: probe 8x8-texel cell interiors of the magnified checker.
            if (RHI210Golden::TexelFootprint(hits, x, y, 64.0f) >= 0.5f)
            {
                return std::nullopt;
            }
            const float tx = hit.uv.x * 64.0f;
            const float ty = hit.uv.y * 64.0f;
            const float cellX = tx / 8.0f - std::floor(tx / 8.0f);
            const float cellY = ty / 8.0f - std::floor(ty / 8.0f);
            if (cellX < 0.15f || cellX > 0.85f || cellY < 0.15f || cellY > 0.85f)
            {
                return std::nullopt;
            }
            const uint32_t texelX = std::min(63u, static_cast<uint32_t>(tx));
            const uint32_t texelY = std::min(63u, static_cast<uint32_t>(ty));
            const float grey = checker[(size_t(texelY) * 64 + texelX) * 4] / 255.0f;
            ++texturedProbes;
            return RHI210Golden::ShadeBasic({grey, grey, grey}, hit.normal, hit.position, camera.position);
        });
    std::printf("[RHI-210 GOLDEN] Frame_DrawList textured quad probes: %d\n", texturedProbes);
    EXPECT_GT(texturedProbes, 5000);
    EXPECT_GT(tally.surface, 20000);
    EXPECT_EQ(tally.failures, 0);
    EXPECT_TRUE(RHI210Golden::MatchesGolden(kRow, "Frame_DrawList", frame.rgba, frame.width, frame.height));
    std::filesystem::remove_all(quadDir, ec);
}

// ----------------------------------------------------------------------------
// The post-processing chain inside the real frame.
// ----------------------------------------------------------------------------
TEST(D3D11FrameGolden_PostChain)
{
    using Spark::Graphics::PostProcessPass;

    const PrimitiveModels models;
    ASSERT_TRUE(models.Ok());
    RHI210::WarpGraphicsEngine warp(kWidth, kHeight);
    ASSERT_TRUE(warp.Ready());
    GraphicsEngine& engine = warp.Engine();
    LitScene scene = BuildLitScene(engine, models);
    ASSERT_TRUE(scene.ok);
    const Camera camera = LitCamera();

    // The same scene without post-processing, to prove the chain changed the frame.
    const RHI210::Frame plain = RenderFrame(engine, camera, scene.drawOrder);
    ASSERT_EQ(plain.width, kWidth);
    ASSERT_EQ(plain.height, kHeight);
    ASSERT_EQ(plain.rgba.size(), size_t(kWidth) * kHeight * 4);

    Spark::Graphics::PostProcessingPipeline* post = engine.GetPostProcessingPipeline();
    ASSERT_TRUE(post != nullptr);
    ASSERT_TRUE(post->IsInitialized());
    const bool enablePost = !RHI210Golden::PassDisabled("postchain");
    post->SetEffectEnabled(PostProcessPass::GTAO, enablePost);
    post->SetEffectEnabled(PostProcessPass::Bloom, enablePost);
    post->SetEffectEnabled(PostProcessPass::Tonemapping, enablePost);
    post->SetEffectEnabled(PostProcessPass::FXAA, enablePost);
    post->GetBloomSettings().threshold = 0.7f;
    post->GetTonemappingSettings().op = Spark::Graphics::TonemapOperator::ACES;

    const RHI210::Frame frame = RenderFrame(engine, camera, scene.drawOrder);
    ASSERT_EQ(frame.width, kWidth);
    ASSERT_EQ(frame.height, kHeight);
    ASSERT_EQ(frame.rgba.size(), plain.rgba.size());
    // A pass whose shader failed to compile or bind is skipped by Process().
    EXPECT_EQ(post->GetActivePassCount(), 4);
    EXPECT_TRUE(Spark::GoldenImageTestRunner::FrameHasRenderedContent(frame.rgba, 0.6));

    // The background is the clear colour through the chain: the far plane is
    // unoccluded, bloom is below its threshold and FXAA keeps flat regions, so only
    // ACES applies there.
    const Color3 background = {AcesChannel(kClear[0]), AcesChannel(kClear[1]), AcesChannel(kClear[2])};
    EXPECT_LE(RHI210Golden::ChannelError(RHI210Golden::PixelAt(frame.rgba, kWidth, 4, 4), background), 1);
    EXPECT_LE(RHI210Golden::ChannelError(RHI210Golden::PixelAt(frame.rgba, kWidth, kWidth - 5, 4), background), 1);

    int changed = 0;
    for (size_t i = 0; i < frame.rgba.size(); i += 4)
    {
        const bool same = frame.rgba[i] == plain.rgba[i] && frame.rgba[i + 1] == plain.rgba[i + 1] &&
                          frame.rgba[i + 2] == plain.rgba[i + 2];
        changed += same ? 0 : 1;
    }
    std::printf("[RHI-210 GOLDEN] Frame_PostChain pixels changed by the chain: %d/%u\n", changed, kWidth * kHeight);
    EXPECT_GT(changed, int(kWidth * kHeight) / 2);
    EXPECT_TRUE(RHI210Golden::MatchesGolden(kRow, "Frame_PostChain", frame.rgba, frame.width, frame.height));
}

#endif // _WIN32
