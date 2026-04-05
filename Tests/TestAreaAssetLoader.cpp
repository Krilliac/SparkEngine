// TestAreaAssetLoader.cpp - Tests for AreaAssetLoader and SceneManifest
// Validates manifest parsing, async area loading, and completion tracking

#include "TestFramework.h"

#include <cstdint>
#include <functional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{

    // ========================================================================
    // Standalone SceneManifest (matches engine SceneManifest.h)
    // ========================================================================

    struct SceneManifest
    {
        std::string name;
        std::vector<std::string> meshPaths;
        std::vector<std::string> texturePaths;
        std::vector<std::string> audioPaths;

        size_t TotalAssetCount() const { return meshPaths.size() + texturePaths.size() + audioPaths.size(); }

        std::vector<std::string> AllPaths() const
        {
            std::vector<std::string> all;
            all.reserve(TotalAssetCount());
            all.insert(all.end(), meshPaths.begin(), meshPaths.end());
            all.insert(all.end(), texturePaths.begin(), texturePaths.end());
            all.insert(all.end(), audioPaths.begin(), audioPaths.end());
            return all;
        }

        static SceneManifest ParseFromString(const std::string& content)
        {
            SceneManifest manifest;
            std::istringstream stream(content);
            std::string line;

            while (std::getline(stream, line))
            {
                auto start = line.find_first_not_of(" \t\r\n");
                if (start == std::string::npos)
                    continue;
                line = line.substr(start);

                if (line.empty() || line.rfind("//", 0) == 0)
                    continue;

                auto eq = line.find('=');
                if (eq == std::string::npos)
                    continue;

                std::string key = line.substr(0, eq);
                std::string value = line.substr(eq + 1);

                auto trimEnd = key.find_last_not_of(" \t");
                if (trimEnd != std::string::npos)
                    key = key.substr(0, trimEnd + 1);
                auto valStart = value.find_first_not_of(" \t");
                if (valStart != std::string::npos)
                    value = value.substr(valStart);
                auto valEnd = value.find_last_not_of(" \t\r\n");
                if (valEnd != std::string::npos)
                    value = value.substr(0, valEnd + 1);

                if (key == "name")
                    manifest.name = value;
                else if (key == "mesh")
                    manifest.meshPaths.push_back(value);
                else if (key == "texture")
                    manifest.texturePaths.push_back(value);
                else if (key == "audio")
                    manifest.audioPaths.push_back(value);
            }

            return manifest;
        }
    };

    // ========================================================================
    // Standalone AreaAssetLoader (matches engine AreaAssetLoader behavior)
    // ========================================================================

    using AreaID = uint32_t;
    using AreaLoadCompleteCallback = std::function<void(AreaID)>;

    struct AreaLoadState
    {
        SceneManifest manifest;
        uint32_t totalAssets = 0;
        uint32_t completedAssets = 0;
        uint32_t failedAssets = 0;
        AreaLoadCompleteCallback onComplete;
        bool loading = false;
    };

    class AreaAssetLoader
    {
      public:
        void Initialize() { m_initialized = true; }
        void Shutdown()
        {
            m_manifests.clear();
            m_loadStates.clear();
            m_initialized = false;
        }

        void SetManifest(AreaID areaId, SceneManifest manifest) { m_manifests[areaId] = std::move(manifest); }
        void RemoveManifest(AreaID areaId) { m_manifests.erase(areaId); }
        bool HasManifest(AreaID areaId) const { return m_manifests.count(areaId) > 0; }

        void BeginAreaLoad(AreaID areaId, AreaLoadCompleteCallback onComplete)
        {
            auto it = m_manifests.find(areaId);
            if (it == m_manifests.end() || it->second.TotalAssetCount() == 0)
            {
                if (onComplete)
                    onComplete(areaId);
                return;
            }

            AreaLoadState state;
            state.manifest = it->second;
            state.totalAssets = static_cast<uint32_t>(it->second.TotalAssetCount());
            state.completedAssets = 0;
            state.failedAssets = 0;
            state.onComplete = std::move(onComplete);
            state.loading = true;
            m_loadStates[areaId] = std::move(state);
        }

        void BeginAreaUnload(AreaID areaId, AreaLoadCompleteCallback onComplete)
        {
            m_loadStates.erase(areaId);
            if (onComplete)
                onComplete(areaId);
        }

        // Simulate completing N assets for an area (test helper)
        void SimulateAssetComplete(AreaID areaId, uint32_t count = 1)
        {
            auto it = m_loadStates.find(areaId);
            if (it == m_loadStates.end())
                return;
            it->second.completedAssets += count;
        }

        void SimulateAssetFailed(AreaID areaId, uint32_t count = 1)
        {
            auto it = m_loadStates.find(areaId);
            if (it == m_loadStates.end())
                return;
            it->second.failedAssets += count;
        }

        // Check and fire callbacks for completed areas
        void Update()
        {
            for (auto& [areaId, state] : m_loadStates)
            {
                if (state.loading && state.completedAssets + state.failedAssets >= state.totalAssets)
                {
                    state.loading = false;
                    if (state.onComplete)
                        state.onComplete(areaId);
                }
            }
        }

        bool IsAreaLoadComplete(AreaID areaId) const
        {
            auto it = m_loadStates.find(areaId);
            if (it == m_loadStates.end())
                return false;
            const auto& s = it->second;
            return !s.loading || (s.completedAssets + s.failedAssets >= s.totalAssets);
        }

        size_t GetLoadedAssetCount(AreaID areaId) const
        {
            auto it = m_loadStates.find(areaId);
            if (it == m_loadStates.end())
                return 0;
            return it->second.completedAssets;
        }

        float GetAreaLoadProgress(AreaID areaId) const
        {
            auto it = m_loadStates.find(areaId);
            if (it == m_loadStates.end())
                return 0.0f;
            const auto& s = it->second;
            if (s.totalAssets == 0)
                return 1.0f;
            return static_cast<float>(s.completedAssets + s.failedAssets) / static_cast<float>(s.totalAssets);
        }

      private:
        std::unordered_map<AreaID, SceneManifest> m_manifests;
        std::unordered_map<AreaID, AreaLoadState> m_loadStates;
        bool m_initialized = false;
    };

} // anonymous namespace

