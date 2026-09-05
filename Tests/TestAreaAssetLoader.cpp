// TestAreaAssetLoader.cpp - Tests for AreaAssetLoader and SceneManifest
// Validates manifest parsing, async area loading, and completion tracking

#include "TestFramework.h"

#include "Engine/Streaming/AreaAssetLoader.h"
#include "Engine/Streaming/SceneManifest.h"

#include <cstdint>
#include <functional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{

    // The scene manifest under test is the shipped one. It used to be a local
    // copy 'matching' Engine/Streaming/SceneManifest.h, which stopped being true
    // the moment the real parser started dropping out-of-root asset paths: the
    // copy would have kept reporting the old behavior forever.
    using SceneManifest = Spark::Streaming::SceneManifest;

    // ========================================================================
    // ModelAreaAssetLoader — a local completion-accounting MODEL, not the shipped
    // loader.
    //
    // It exists because Engine/Streaming/AreaAssetLoader.cpp drives real I/O
    // through DirectStorageLoader, which these tests cannot stand up. Nothing it
    // asserts is evidence about the production loader, so it carries a name that
    // cannot be mistaken for one. The production behaviour that CAN be tested
    // without I/O — path containment at BeginAreaLoad — is covered against the
    // shipped class at the bottom of this file.
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

    class ModelAreaAssetLoader
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

TEST(SceneManifest_ParseDropsOutOfRootAssetPaths)
{
    // A .sparkscene is untrusted content (a shared project or a mod supplies it),
    // and every path it declares is opened verbatim by the streaming loaders.
    std::string content = "name = Hostile\n"
                          "mesh = ../../secrets/id_rsa\n"
                          "texture = /etc/passwd\n"
                          "audio = audio/ok.wav\n";

    auto manifest = SceneManifest::ParseFromString(content);

    EXPECT_EQ(manifest.name, std::string("Hostile"));
    EXPECT_EQ(manifest.meshPaths.size(), 0u);
    EXPECT_EQ(manifest.texturePaths.size(), 0u);
    EXPECT_EQ(manifest.audioPaths.size(), 1u);
    EXPECT_EQ(manifest.audioPaths[0], std::string("audio/ok.wav"));
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
// ModelAreaAssetLoader Tests — completion accounting only; see the note above the
// class. These assert nothing about Engine/Streaming/AreaAssetLoader.cpp.
// ============================================================================

TEST(AreaAssetLoader_EmptyManifestCompletesImmediately)
{
    ModelAreaAssetLoader loader;
    loader.Initialize();

    // No manifest registered — should complete immediately
    bool completed = false;
    loader.BeginAreaLoad(1, [&completed](AreaID) { completed = true; });

    EXPECT_TRUE(completed);
    loader.Shutdown();
}

TEST(AreaAssetLoader_EmptyAssetListCompletesImmediately)
{
    ModelAreaAssetLoader loader;
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
    ModelAreaAssetLoader loader;
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
    ModelAreaAssetLoader loader;
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
    ModelAreaAssetLoader loader;
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
    ModelAreaAssetLoader loader;
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
    ModelAreaAssetLoader loader;
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
    ModelAreaAssetLoader loader;
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

// ============================================================================
// Production AreaAssetLoader — path containment at the consumption point
// ============================================================================

TEST(AreaAssetLoader_ProductionDropsOutOfRootManifestPaths)
{
    // A SceneManifest can be built in code and handed to SetManifest without ever
    // passing through the parser, so the parser's own filter is not a gate. This
    // drives the SHIPPED loader: every declared path escapes the asset root, so
    // none may reach DirectStorageLoader and the area must still complete.
    Spark::Streaming::AreaAssetLoader loader;
    loader.Initialize();

    Spark::Streaming::SceneManifest manifest;
    manifest.name = "Hostile";
    manifest.meshPaths = {"../../../../Users/victim/.ssh/id_rsa"};
    manifest.texturePaths = {"C:/Windows/System32/config/SAM"};
    manifest.audioPaths = {"assets/logo.png:secret"};
    EXPECT_EQ(manifest.TotalAssetCount(), 3u);

    loader.SetManifest(7, manifest);
    EXPECT_TRUE(loader.HasManifest(7));

    bool completed = false;
    loader.BeginAreaLoad(7, [&completed](Spark::Streaming::AreaID) { completed = true; });

    // No request was submitted, so the callback has to have fired synchronously.
    EXPECT_TRUE(completed);
    EXPECT_EQ(loader.GetLoadedAssetCount(7), 0u);

    loader.Shutdown();
}
