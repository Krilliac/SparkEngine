/**
 * @file TestEditorSubsystemsReal.cpp
 * @brief Production-source tests for the editor subsystems that had none
 *
 * Every test here drives the real classes:
 *   - SparkEditor::TerrainEditor writing a .sparkterrain and Spark::Graphics::TerrainRenderer reading it,
 *     which is the editor-to-runtime path that shipped with a 3-byte field-width desync and no coverage.
 *   - SparkEditor::PrefabManager instantiating, applying and reverting against a real SceneFile.
 *   - SparkEditor::PerformanceProfiler being fed a real frame delta.
 *
 * There are no reimplementations in this file.
 */

#include "TestFramework.h"

#include "Prefabs/PrefabManager.h"
#include "Profiler/PerformanceProfiler.h"
#include "Panels/MaterialEditorPanel.h"
#include "SceneSystem/SceneFile.h"
#include "Terrain/TerrainEditor.h"
#include "Utils/FileUtils.h"

#include "Core/EngineContext.h"
#include "Engine/ECS/Components.h"

#include "Engine/ECS/Components/TerrainComponents.h"
#include "Graphics/TerrainRenderer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace
{
    /// @brief Unique scratch directory for one test, removed by the destructor.
    class ScratchDir
    {
      public:
        explicit ScratchDir(const std::string& tag)
        {
            static int counter = 0;
            m_path = std::filesystem::temp_directory_path() /
                     ("spark_editor_subsystems_" + tag + "_" + std::to_string(++counter));
            std::error_code ec;
            std::filesystem::remove_all(m_path, ec);
            std::filesystem::create_directories(m_path, ec);
        }

        ~ScratchDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(m_path, ec);
        }

        ScratchDir(const ScratchDir&) = delete;
        ScratchDir& operator=(const ScratchDir&) = delete;

        std::string File(const std::string& name) const { return (m_path / name).string(); }
        const std::filesystem::path& Path() const { return m_path; }

      private:
        std::filesystem::path m_path;
    };

    class MaterialWorkingDirectoryGuard
    {
      public:
        explicit MaterialWorkingDirectoryGuard(const std::filesystem::path& next)
            : m_previous(std::filesystem::current_path())
        {
            std::filesystem::current_path(next);
        }
        ~MaterialWorkingDirectoryGuard()
        {
            std::error_code error;
            std::filesystem::current_path(m_previous, error);
        }
        MaterialWorkingDirectoryGuard(const MaterialWorkingDirectoryGuard&) = delete;
        MaterialWorkingDirectoryGuard& operator=(const MaterialWorkingDirectoryGuard&) = delete;

      private:
        std::filesystem::path m_previous;
    };

    /// @brief Build a small authored terrain in the editor: heights, a bound texture layer, a detail mesh.
    void AuthorTerrain(SparkEditor::TerrainEditor& editor, int resolution)
    {
        editor.CreateNewTerrain(500.0f, resolution);
        SparkEditor::TerrainData* terrain = editor.GetCurrentTerrain();
        ASSERT_TRUE(terrain != nullptr);

        terrain->name = "RoundTrip";
        terrain->generateCollider = false;
        terrain->lodLevels = 3;
        terrain->lodBias = 2.5f;
        terrain->heightmap.scale = 1.25f;
        terrain->heightmap.minHeight = -4.0f;
        terrain->heightmap.maxHeight = 77.0f;
        for (size_t i = 0; i < terrain->heightmap.heights.size(); ++i)
            terrain->heightmap.heights[i] = static_cast<float>(i) * 0.5f;

        SparkEditor::TerrainTextureLayer* layer = terrain->GetTextureLayer(0);
        ASSERT_TRUE(layer != nullptr);
        layer->name = "Grass";
        layer->diffuseTexture = "Textures/Terrain/Grass_D.png";
        layer->normalTexture = "Textures/Terrain/Grass_N.png";
        layer->tiling = {4.0f, 8.0f};
        layer->metallic = 0.25f;
        layer->roughness = 0.75f;

        auto detail = std::make_unique<SparkEditor::TerrainDetailMesh>();
        detail->name = "Bushes";
        detail->meshPath = "Meshes/Bush.fbx";
        detail->density = 3.5f;
        terrain->detailMeshes.push_back(std::move(detail));
        terrain->detailInstances.resize(1);
        terrain->detailInstances[0] = {{1.0f, 2.0f, 3.0f}, {4.0f, 5.0f, 6.0f}};

        if (!terrain->splatmaps.empty())
            terrain->splatmaps[0] = 200;
    }
} // namespace

// ============================================================================
// .sparkterrain: editor writer versus runtime reader
// ============================================================================