// ============================================================================
// SceneManifest Tests
// ============================================================================

TEST(SceneManifest_ParseFromString)
{
    std::string content = "// Town scene\n"
                          "name = TownSquare\n"
                          "mesh = models/fountain.mesh\n"
                          "mesh = models/houses.mesh\n"
                          "texture = textures/cobblestone.dds\n"
                          "audio = audio/town_ambience.wav\n";

    auto manifest = SceneManifest::ParseFromString(content);

    EXPECT_EQ(manifest.name, std::string("TownSquare"));
    EXPECT_EQ(manifest.meshPaths.size(), 2u);
    EXPECT_EQ(manifest.texturePaths.size(), 1u);
    EXPECT_EQ(manifest.audioPaths.size(), 1u);
    EXPECT_EQ(manifest.TotalAssetCount(), 4u);

    EXPECT_EQ(manifest.meshPaths[0], std::string("models/fountain.mesh"));
    EXPECT_EQ(manifest.meshPaths[1], std::string("models/houses.mesh"));
    EXPECT_EQ(manifest.texturePaths[0], std::string("textures/cobblestone.dds"));
    EXPECT_EQ(manifest.audioPaths[0], std::string("audio/town_ambience.wav"));
}

TEST(SceneManifest_ParseEmptyAndComments)
{
    std::string content = "// This is a comment\n"
                          "\n"
                          "   \n"
                          "// Another comment\n"
                          "name = EmptyScene\n";

    auto manifest = SceneManifest::ParseFromString(content);

    EXPECT_EQ(manifest.name, std::string("EmptyScene"));
    EXPECT_EQ(manifest.TotalAssetCount(), 0u);
}

TEST(SceneManifest_AllPaths)
{
    SceneManifest m;
    m.meshPaths = {"a.mesh", "b.mesh"};
    m.texturePaths = {"c.dds"};
    m.audioPaths = {"d.wav"};

    auto all = m.AllPaths();
    EXPECT_EQ(all.size(), 4u);
    EXPECT_EQ(all[0], std::string("a.mesh"));
    EXPECT_EQ(all[1], std::string("b.mesh"));
    EXPECT_EQ(all[2], std::string("c.dds"));
    EXPECT_EQ(all[3], std::string("d.wav"));
}

TEST(SceneManifest_ParseWhitespaceHandling)
{
    std::string content = "  name  =  Spaced Out  \n"
                          "  mesh  =  models/test.mesh  \n";

    auto manifest = SceneManifest::ParseFromString(content);
    EXPECT_EQ(manifest.name, std::string("Spaced Out"));
    EXPECT_EQ(manifest.meshPaths.size(), 1u);
    EXPECT_EQ(manifest.meshPaths[0], std::string("models/test.mesh"));
}

// ============================================================================
// AreaAssetLoader Tests
// ============================================================================

TEST(AreaAssetLoader_EmptyManifestCompletesImmediately)
{
    AreaAssetLoader loader;
    loader.Initialize();

    // No manifest registered — should complete immediately
    bool completed = false;
    loader.BeginAreaLoad(1, [&completed](AreaID) { completed = true; });

    EXPECT_TRUE(completed);
    loader.Shutdown();
}

TEST(AreaAssetLoader_EmptyAssetListCompletesImmediately)
{
    AreaAssetLoader loader;
    loader.Initialize();

    SceneManifest empty;
    empty.name = "EmptyArea";
    loader.SetManifest(1, empty);

    bool completed = false;
    loader.BeginAreaLoad(1, [&completed](AreaID) { completed = true; });

    EXPECT_TRUE(completed);
    loader.Shutdown();
}

