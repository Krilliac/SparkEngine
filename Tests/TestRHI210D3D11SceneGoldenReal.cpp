/**
 * @file TestRHI210D3D11SceneGoldenReal.cpp
 * @brief RHI-210: the FPS module's authored scene on the d3d11-warp golden row.
 *
 * Representative content: Assets/Scenes/level1.scene, the scene SparkGameFPS
 * loads at startup (FPSVisibleFrame_AuthoredAndFallback checks that the shipped
 * module really loads it). The scene goes through the production path the module
 * uses: SceneManager::LoadScene instantiates the authored nodes as GameObjects,
 * each object's material project root is bound as Game::BindSceneMaterialRoots
 * does, and a WARP GraphicsEngine renders them with BeginFrame / RenderScene /
 * EndFrame (the default Forward pipeline and the authored basic materials) at
 * 640x360. The frame is read back before Present and compared with the committed
 * baseline (reviewed thresholds and SHA-256 in Tests/GoldenImages/manifest.json,
 * fail-closed).
 *
 * Scenes:
 *   - Scene_FPSLevel1_MainCamera: the authored main camera, set up exactly as
 *     Game::Initialize does (SparkEngineCamera, authored position and rotation,
 *     authored clipping planes): what the player sees at spawn.
 *   - Scene_FPSLevel1_Overview: an elevated view over the whole arena, so every
 *     authored floor, wall, crate and ramp is in frame.
 *
 * Out of scope (drawn by SparkGameFPS module code, not by the authored scene):
 * the procedural arena models of Game::CreateCombatArena, enemies, vehicles,
 * pickups, the first-person weapon and the HUD. Their frames depend on wall-clock
 * game time, so they cannot be compared against a fixed baseline.
 *
 * Every interior pixel of a floor, wall tile, crate or ramp face (and the sky) is
 * checked against a CPU ray cast shaded with the basic pixel shader formula and
 * the authored material values, so a broken render cannot become a baseline.
 */

#include "TestFramework.h"

#ifdef _WIN32

#include "RHI210D3D11EngineFixture.h"
#include "RHI210D3D11GoldenSupport.h"