TEST(EditorSubsystemsReal_TerrainRoundTripsFromEditorWriterToRuntimeReader)
{
    ScratchDir scratch("terrain_roundtrip");
    const std::string path = scratch.File("round.sparkterrain");

    SparkEditor::TerrainEditor editor;
    AuthorTerrain(editor, 17);
    ASSERT_TRUE(editor.SaveTerrain(path));

    TerrainComponent component;
    ASSERT_TRUE(Spark::Graphics::TerrainRenderer::LoadSparkTerrain(path, component));

    // Field widths must agree, or every field after generateCollider is read at the wrong offset.
    EXPECT_EQ(component.heightmapResolution, 17);
    EXPECT_NEAR(component.terrainSize, 500.0f, 0.001f);
    EXPECT_EQ(component.lodLevels, 3);
    EXPECT_NEAR(component.lodBias, 2.5f, 0.001f);
    EXPECT_FALSE(component.generateCollider);
    EXPECT_NEAR(component.heightScale, 1.25f, 0.001f);
    EXPECT_NEAR(component.minHeight, -4.0f, 0.001f);
    EXPECT_NEAR(component.maxHeight, 77.0f, 0.001f);

    ASSERT_EQ(component.heightmap.size(), static_cast<size_t>(17 * 17));
    EXPECT_NEAR(component.heightmap[0], 0.0f, 0.0001f);
    EXPECT_NEAR(component.heightmap[10], 5.0f, 0.0001f);
    EXPECT_NEAR(component.heightmap.back(), static_cast<float>(17 * 17 - 1) * 0.5f, 0.0001f);

    EXPECT_EQ(component.splatmapResolution, 512);
    ASSERT_EQ(component.splatmap.size(), static_cast<size_t>(512) * 512 * 4);
    EXPECT_EQ(static_cast<int>(component.splatmap[0]), 200);
}

TEST(EditorSubsystemsReal_TerrainLayerBindingsReachTheRuntimeAsTexturePaths)
{
    ScratchDir scratch("terrain_layers");
    const std::string path = scratch.File("layers.sparkterrain");

    SparkEditor::TerrainEditor editor;
    AuthorTerrain(editor, 9);
    ASSERT_TRUE(editor.SaveTerrain(path));

    TerrainComponent component;
    ASSERT_TRUE(Spark::Graphics::TerrainRenderer::LoadSparkTerrain(path, component));

    // textureLayerPaths is a texture path list, not a display-name list.
    ASSERT_EQ(component.textureLayerPaths.size(), static_cast<size_t>(1));
    EXPECT_EQ(component.textureLayerPaths[0], std::string("Textures/Terrain/Grass_D.png"));
    EXPECT_NE(component.textureLayerPaths[0], std::string("Grass"));
}

TEST(EditorSubsystemsReal_TerrainEditorReloadKeepsLayerAndDetailAuthoring)
{
    ScratchDir scratch("terrain_editor_reload");
    const std::string path = scratch.File("authoring.sparkterrain");

    SparkEditor::TerrainEditor writer;
    AuthorTerrain(writer, 9);
    ASSERT_TRUE(writer.SaveTerrain(path));

    SparkEditor::TerrainEditor reader;
    ASSERT_TRUE(reader.LoadTerrain(path));
    SparkEditor::TerrainData* terrain = reader.GetCurrentTerrain();
    ASSERT_TRUE(terrain != nullptr);

    EXPECT_EQ(terrain->name, std::string("RoundTrip"));
    ASSERT_EQ(terrain->textureLayers.size(), static_cast<size_t>(1));
    EXPECT_EQ(terrain->textureLayers[0]->diffuseTexture, std::string("Textures/Terrain/Grass_D.png"));
    EXPECT_EQ(terrain->textureLayers[0]->normalTexture, std::string("Textures/Terrain/Grass_N.png"));
    EXPECT_NEAR(terrain->textureLayers[0]->tiling.x, 4.0f, 0.001f);
    EXPECT_NEAR(terrain->textureLayers[0]->tiling.y, 8.0f, 0.001f);
    EXPECT_NEAR(terrain->textureLayers[0]->metallic, 0.25f, 0.001f);
    EXPECT_NEAR(terrain->textureLayers[0]->roughness, 0.75f, 0.001f);

    ASSERT_EQ(terrain->detailMeshes.size(), static_cast<size_t>(1));
    EXPECT_EQ(terrain->detailMeshes[0]->meshPath, std::string("Meshes/Bush.fbx"));
    ASSERT_EQ(terrain->detailInstances.size(), static_cast<size_t>(1));
    ASSERT_EQ(terrain->detailInstances[0].size(), static_cast<size_t>(2));
    EXPECT_NEAR(terrain->detailInstances[0][1].y, 5.0f, 0.001f);

    EXPECT_EQ(reader.GetTerrainFilePath(), path);
}