TEST(AreaAssetLoader_ManifestManagement)
{
    AreaAssetLoader loader;
    loader.Initialize();

    EXPECT_FALSE(loader.HasManifest(1));

    SceneManifest m;
    m.name = "Test";
    m.meshPaths = {"a.mesh"};
    loader.SetManifest(1, m);
    EXPECT_TRUE(loader.HasManifest(1));

    loader.RemoveManifest(1);
    EXPECT_FALSE(loader.HasManifest(1));

    loader.Shutdown();
}

TEST(AreaAssetLoader_AsyncLoadCompletion)
{
    AreaAssetLoader loader;
    loader.Initialize();

    SceneManifest m;
    m.name = "Forest";
    m.meshPaths = {"trees.mesh", "rocks.mesh"};
    m.texturePaths = {"bark.dds"};
    loader.SetManifest(1, m);

    bool completed = false;
    loader.BeginAreaLoad(1, [&completed](AreaID) { completed = true; });

    // Not yet complete
    EXPECT_FALSE(completed);
    EXPECT_FALSE(loader.IsAreaLoadComplete(1));
    EXPECT_NEAR(loader.GetAreaLoadProgress(1), 0.0f, 0.001f);

    // Complete 1 of 3 assets
    loader.SimulateAssetComplete(1, 1);
    loader.Update();
    EXPECT_FALSE(completed);
    EXPECT_EQ(loader.GetLoadedAssetCount(1), 1u);
    EXPECT_NEAR(loader.GetAreaLoadProgress(1), 1.0f / 3.0f, 0.01f);

    // Complete remaining 2
    loader.SimulateAssetComplete(1, 2);
    loader.Update();
    EXPECT_TRUE(completed);
    EXPECT_TRUE(loader.IsAreaLoadComplete(1));
    EXPECT_EQ(loader.GetLoadedAssetCount(1), 3u);

    loader.Shutdown();
}

TEST(AreaAssetLoader_FailedAssetsStillComplete)
{
    AreaAssetLoader loader;
    loader.Initialize();

    SceneManifest m;
    m.name = "Ruins";
    m.meshPaths = {"good.mesh", "missing.mesh"};
    loader.SetManifest(1, m);

    bool completed = false;
    loader.BeginAreaLoad(1, [&completed](AreaID) { completed = true; });

    loader.SimulateAssetComplete(1, 1);
    loader.SimulateAssetFailed(1, 1);
    loader.Update();

    EXPECT_TRUE(completed);
    EXPECT_TRUE(loader.IsAreaLoadComplete(1));
    EXPECT_EQ(loader.GetLoadedAssetCount(1), 1u);

    loader.Shutdown();
}

TEST(AreaAssetLoader_UnloadReleasesState)
{
    AreaAssetLoader loader;
    loader.Initialize();

    SceneManifest m;
    m.meshPaths = {"a.mesh"};
    loader.SetManifest(1, m);

    loader.BeginAreaLoad(1, nullptr);
    loader.SimulateAssetComplete(1, 1);
    loader.Update();
    EXPECT_TRUE(loader.IsAreaLoadComplete(1));

    bool unloaded = false;
    loader.BeginAreaUnload(1, [&unloaded](AreaID) { unloaded = true; });
    EXPECT_TRUE(unloaded);

    // Load state cleared after unload
    EXPECT_EQ(loader.GetLoadedAssetCount(1), 0u);

    loader.Shutdown();
}

TEST(AreaAssetLoader_MultipleConcurrentAreaLoads)
{
    AreaAssetLoader loader;
    loader.Initialize();

    SceneManifest m1;
    m1.meshPaths = {"a.mesh"};
    SceneManifest m2;
    m2.meshPaths = {"b.mesh", "c.mesh"};

    loader.SetManifest(1, m1);
    loader.SetManifest(2, m2);

    bool done1 = false, done2 = false;
    loader.BeginAreaLoad(1, [&done1](AreaID) { done1 = true; });
    loader.BeginAreaLoad(2, [&done2](AreaID) { done2 = true; });

    EXPECT_FALSE(done1);
    EXPECT_FALSE(done2);

    loader.SimulateAssetComplete(1, 1);
    loader.Update();
    EXPECT_TRUE(done1);
    EXPECT_FALSE(done2);

    loader.SimulateAssetComplete(2, 2);
    loader.Update();
    EXPECT_TRUE(done2);

    loader.Shutdown();
}

TEST(AreaAssetLoader_ProgressTracking)
{
    AreaAssetLoader loader;
    loader.Initialize();

    SceneManifest m;
    m.meshPaths = {"a.mesh", "b.mesh", "c.mesh", "d.mesh"};
    loader.SetManifest(1, m);

    loader.BeginAreaLoad(1, nullptr);

    EXPECT_NEAR(loader.GetAreaLoadProgress(1), 0.0f, 0.001f);

    loader.SimulateAssetComplete(1, 2);
    // Note: Update not called yet, but progress reflects completedAssets
    EXPECT_NEAR(loader.GetAreaLoadProgress(1), 0.5f, 0.001f);

    loader.SimulateAssetComplete(1, 2);
    EXPECT_NEAR(loader.GetAreaLoadProgress(1), 1.0f, 0.001f);

    loader.Shutdown();
}