#include "Camera/SparkEngineCamera.h"
#include "Game/GameObject.h"
#include "Graphics/GraphicsEngine.h"
#include "SceneManager/SceneManager.h"
#include "Utils/GoldenImageManifest.h"

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <deque>
#include <map>
#include <optional>
#include <sstream>
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
    const std::array<const char*, 2> kScenes = {"Scene_FPSLevel1_MainCamera", "Scene_FPSLevel1_Overview"};

    constexpr uint32_t kWidth = 640;
    constexpr uint32_t kHeight = 360;
    constexpr Color3 kClear = {0.0f, 0.2f, 0.4f};

    std::filesystem::path SourceRoot()
    {
        return std::filesystem::path(SPARK_TEST_SOURCE_DIR);
    }

    std::string Utf8(const std::filesystem::path& path)
    {
        const std::u8string text = path.generic_u8string();
        return {reinterpret_cast<const char*>(text.data()), text.size()};
    }

    /// The values the basic shader sees for one authored material JSON.
    struct MaterialValues
    {
        Color3 albedo{};
        std::array<uint8_t, 3> normalTexel{};
        float roughness = 1.0f;
        bool ok = false;
    };

    std::string JsonString(const std::string& text, const std::string& key)
    {
        const size_t keyAt = text.find("\"" + key + "\"");
        const size_t open = keyAt == std::string::npos ? keyAt : text.find('"', text.find(':', keyAt) + 1);
        const size_t close = open == std::string::npos ? open : text.find('"', open + 1);
        return close == std::string::npos ? std::string() : text.substr(open + 1, close - open - 1);
    }

    /// The first texel of a uniform texture (every shipped material texture is one
    /// colour), or nothing when the texture is not uniform.
    std::optional<std::array<uint8_t, 3>> UniformTexel(const std::filesystem::path& path)
    {
        uint32_t width = 0;
        uint32_t height = 0;
        const std::vector<uint8_t> rgba = Spark::GoldenImageTestRunner::LoadPNG(path.string(), width, height);
        if (rgba.empty())
        {
            return std::nullopt;
        }
        for (size_t i = 0; i < rgba.size(); i += 4)
        {
            if (rgba[i] != rgba[0] || rgba[i + 1] != rgba[1] || rgba[i + 2] != rgba[2])
            {
                return std::nullopt;
            }
        }
        return std::array<uint8_t, 3>{rgba[0], rgba[1], rgba[2]};
    }

    MaterialValues ReadMaterial(const std::string& materialPath)
    {
        MaterialValues values;
        std::ifstream file(SourceRoot() / materialPath, std::ios::binary);
        std::stringstream text;
        text << file.rdbuf();
        const std::string json = text.str();
        const auto albedo = UniformTexel(SourceRoot() / "Assets" / JsonString(json, "albedo"));
        const auto normal = UniformTexel(SourceRoot() / "Assets" / JsonString(json, "normal"));
        const size_t roughnessAt = json.find("\"roughness\"");
        if (!albedo || !normal || roughnessAt == std::string::npos)
        {
            return values;
        }
        values.albedo = {(*albedo)[0] / 255.0f, (*albedo)[1] / 255.0f, (*albedo)[2] / 255.0f};
        values.normalTexel = *normal;
        values.roughness = std::stof(json.substr(json.find(':', roughnessAt) + 1));
        values.ok = true;
        return values;
    }

    /// level1.scene loaded the way SparkGameFPS loads it.
    struct LoadedLevel
    {
        std::unique_ptr<SceneManager> scene;
        std::vector<GameObject*> objects;
        std::deque<TriangleMesh> meshes; ///< Stable storage for Instance::mesh pointers.
        std::vector<Instance> instances;
        std::vector<bool> probed;           ///< Per instance: an exact material is available for probing.
        std::vector<std::string> materials; ///< Per instance: its authored material path.
        std::map<std::string, MaterialValues> materialValues;
        const SceneNode* mainCamera = nullptr;
        int nodes = 0;
        int boundRoots = 0;
    };

    LoadedLevel LoadLevel(GraphicsEngine& engine)
    {
        LoadedLevel level;
        level.scene = std::make_unique<SceneManager>(&engine, nullptr);
        if (!level.scene->LoadScene((SourceRoot() / "Assets" / "Scenes" / "level1.scene").wstring()))
        {
            return level;
        }
        level.nodes = level.scene->GetNodeCount();
        const std::string projectRoot = Utf8(SourceRoot());
        const auto& objects = level.scene->GetObjects();
        for (int i = 0; i < level.nodes && i < static_cast<int>(objects.size()); ++i)
        {
            const SceneNode* node = level.scene->GetNode(i);
            if (node && node->type == "Camera" && !level.mainCamera)
            {
                level.mainCamera = node;
            }
            GameObject* object = objects[static_cast<size_t>(i)].get();
            if (!node || !object)
            {
                continue;
            }
            // Game::BindSceneMaterialRoots: the project root is trusted module state.
            level.boundRoots += object->SetMaterialProjectRoot(projectRoot) ? 1 : 0;
            if (!object->IsActive() || !object->IsVisible())
            {
                continue;
            }
            level.objects.push_back(object);

            const Mesh* mesh = object->GetMesh();
            const auto cpuMesh =
                mesh ? RHI210Golden::ReadMeshBuffers(*mesh, engine.GetDevice(), engine.GetContext()) : std::nullopt;
            ASSERT_TRUE(cpuMesh.has_value());
            level.meshes.push_back(*cpuMesh);
            level.instances.push_back(RHI210Golden::MakeInstance(level.meshes.back(), object->GetWorldMatrix()));
            const std::string& material = object->GetMaterialPath();
            if (!material.empty() && level.materialValues.find(material) == level.materialValues.end())
            {
                level.materialValues[material] = ReadMaterial(material);
            }
            const auto values = level.materialValues.find(material);
            level.probed.push_back(values != level.materialValues.end() && values->second.ok);
            level.materials.push_back(material);
        }
        return level;
    }

    /// Renders the level and checks every probe-able pixel against the CPU reference.
    RHI210::Frame RenderAndProbe(const char* scene, GraphicsEngine& engine, LoadedLevel& level, const XMMATRIX& view,
                                 const XMMATRIX& projection, const XMFLOAT3& cameraPosition,
                                 RHI210Golden::ProbeTally& tally)
    {
        // Materials and their textures load on first use inside the frame; capture the
        // second frame so nothing depends on load timing.
        const std::vector<GameObject*> emptyObjects;
        const auto& drawObjects = RHI210Golden::PassDisabled("scene") ? emptyObjects : level.objects;
        RHI210::RenderObjectsFrame(engine, view, projection, drawObjects);
        RHI210::Frame frame = RHI210::RenderObjectsFrame(engine, view, projection, drawObjects);
        const RHI210Golden::HitBuffer hits =
            RHI210Golden::CastFrame(frame.width, frame.height, view, projection, level.instances);
        tally = RHI210Golden::ProbeFrame(scene, frame.rgba, hits, {kClear[0], kClear[1], kClear[2]},
                                         [&](const SurfaceHit& hit, uint32_t px, uint32_t py) -> std::optional<Color3>
                                         {
                                             const size_t instance = static_cast<size_t>(hit.instance);
                                             if (instance >= level.probed.size() || !level.probed[instance])
                                             {
                                                 return std::nullopt;
                                             }
                                             const MaterialValues& material =
                                                 level.materialValues.at(level.materials[instance]);
                                             const XMFLOAT3 normal =
                                                 RHI210Golden::MappedNormal(hits, px, py, material.normalTexel);
                                             return RHI210Golden::ShadeBasic(material.albedo, normal, hit.position,
                                                                             cameraPosition, material.roughness);
                                         });
        return frame;
    }

    void CheckLevelStructure(const LoadedLevel& level)
    {
        // The authored arena (FPS logs the same identity), with its geometry
        // instantiated and every renderable object's material root bound.
        EXPECT_EQ(level.scene->GetMetadata().sceneName, std::string("FPS Arena"));
        EXPECT_GT(level.nodes, 50);
        EXPECT_GT(static_cast<int>(level.objects.size()), 50);
        EXPECT_EQ(level.boundRoots, static_cast<int>(level.objects.size()));
        for (const auto& [path, values] : level.materialValues)
        {
            if (!values.ok)
            {
                std::printf("[RHI-210 GOLDEN] authored material could not be read for the CPU reference: %s\n",
                            path.c_str());
            }
            EXPECT_TRUE(values.ok);
        }
    }
} // namespace

