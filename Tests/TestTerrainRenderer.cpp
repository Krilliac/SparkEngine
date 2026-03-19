// TestTerrainRenderer.cpp - Tests for terrain mesh building, dirty marking, and removal
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace TestTerrain
{

    using TerrainID = uint32_t;

    struct TerrainComponent
    {
        TerrainID id = 0;
        float heightScale = 1.0f;
        int resolution = 64;
        bool dirty = true;
    };

    struct TerrainMesh
    {
        TerrainID terrainID = 0;
        int vertexCount = 0;
        bool built = false;
    };

    struct TerrainRenderer
    {
        std::unordered_map<TerrainID, TerrainComponent> terrains;
        std::unordered_map<TerrainID, TerrainMesh> meshes;
        bool initialized = false;

        bool Initialize()
        {
            initialized = true;
            return true;
        }

        void Shutdown()
        {
            terrains.clear();
            meshes.clear();
            initialized = false;
        }

        bool AddTerrain(const TerrainComponent& comp)
        {
            if (!initialized)
                return false;
            terrains[comp.id] = comp;
            return true;
        }

        bool BuildMeshFromComponent(TerrainID id)
        {
            auto it = terrains.find(id);
            if (it == terrains.end())
                return false;

            auto& comp = it->second;
            int verts = (comp.resolution + 1) * (comp.resolution + 1);
            meshes[id] = {id, verts, true};
            comp.dirty = false;
            return true;
        }

        void MarkDirty(TerrainID id)
        {
            auto it = terrains.find(id);
            if (it != terrains.end())
                it->second.dirty = true;
        }

        bool RemoveTerrain(TerrainID id)
        {
            auto tIt = terrains.find(id);
            if (tIt == terrains.end())
                return false;
            terrains.erase(tIt);
            meshes.erase(id);
            return true;
        }

        int GetActiveTerrainCount() const { return static_cast<int>(terrains.size()); }

        bool IsDirty(TerrainID id) const
        {
            auto it = terrains.find(id);
            return it != terrains.end() && it->second.dirty;
        }
    };

} // namespace TestTerrain

// ============================================================================
// Initialization and Shutdown
// ============================================================================

TEST(TerrainRenderer_InitializeAndShutdown)
{
    TestTerrain::TerrainRenderer renderer;
    EXPECT_FALSE(renderer.initialized);
    EXPECT_TRUE(renderer.Initialize());
    EXPECT_TRUE(renderer.initialized);
    renderer.Shutdown();
    EXPECT_FALSE(renderer.initialized);
}

// ============================================================================
// Mesh Building and Dirty Tracking
// ============================================================================

TEST(TerrainRenderer_BuildMeshFromComponent)
{
    TestTerrain::TerrainRenderer renderer;
    renderer.Initialize();

    TestTerrain::TerrainComponent comp{1, 2.0f, 32, true};
    EXPECT_TRUE(renderer.AddTerrain(comp));
    EXPECT_TRUE(renderer.IsDirty(1));

    EXPECT_TRUE(renderer.BuildMeshFromComponent(1));
    EXPECT_FALSE(renderer.IsDirty(1));

    auto it = renderer.meshes.find(1);
    EXPECT_TRUE(it != renderer.meshes.end());
    EXPECT_EQ(it->second.vertexCount, 33 * 33);
    EXPECT_TRUE(it->second.built);

    renderer.Shutdown();
}

TEST(TerrainRenderer_MarkDirty)
{
    TestTerrain::TerrainRenderer renderer;
    renderer.Initialize();
    renderer.AddTerrain({1, 1.0f, 16, true});
    renderer.BuildMeshFromComponent(1);
    EXPECT_FALSE(renderer.IsDirty(1));

    renderer.MarkDirty(1);
    EXPECT_TRUE(renderer.IsDirty(1));

    renderer.Shutdown();
}

// ============================================================================
// Removal and Count
// ============================================================================

TEST(TerrainRenderer_RemoveTerrain)
{
    TestTerrain::TerrainRenderer renderer;
    renderer.Initialize();
    renderer.AddTerrain({1, 1.0f, 16, true});
    renderer.AddTerrain({2, 1.0f, 32, true});
    EXPECT_EQ(renderer.GetActiveTerrainCount(), 2);

    EXPECT_TRUE(renderer.RemoveTerrain(1));
    EXPECT_EQ(renderer.GetActiveTerrainCount(), 1);
    EXPECT_FALSE(renderer.RemoveTerrain(1));
    EXPECT_FALSE(renderer.RemoveTerrain(99));

    renderer.Shutdown();
}

TEST(TerrainRenderer_GetActiveTerrainCount)
{
    TestTerrain::TerrainRenderer renderer;
    renderer.Initialize();
    EXPECT_EQ(renderer.GetActiveTerrainCount(), 0);

    renderer.AddTerrain({1, 1.0f, 16, true});
    renderer.AddTerrain({2, 1.0f, 32, true});
    renderer.AddTerrain({3, 1.0f, 64, true});
    EXPECT_EQ(renderer.GetActiveTerrainCount(), 3);

    renderer.Shutdown();
    EXPECT_EQ(renderer.GetActiveTerrainCount(), 0);
}