TEST(EditorSubsystemsReal_TruncatedTerrainFileIsRejectedByBothReaders)
{
    ScratchDir scratch("terrain_truncated");
    const std::string full = scratch.File("full.sparkterrain");
    const std::string truncated = scratch.File("truncated.sparkterrain");

    SparkEditor::TerrainEditor editor;
    AuthorTerrain(editor, 9);
    ASSERT_TRUE(editor.SaveTerrain(full));

    std::vector<char> bytes;
    {
        std::ifstream in(full, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    ASSERT_TRUE(bytes.size() > 64);
    {
        std::ofstream out(truncated, std::ios::binary);
        out.write(bytes.data(), 48);
    }

    TerrainComponent component;
    EXPECT_FALSE(Spark::Graphics::TerrainRenderer::LoadSparkTerrain(truncated, component));
    // A rejected load must leave the component untouched, not zero-filled and reported as loaded.
    EXPECT_TRUE(component.heightmap.empty());

    SparkEditor::TerrainEditor reload;
    EXPECT_FALSE(reload.LoadTerrain(truncated));
}

TEST(EditorSubsystemsReal_HostileTerrainHeaderDoesNotSizeAnAllocation)
{
    ScratchDir scratch("terrain_hostile");
    const std::string path = scratch.File("hostile.sparkterrain");

    namespace Format = Spark::Graphics::SparkTerrain;
    {
        std::ofstream out(path, std::ios::binary);
        const uint32_t magic = Format::kMagic;
        const uint32_t version = Format::kVersion;
        const uint32_t nameLen = 0;
        const float size = 100.0f;
        const float pos[3] = {0.0f, 0.0f, 0.0f};
        const int32_t lodLevels = 1;
        const float lodBias = 1.0f;
        const uint8_t collider = 1;
        const int32_t huge = 1 << 30; // 1,073,741,824 samples per side
        const float heightParams[3] = {1.0f, 0.0f, 1.0f};

        out.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
        out.write(reinterpret_cast<const char*>(&version), sizeof(version));
        out.write(reinterpret_cast<const char*>(&nameLen), sizeof(nameLen));
        out.write(reinterpret_cast<const char*>(&size), sizeof(size));
        out.write(reinterpret_cast<const char*>(pos), sizeof(pos));
        out.write(reinterpret_cast<const char*>(&lodLevels), sizeof(lodLevels));
        out.write(reinterpret_cast<const char*>(&lodBias), sizeof(lodBias));
        out.write(reinterpret_cast<const char*>(&collider), sizeof(collider));
        out.write(reinterpret_cast<const char*>(&huge), sizeof(huge));
        out.write(reinterpret_cast<const char*>(&huge), sizeof(huge));
        out.write(reinterpret_cast<const char*>(heightParams), sizeof(heightParams));
    }

    TerrainComponent component;
    EXPECT_FALSE(Spark::Graphics::TerrainRenderer::LoadSparkTerrain(path, component));
    EXPECT_TRUE(component.heightmap.empty());

    SparkEditor::TerrainEditor editor;
    EXPECT_FALSE(editor.LoadTerrain(path));
}

TEST(EditorSubsystemsReal_TerrainSaveAndLoadShareOneDefaultLocation)
{
    const std::string defaultPath = SparkEditor::TerrainEditor::DefaultTerrainPath("My Terrain");
    EXPECT_STR_CONTAINS(defaultPath, "Assets/Terrains/");
    EXPECT_STR_CONTAINS(defaultPath, ".sparkterrain");
    // Illegal filename characters are replaced rather than producing an unreachable nested path.
    EXPECT_EQ(SparkEditor::TerrainEditor::DefaultTerrainPath("a/b"), std::string("Assets/Terrains/a_b.sparkterrain"));
}

TEST(EditorSubsystemsReal_TerrainSaveCreatesItsParentDirectory)
{
    ScratchDir scratch("terrain_mkdir");
    const std::string nested = (scratch.Path() / "Assets" / "Terrains" / "nested.sparkterrain").string();

    SparkEditor::TerrainEditor editor;
    AuthorTerrain(editor, 5);
    ASSERT_TRUE(editor.SaveTerrain(nested));
    EXPECT_TRUE(std::filesystem::exists(nested));
    EXPECT_EQ(editor.GetTerrainFilePath(), nested);
}

// ============================================================================
// SyncToEngine against a World whose terrain entity carries no engine Transform
// ============================================================================

namespace
{
    /// Install a static World on the process-wide EngineContext for one test and
    /// put the previous pointer back on every exit path (ASSERT_* throws).
    /// SetWorld(nullptr) erases the registration, so a null previous pointer is
    /// restored exactly too.
    class ScopedContextWorld
    {
      public:
        ScopedContextWorld()
        {
            if (!EngineContext::Get())
                EngineContext::SetOwned(std::make_unique<EngineContext>());
            m_ctx = EngineContext::Get();
            m_previous = m_ctx->GetWorld();
            m_ctx->SetWorld(&Instance());
        }
        ~ScopedContextWorld()
        {
            Instance().GetRegistry().clear();
            m_ctx->SetWorld(m_previous);
        }
        static ::World& Instance()
        {
            static ::World s_world; // Static: the context must never point at a dead frame object.
            return s_world;
        }

      private:
        EngineContext* m_ctx = nullptr;
        ::World* m_previous = nullptr;
    };

    size_t CountTerrainEntities(entt::registry& reg)
    {
        size_t count = 0;
        for ([[maybe_unused]] auto entity : reg.view<TerrainComponent>())
            ++count;
        return count;
    }
} // namespace

TEST(EditorSubsystemsReal_TerrainSyncAttachesTheEngineTransformToAnAdoptedEntity)
{
    // The editor adopts the first entity carrying a TerrainComponent from the
    // process-wide World. Two defects met here: inside namespace SparkEditor
    // the unqualified "Transform" resolved to the scene-file
    // SparkEditor::Transform, so the entity never carried the engine
    // ::Transform the terrain systems read (TerrainSystem views
    // <TerrainComponent, Transform>, so it never visited the editor's entity);
    // and an adopted entity without the expected component tripped entt's
    // "Set does not contain entity" assertion (ASan lane, shuffled order,
    // SparkTests aborted with SIGABRT). A TerrainComponent without a Transform
    // is a legitimate runtime state: the reflected scene serializer attaches
    // only the components a scene lists.
    ScopedContextWorld scope;
    auto& reg = ScopedContextWorld::Instance().GetRegistry();
    const entt::entity bare = reg.create();
    reg.emplace<TerrainComponent>(bare);
    ASSERT_FALSE(reg.any_of<::Transform>(bare));

    // CreateNewTerrain runs UpdateTerrainMesh, which is the first SyncToEngine:
    // that call must adopt the bare entity and attach the engine transform.
    SparkEditor::TerrainEditor editor;
    editor.CreateNewTerrain(250.0f, 5, XMFLOAT3{1.0f, 2.0f, 3.0f});
    ASSERT_TRUE(reg.valid(bare));
    ASSERT_TRUE(reg.any_of<::Transform>(bare));
    EXPECT_EQ(reg.get<::Transform>(bare).position.x, 1.0f);
    EXPECT_EQ(reg.get<::Transform>(bare).position.y, 2.0f);
    EXPECT_EQ(reg.get<::Transform>(bare).position.z, 3.0f);
    EXPECT_EQ(reg.get<TerrainComponent>(bare).heightmapResolution, 5);
    EXPECT_TRUE(reg.get<TerrainComponent>(bare).dirty);
    EXPECT_EQ(CountTerrainEntities(reg), 1u);

    // A later sync updates the same transform in place and still adopts
    // rather than creating a second terrain entity.
    editor.GetCurrentTerrain()->position = XMFLOAT3{4.0f, 5.0f, 6.0f};
    editor.SyncToEngine();
    EXPECT_EQ(reg.get<::Transform>(bare).position.x, 4.0f);
    EXPECT_EQ(reg.get<::Transform>(bare).position.z, 6.0f);
    EXPECT_EQ(CountTerrainEntities(reg), 1u);
}

// ============================================================================
// PrefabManager against a real SceneFile
// ============================================================================

namespace
{
    const SparkEditor::SceneObject* FindObject(const SparkEditor::SceneFile& scene, uint64_t id)
    {
        auto it = std::find_if(scene.objects.begin(), scene.objects.end(), [id](const SparkEditor::SceneObject& obj)
                               { return static_cast<uint64_t>(obj.id) == id; });
        return it == scene.objects.end() ? nullptr : &(*it);
    }
} // namespace

TEST(EditorSubsystemsReal_PrefabInstantiationMaterializesRotationAndComponents)
{
    SparkEditor::SceneFile scene;
    SparkEditor::PrefabManager manager;
    ASSERT_TRUE(manager.Initialize());
    manager.SetScene(&scene);

    SparkEditor::PrefabAsset* player = manager.GetPrefab("FPS Player");
    ASSERT_TRUE(player != nullptr);
    SparkEditor::SerializedComponent transform;
    transform.typeName = "Transform";
    transform.properties["position"] = XMFLOAT3{1.0f, 2.0f, 3.0f};
    transform.properties["rotation"] = XMFLOAT4{0.0f, 0.7071f, 0.0f, 0.7071f};
    transform.properties["scale"] = XMFLOAT3{2.0f, 2.0f, 2.0f};
    player->AddComponent(transform);

    const uint64_t entityId = manager.InstantiatePrefab("FPS Player");
    ASSERT_TRUE(entityId != 0);

    const SparkEditor::SceneObject* obj = FindObject(scene, entityId);
    ASSERT_TRUE(obj != nullptr);
    EXPECT_NEAR(obj->transform.position.y, 2.0f, 0.001f);
    EXPECT_NEAR(obj->transform.scale.x, 2.0f, 0.001f);
    // Rotation was silently dropped before; a prefab authored facing sideways instantiated facing forward.
    EXPECT_NEAR(obj->transform.rotation.y, 0.7071f, 0.001f);
    EXPECT_NEAR(obj->transform.rotation.w, 0.7071f, 0.001f);

    // Camera, RigidBody and Collider must reach the instance, not just Transform.
    EXPECT_TRUE(std::find(obj->componentTypes.begin(), obj->componentTypes.end(), SparkEditor::ComponentType::CAMERA) !=
                obj->componentTypes.end());
    EXPECT_TRUE(std::find(obj->componentTypes.begin(), obj->componentTypes.end(),
                          SparkEditor::ComponentType::RIGID_BODY) != obj->componentTypes.end());
    EXPECT_TRUE(std::find(obj->componentTypes.begin(), obj->componentTypes.end(),
                          SparkEditor::ComponentType::COLLIDER) != obj->componentTypes.end());
}

TEST(EditorSubsystemsReal_ApplyPrefabToInstancesWritesTheScene)
{
    SparkEditor::SceneFile scene;
    SparkEditor::PrefabManager manager;
    ASSERT_TRUE(manager.Initialize());
    manager.SetScene(&scene);

    const uint64_t entityId = manager.InstantiatePrefab("Point Light");
    ASSERT_TRUE(entityId != 0);

    // Move the template, then apply.
    SparkEditor::PrefabAsset* light = manager.GetPrefab("Point Light");
    ASSERT_TRUE(light != nullptr);
    SparkEditor::SerializedComponent transform;
    transform.typeName = "Transform";
    transform.properties["position"] = XMFLOAT3{9.0f, 9.0f, 9.0f};
    transform.properties["rotation"] = XMFLOAT4{0.0f, 0.0f, 0.0f, 1.0f};
    transform.properties["scale"] = XMFLOAT3{1.0f, 1.0f, 1.0f};
    light->AddComponent(transform);

    EXPECT_EQ(manager.ApplyPrefabToInstances("Point Light"), 1);

    const SparkEditor::SceneObject* obj = FindObject(scene, entityId);
    ASSERT_TRUE(obj != nullptr);
    EXPECT_NEAR(obj->transform.position.x, 9.0f, 0.001f);
    EXPECT_NEAR(obj->transform.position.z, 9.0f, 0.001f);
}

TEST(EditorSubsystemsReal_ApplyPrefabReportsZeroWhenItCannotWriteAnything)
{
    SparkEditor::PrefabManager manager;
    ASSERT_TRUE(manager.Initialize());

    // No scene is set, so nothing can be updated. The old code still counted and logged instances.
    manager.RegisterInstance(4242, "Point Light");
    EXPECT_EQ(manager.ApplyPrefabToInstances("Point Light"), 0);

    // A tracked instance whose scene object is gone is not an update either.
    SparkEditor::SceneFile scene;
    manager.SetScene(&scene);
    EXPECT_EQ(manager.ApplyPrefabToInstances("Point Light"), 0);
}

TEST(EditorSubsystemsReal_ApplyPrefabRespectsInstanceOverridesAndRevertClearsThem)
{
    SparkEditor::SceneFile scene;
    SparkEditor::PrefabManager manager;
    ASSERT_TRUE(manager.Initialize());
    manager.SetScene(&scene);

    const uint64_t entityId = manager.InstantiatePrefab("Point Light");
    ASSERT_TRUE(entityId != 0);
    ASSERT_TRUE(manager.SetInstanceOverride(entityId, "Transform", "position", XMFLOAT3{5.0f, 5.0f, 5.0f}));

    auto* obj = const_cast<SparkEditor::SceneObject*>(FindObject(scene, entityId));
    ASSERT_TRUE(obj != nullptr);
    obj->transform.position = {5.0f, 5.0f, 5.0f};

    SparkEditor::PrefabAsset* light = manager.GetPrefab("Point Light");
    ASSERT_TRUE(light != nullptr);
    SparkEditor::SerializedComponent transform;
    transform.typeName = "Transform";
    transform.properties["position"] = XMFLOAT3{1.0f, 1.0f, 1.0f};
    transform.properties["scale"] = XMFLOAT3{3.0f, 3.0f, 3.0f};
    light->AddComponent(transform);

    EXPECT_EQ(manager.ApplyPrefabToInstances("Point Light"), 1);
    obj = const_cast<SparkEditor::SceneObject*>(FindObject(scene, entityId));
    ASSERT_TRUE(obj != nullptr);
    EXPECT_NEAR(obj->transform.position.x, 5.0f, 0.001f); // overridden, kept
    EXPECT_NEAR(obj->transform.scale.x, 3.0f, 0.001f);    // not overridden, synced

    EXPECT_TRUE(manager.RevertProperty(entityId, "Transform", "position"));
    obj = const_cast<SparkEditor::SceneObject*>(FindObject(scene, entityId));
    ASSERT_TRUE(obj != nullptr);
    EXPECT_NEAR(obj->transform.position.x, 1.0f, 0.001f);

    EXPECT_TRUE(manager.RevertInstance(entityId));
    EXPECT_FALSE(manager.RevertInstance(999999));
}

TEST(EditorSubsystemsReal_RevertInstanceClearsRecordedOverridesSoApplyWritesAgain)
{
    SparkEditor::SceneFile scene;
    SparkEditor::PrefabManager manager;
    ASSERT_TRUE(manager.Initialize());
    manager.SetScene(&scene);

    const uint64_t entityId = manager.InstantiatePrefab("Point Light");
    ASSERT_TRUE(entityId != 0);

    SparkEditor::PrefabAsset* light = manager.GetPrefab("Point Light");
    ASSERT_TRUE(light != nullptr);
    SparkEditor::SerializedComponent transform;
    transform.typeName = "Transform";
    transform.properties["position"] = XMFLOAT3{1.0f, 1.0f, 1.0f};
    light->AddComponent(transform);

    // Record an override and give the scene object the overridden value.
    ASSERT_TRUE(manager.SetInstanceOverride(entityId, "Transform", "position", XMFLOAT3{5.0f, 5.0f, 5.0f}));
    auto* obj = const_cast<SparkEditor::SceneObject*>(FindObject(scene, entityId));
    ASSERT_TRUE(obj != nullptr);
    obj->transform.position = {5.0f, 5.0f, 5.0f};

    const auto overrideCount = [&manager](uint64_t id) -> std::size_t
    {
        for (const auto& instance : manager.GetInstances("Point Light"))
        {
            if (instance.entityId == id)
                return instance.overrides.size();
        }
        return 0u;
    };
    ASSERT_TRUE(overrideCount(entityId) == 1u);

    // Half one: a recorded override survives a prefab apply, value and record both.
    EXPECT_EQ(manager.ApplyPrefabToInstances("Point Light"), 1);
    obj = const_cast<SparkEditor::SceneObject*>(FindObject(scene, entityId));
    ASSERT_TRUE(obj != nullptr);
    EXPECT_NEAR(obj->transform.position.x, 5.0f, 0.001f);
    EXPECT_TRUE(overrideCount(entityId) == 1u);

    // Half two: RevertInstance must actually clear the record, not merely return
    // true. A bool alone cannot distinguish a real revert from a no-op, and a
    // stale override would go on shielding the property from every later apply.
    EXPECT_TRUE(manager.RevertInstance(entityId));
    EXPECT_TRUE(overrideCount(entityId) == 0u);

    // With the record gone, the next apply writes the prefab's value through.
    obj = const_cast<SparkEditor::SceneObject*>(FindObject(scene, entityId));
    ASSERT_TRUE(obj != nullptr);
    obj->transform.position = {9.0f, 9.0f, 9.0f};
    EXPECT_EQ(manager.ApplyPrefabToInstances("Point Light"), 1);
    obj = const_cast<SparkEditor::SceneObject*>(FindObject(scene, entityId));
    ASSERT_TRUE(obj != nullptr);
    EXPECT_NEAR(obj->transform.position.x, 1.0f, 0.001f);
}

TEST(EditorSubsystemsReal_PrefabSaveLoadRoundTripUsesTheRealAsset)
{
    ScratchDir scratch("prefab_io");

    SparkEditor::PrefabManager manager;
    ASSERT_TRUE(manager.Initialize());
    ASSERT_TRUE(manager.SavePrefab("FPS Player", scratch.Path().string()));

    SparkEditor::PrefabManager loader;
    SparkEditor::PrefabAsset* loaded = loader.LoadPrefab(scratch.File("FPS Player.sparkprefab"));
    ASSERT_TRUE(loaded != nullptr);
    EXPECT_EQ(loaded->GetName(), std::string("FPS Player"));
    EXPECT_TRUE(loaded->GetComponent("Transform") != nullptr);
}

// ============================================================================
// PerformanceProfiler frame clock
// ============================================================================

TEST(EditorSubsystemsReal_ProfilerReportsNoSampleBeforeAnyFrame)
{
    SparkEditor::PerformanceProfiler profiler;
    ASSERT_TRUE(profiler.Initialize());
    EXPECT_FALSE(profiler.HasFrameSample());
    EXPECT_NEAR(profiler.GetAverageFrameTimeMs(), 0.0f, 0.0001f);
}

TEST(EditorSubsystemsReal_ProfilerFrameTimeTracksTheRealDelta)
{
    SparkEditor::PerformanceProfiler profiler;
    ASSERT_TRUE(profiler.Initialize());

    // 20 ms per frame.
    for (int i = 0; i < 8; ++i)
        profiler.Update(0.020f);

    EXPECT_TRUE(profiler.HasFrameSample());

    const SparkEditor::FrameProfileData* frame = profiler.GetCurrentFrame();
    ASSERT_TRUE(frame != nullptr);
    // Before the fix frameTime was averaged out of a history seeded from itself, so it was always 0.
    EXPECT_NEAR(frame->frameTime, 20.0f, 0.01f);
    EXPECT_NEAR(frame->fps, 50.0f, 0.1f);
    EXPECT_NEAR(profiler.GetAverageFrameTimeMs(), 20.0f, 0.01f);
    EXPECT_NEAR(profiler.GetP50FrameTimeMs(), 20.0f, 0.01f);
    EXPECT_NEAR(profiler.GetP95FrameTimeMs(), 20.0f, 0.01f);
    EXPECT_NEAR(profiler.GetP99FrameTimeMs(), 20.0f, 0.01f);

    profiler.Shutdown();
}

TEST(EditorSubsystemsReal_MaterialSave_CreatesMissingParent)
{
    ScratchDir scratch("material_first_save");
    MaterialWorkingDirectoryGuard workingDirectory(scratch.Path());
    SparkEditor::MaterialEditorPanel panel;
    panel.CreateMaterial("FirstMaterial", "Shaders/QA.hlsl");
    const auto destination = scratch.Path() / "Assets" / "Materials" / "FirstMaterial.spkmat";
    EXPECT_FALSE(std::filesystem::exists(destination.parent_path()));
    ASSERT_TRUE(panel.SaveMaterial());
    EXPECT_TRUE(std::filesystem::is_regular_file(destination));
    EXPECT_TRUE(std::filesystem::file_size(destination) > 0);
    EXPECT_FALSE(panel.HasUnsavedChanges());
}

TEST(EditorSubsystemsReal_MaterialSave_ParentFileFailureRetainsDirtyState)
{
    ScratchDir scratch("material_parent_file");
    MaterialWorkingDirectoryGuard workingDirectory(scratch.Path());
    const auto blocker = scratch.Path() / "Assets";
    ASSERT_TRUE(Spark::FileUtils::WriteTextFile(blocker.string(), "existing user asset"));
    SparkEditor::MaterialEditorPanel panel;
    panel.CreateMaterial("FirstMaterial", "Shaders/QA.hlsl");
    EXPECT_FALSE(panel.SaveMaterial());
    EXPECT_TRUE(panel.HasUnsavedChanges());
    EXPECT_EQ(Spark::FileUtils::ReadTextFile(blocker.string()).value_or(""), std::string("existing user asset"));
}

TEST(EditorSubsystemsReal_MaterialSave_DestinationDirectoryFailureRetainsDirtyState)
{
    ScratchDir scratch("material_destination_directory");
    MaterialWorkingDirectoryGuard workingDirectory(scratch.Path());
    const auto destination = scratch.Path() / "Assets" / "Materials" / "FirstMaterial.spkmat";
    ASSERT_TRUE(std::filesystem::create_directories(destination));
    SparkEditor::MaterialEditorPanel panel;
    panel.CreateMaterial("FirstMaterial", "Shaders/QA.hlsl");
    EXPECT_FALSE(panel.SaveMaterial());
    EXPECT_TRUE(panel.HasUnsavedChanges());
    EXPECT_TRUE(std::filesystem::is_directory(destination));
}

TEST(EditorSubsystemsReal_MaterialSave_UsesNativeUtf8Path)
{
    ScratchDir scratch("material_utf8");
    MaterialWorkingDirectoryGuard workingDirectory(scratch.Path());
    const auto filename = std::filesystem::path(u8"塗料");
    const auto utf8Name = Spark::FileUtils::TryPathToUtf8(filename);
    ASSERT_TRUE(utf8Name.has_value());
    const auto destination = scratch.Path() / "Assets" / "Materials" / std::filesystem::path(u8"塗料.spkmat");
    SparkEditor::MaterialEditorPanel panel;
    panel.CreateMaterial(*utf8Name, "Shaders/QA.hlsl");
    ASSERT_TRUE(panel.SaveMaterial());
    EXPECT_TRUE(std::filesystem::is_regular_file(destination));
    EXPECT_FALSE(panel.HasUnsavedChanges());
}

TEST(EditorSubsystemsReal_MaterialSave_ReopenPreservesAuthoredValues)
{
    ScratchDir scratch("material_reopen");
    MaterialWorkingDirectoryGuard workingDirectory(scratch.Path());
    const std::string path = scratch.File("Authored.spkmat");
    const std::string authored =
        "name: Authored Material\nshader: Shaders/Custom Shader.hlsl\n"
        "param roughness float 0.248\nparam Vector2 float2 2 3\n"
        "param Vector3 float3 4 5 6\nparam Vector4 float4 7 8 9 10\n"
        "param Tint color 0.1 0.2 0.3 0.4\nparam Count int 17\nparam Enabled bool true\n"
        "param Diffuse texture2d Textures/Color Map.png\nparam Environment texturecube Textures/Sky Cube.dds\n"
        "param Matrix matrix4x4 1 0 0 0 0 2 0 0 0 0 3 0 4 5 6 1\n"
        "texture_slot Albedo 3 Textures/Color Map.png 2 3 0.25 0.5\n"
        "blend_mode 2\ncull_mode 1\ndepth_write false\ndepth_test true\n"
        "cast_shadows false\nreceive_shadows true\nalpha_clip 0.33\nrender_queue 2500\n";
    ASSERT_TRUE(Spark::FileUtils::WriteTextFile(path, authored));
    SparkEditor::MaterialEditorPanel panel;
    panel.OpenMaterial(path);
    EXPECT_FALSE(panel.HasUnsavedChanges());
    ASSERT_TRUE(panel.SaveMaterial());
    const std::string saved = Spark::FileUtils::ReadTextFile(path).value_or("");
    for (const std::string& line : {"name: Authored Material",
                                    "shader: Shaders/Custom Shader.hlsl",
                                    "param roughness float 0.248",
                                    "param Vector2 float2 2 3",
                                    "param Vector3 float3 4 5 6",
                                    "param Vector4 float4 7 8 9 10",
                                    "param Tint color 0.1 0.2 0.3 0.4",
                                    "param Count int 17",
                                    "param Enabled bool true",
                                    "param Diffuse texture2d Textures/Color Map.png",
                                    "param Environment texturecube Textures/Sky Cube.dds",
                                    "param Matrix matrix4x4 1 0 0 0 0 2 0 0 0 0 3 0 4 5 6 1",
                                    "texture_slot Albedo 3 Textures/Color Map.png 2 3 0.25 0.5",
                                    "blend_mode 2",
                                    "cull_mode 1",
                                    "depth_write false",
                                    "depth_test true",
                                    "cast_shadows false",
                                    "receive_shadows true",
                                    "alpha_clip 0.33",
                                    "render_queue 2500"})
    {
        EXPECT_TRUE(saved.find(line) != std::string::npos);
    }
}

TEST(EditorSubsystemsReal_MaterialSave_MissingLoadRetainsSelectionAndDirtyState)
{
    ScratchDir scratch("material_missing_load");
    MaterialWorkingDirectoryGuard workingDirectory(scratch.Path());
    SparkEditor::MaterialEditorPanel panel;
    panel.CreateMaterial("Existing", "Shaders/QA.hlsl");
    panel.OpenMaterial(scratch.File("Missing.spkmat"));
    EXPECT_TRUE(panel.HasUnsavedChanges());
    ASSERT_TRUE(panel.SaveMaterial());
    EXPECT_TRUE(std::filesystem::is_regular_file(scratch.Path() / "Assets/Materials/Existing.spkmat"));
    EXPECT_FALSE(std::filesystem::exists(scratch.Path() / "Missing.spkmat"));
}

TEST(EditorSubsystemsReal_MaterialSave_MalformedLoadRetainsAssetAndDirtyState)
{
    ScratchDir scratch("material_malformed_load");
    MaterialWorkingDirectoryGuard workingDirectory(scratch.Path());
    const std::string path = scratch.File("Malformed.spkmat");
    const std::string invalid = "name: Invalid\nshader: Shaders/QA.hlsl\nparam roughness float wrong\n";
    ASSERT_TRUE(Spark::FileUtils::WriteTextFile(path, invalid));
    SparkEditor::MaterialEditorPanel panel;
    panel.CreateMaterial("Existing", "Shaders/QA.hlsl");
    panel.OpenMaterial(path);
    EXPECT_TRUE(panel.HasUnsavedChanges());
    ASSERT_TRUE(panel.SaveMaterial());
    EXPECT_EQ(Spark::FileUtils::ReadTextFile(path).value_or(""), invalid);
    EXPECT_TRUE(std::filesystem::is_regular_file(scratch.Path() / "Assets/Materials/Existing.spkmat"));
}