// ----------------------------------------------------------------------------
// Every scene this lane certifies has a reviewed software-row entry and PNG.
// ----------------------------------------------------------------------------
TEST(D3D11SceneGolden_ManifestHasEveryScene)
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
// level1.scene from the authored main camera, set up as Game::Initialize does.
// ----------------------------------------------------------------------------
TEST(D3D11SceneGolden_FPSLevel1MainCamera)
{
    RHI210::WarpGraphicsEngine warp(kWidth, kHeight);
    ASSERT_TRUE(warp.Ready());
    LoadedLevel level = LoadLevel(warp.Engine());
    ASSERT_TRUE(level.scene != nullptr && level.nodes > 0);
    CheckLevelStructure(level);
    ASSERT_TRUE(level.mainCamera != nullptr);

    SparkEngineCamera camera;
    camera.Initialize(float(kWidth) / float(kHeight));
    camera.SetPosition(level.mainCamera->position);
    camera.Console_SetRotation(level.mainCamera->rotation.x, level.mainCamera->rotation.y,
                               level.mainCamera->rotation.z);
    const auto nearPlane = level.mainCamera->properties.find("nearPlane");
    const auto farPlane = level.mainCamera->properties.find("farPlane");
    ASSERT_TRUE(nearPlane != level.mainCamera->properties.end() && farPlane != level.mainCamera->properties.end());
    camera.Console_SetClippingPlanes(std::stof(nearPlane->second), std::stof(farPlane->second));

    RHI210Golden::ProbeTally tally;
    const RHI210::Frame frame =
        RenderAndProbe("Scene_FPSLevel1_MainCamera", warp.Engine(), level, camera.GetViewMatrix(),
                       camera.GetProjectionMatrix(), camera.GetPosition(), tally);
    ASSERT_EQ(frame.width, kWidth);
    ASSERT_EQ(frame.height, kHeight);
    EXPECT_TRUE(Spark::GoldenImageTestRunner::FrameHasRenderedContent(frame.rgba, 0.6));
    EXPECT_GT(tally.background, 10000);
    EXPECT_GT(tally.surface, 50000);
    EXPECT_EQ(tally.failures, 0);
    EXPECT_TRUE(RHI210Golden::MatchesGolden(kRow, "Scene_FPSLevel1_MainCamera", frame.rgba, frame.width, frame.height));
}

// ----------------------------------------------------------------------------
// level1.scene from above: every authored floor, wall tile, crate and ramp.
// ----------------------------------------------------------------------------
TEST(D3D11SceneGolden_FPSLevel1Overview)
{
    RHI210::WarpGraphicsEngine warp(kWidth, kHeight);
    ASSERT_TRUE(warp.Ready());
    LoadedLevel level = LoadLevel(warp.Engine());
    ASSERT_TRUE(level.scene != nullptr && level.nodes > 0);
    CheckLevelStructure(level);

    const XMFLOAT3 eye = {26.0f, 30.0f, -46.0f};
    const XMFLOAT3 target = {0.0f, 0.0f, 2.0f};
    const XMMATRIX view =
        XMMatrixLookAtLH(XMLoadFloat3(&eye), XMLoadFloat3(&target), XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
    const XMMATRIX projection =
        XMMatrixPerspectiveFovLH(XMConvertToRadians(50.0f), float(kWidth) / float(kHeight), 0.5f, 400.0f);

    RHI210Golden::ProbeTally tally;
    const RHI210::Frame frame =
        RenderAndProbe("Scene_FPSLevel1_Overview", warp.Engine(), level, view, projection, eye, tally);
    ASSERT_EQ(frame.width, kWidth);
    // With every authored object drawn, WARP measured 0.7003 background. Keep
    // the overview composition and 4.97 percentage points of coverage margin;
    // all eligible CPU colour probes must still match.
    EXPECT_TRUE(Spark::GoldenImageTestRunner::FrameHasRenderedContent(frame.rgba, 0.75));
    EXPECT_GT(tally.surface, 50000);
    EXPECT_EQ(tally.failures, 0);
    EXPECT_TRUE(RHI210Golden::MatchesGolden(kRow, "Scene_FPSLevel1_Overview", frame.rgba, frame.width, frame.height));
}

#endif // _WIN32
