/**
 * @file TestSceneSaveConfinedReal.cpp
 * @brief Real-class coverage for SceneManager::SaveSceneWithinRoot, the
 *        root-confined save behind the FPS `scene_save` console command.
 *
 * The attack these tests pin down: a path component below the trusted scene
 * root is (or is swapped to become) a symlink/junction pointing elsewhere. A
 * validate-then-write implementation can be redirected between the check and
 * the write; the confined save must refuse and never create a file outside
 * the root.
 */

#include "TestFramework.h"

#include "Graphics/GraphicsEngine.h"
#include "Input/InputManager.h"
#include "SceneManager/SceneManager.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

namespace fs = std::filesystem;

namespace
{
    // Directory link that an unprivileged test can create: a symlink where
    // allowed, otherwise (Windows without Developer Mode) an NTFS junction.
    bool MakeDirectoryLink(const fs::path& target, const fs::path& link)
    {
        std::error_code error;
        fs::create_directory_symlink(target, link, error);
        if (!error)
            return true;
#if defined(_WIN32)
        const std::wstring command =
            L"cmd /c mklink /J \"" + link.wstring() + L"\" \"" + target.wstring() + L"\" >nul 2>&1";
        return _wsystem(command.c_str()) == 0 && fs::exists(fs::symlink_status(link, error));
#else
        return false;
#endif
    }

    bool IsLink(const fs::path& path)
    {
        std::error_code error;
        const auto type = fs::symlink_status(path, error).type();
#if defined(_WIN32)
        if (type == fs::file_type::junction)
            return true;
#endif
        return type == fs::file_type::symlink;
    }

    // Removes the link itself, never the directory it points at.
    void RemoveDirectoryLink(const fs::path& link)
    {
        std::error_code error;
        fs::remove(link, error);
    }

    size_t CountEntries(const fs::path& directory)
    {
        std::error_code error;
        size_t count = 0;
        for (fs::recursive_directory_iterator it(directory, error), end; !error && it != end; it.increment(error))
            ++count;
        return count;
    }

    std::string ReadAll(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    // <temp>/spark_scene_save_<nonce>/{root/Levels, outside}
    class SaveSandbox
    {
      public:
        SaveSandbox()
        {
            std::error_code error;
            const auto temp = fs::temp_directory_path(error);
            const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
            m_base = temp / ("spark_scene_save_" + std::to_string(nonce));
            fs::create_directories(m_base / "root" / "Levels", error);
            fs::create_directories(m_base / "outside", error);
            m_ready = !error;
        }
        ~SaveSandbox()
        {
            // Directory links are only ever created directly under root/.
            // Unlink them first so remove_all cannot reach through one.
            std::error_code error;
            for (fs::directory_iterator it(Root(), error), end; !error && it != end; it.increment(error))
            {
                if (IsLink(it->path()))
                    RemoveDirectoryLink(it->path());
            }
            error.clear();
            if (m_base.filename().string().rfind("spark_scene_save_", 0) == 0)
                fs::remove_all(m_base, error);
        }
        bool Ready() const { return m_ready; }
        fs::path Root() const { return m_base / "root"; }
        fs::path Outside() const { return m_base / "outside"; }

      private:
        fs::path m_base;
        bool m_ready = false;
    };

    void PopulateScene(SceneManager& scene)
    {
        scene.GetMetadata().sceneName = "Confined save";
        scene.GetMetadata().author = "Playtester";

        SceneNode camera;
        camera.type = "Camera";
        camera.name = "Main Camera";
        camera.position = {1.0f, 2.0f, -20.0f};
        camera.properties["isMain"] = "true";
        camera.properties["nearPlane"] = "0.25";
        ASSERT_TRUE(scene.AddNode(camera) >= 0);

        SceneNode floor;
        floor.type = "plane";
        floor.name = "Painted Floor";
        floor.materialPath = "Assets/Materials/Terrain_Dirt.json";
        ASSERT_TRUE(scene.AddNode(floor) >= 0);
    }
} // namespace

TEST(SceneSaveConfined_WritesInsideRootAndRoundTrips)
{
    SaveSandbox sandbox;
    ASSERT_TRUE(sandbox.Ready());
    GraphicsEngine graphics;
    InputManager input;
    SceneManager scene(&graphics, &input);
    PopulateScene(scene);

    ASSERT_TRUE(scene.SaveSceneWithinRoot(sandbox.Root(), "Levels/arena.scene"));
    EXPECT_FALSE(scene.IsDirty());
    // Overwrite of an existing scene goes through the same path.
    ASSERT_TRUE(scene.SaveSceneWithinRoot(sandbox.Root(), fs::path("Levels") / "arena.scene"));

    const fs::path saved = sandbox.Root() / "Levels" / "arena.scene";
    EXPECT_EQ(CountEntries(sandbox.Root() / "Levels"), static_cast<size_t>(1)); // no temp files left
    EXPECT_EQ(CountEntries(sandbox.Outside()), static_cast<size_t>(0));

    SceneManager reloaded(&graphics, &input);
    ASSERT_TRUE(reloaded.LoadScene(saved.wstring()));
    ASSERT_EQ(reloaded.GetNodeCount(), 2);
    const SceneNode* camera = reloaded.GetNode(reloaded.FindNode("Main Camera"));
    const SceneNode* floor = reloaded.GetNode(reloaded.FindNode("Painted Floor"));
    ASSERT_TRUE(camera != nullptr);
    ASSERT_TRUE(floor != nullptr);
    EXPECT_EQ(camera->properties.at("isMain"), std::string("true"));
    EXPECT_EQ(camera->properties.at("nearPlane"), std::string("0.25"));
    EXPECT_EQ(floor->materialPath, std::string("Assets/Materials/Terrain_Dirt.json"));
    EXPECT_EQ(reloaded.GetMetadata().author, std::string("Playtester"));
}

TEST(SceneSaveConfined_RejectsEscapingAndMalformedPaths)
{
    SaveSandbox sandbox;
    ASSERT_TRUE(sandbox.Ready());
    GraphicsEngine graphics;
    InputManager input;
    SceneManager scene(&graphics, &input);
    PopulateScene(scene);

    const fs::path rejected[] = {
        fs::path(),
        fs::path("..") / "outside" / "escape.scene",
        fs::path("Levels") / ".." / ".." / "outside" / "escape.scene",
        fs::path(".") / "Levels" / "dot.scene",
        sandbox.Outside() / "absolute.scene",
        fs::path("Levels") / "wrong.txt",
        fs::path("Levels"),
        fs::path("Missing") / "nested.scene", // directories are never created
    };
    for (const auto& path : rejected)
        EXPECT_FALSE(scene.SaveSceneWithinRoot(sandbox.Root(), path));

    EXPECT_EQ(CountEntries(sandbox.Outside()), static_cast<size_t>(0));
    EXPECT_EQ(CountEntries(sandbox.Root()), static_cast<size_t>(1)); // only Levels/
}

TEST(SceneSaveConfined_RefusesLinkedDirectoryComponent)
{
    SaveSandbox sandbox;
    ASSERT_TRUE(sandbox.Ready());
    if (!MakeDirectoryLink(sandbox.Outside(), sandbox.Root() / "Escape"))
        SKIP_TEST("cannot create a directory symlink or junction on this host");
    ASSERT_TRUE(MakeDirectoryLink(sandbox.Root() / "Levels", sandbox.Root() / "Alias"));

    GraphicsEngine graphics;
    InputManager input;
    SceneManager scene(&graphics, &input);
    PopulateScene(scene);

    // A link that leaves the root is refused and writes nothing outside.
    EXPECT_FALSE(scene.SaveSceneWithinRoot(sandbox.Root(), fs::path("Escape") / "escape.scene"));
    EXPECT_EQ(CountEntries(sandbox.Outside()), static_cast<size_t>(0));

    // So is a link that stays inside it: no link is trusted as a component,
    // because its target can be changed after it is checked.
    EXPECT_FALSE(scene.SaveSceneWithinRoot(sandbox.Root(), fs::path("Alias") / "alias.scene"));
    EXPECT_EQ(CountEntries(sandbox.Root() / "Levels"), static_cast<size_t>(0));
}

TEST(SceneSaveConfined_RefusesLinkedDestinationFile)
{
    SaveSandbox sandbox;
    ASSERT_TRUE(sandbox.Ready());
    const fs::path victim = sandbox.Outside() / "victim.scene";
    {
        std::ofstream file(victim, std::ios::binary);
        file << "original";
    }
    std::error_code linkError;
    fs::create_symlink(victim, sandbox.Root() / "Levels" / "victim.scene", linkError);
    if (linkError)
        SKIP_TEST("cannot create a file symlink on this host");

    GraphicsEngine graphics;
    InputManager input;
    SceneManager scene(&graphics, &input);
    PopulateScene(scene);

    EXPECT_FALSE(scene.SaveSceneWithinRoot(sandbox.Root(), fs::path("Levels") / "victim.scene"));
    EXPECT_EQ(ReadAll(victim), std::string("original"));
    EXPECT_EQ(CountEntries(sandbox.Outside()), static_cast<size_t>(1));
}

// The TOCTOU attack itself: another thread keeps swapping Levels/ for a link
// to a directory outside the root while saves run. Every save must either land
// inside the root or fail; nothing may ever appear outside.
TEST(SceneSaveConfined_ComponentSwapRaceNeverWritesOutsideRoot)
{
    SaveSandbox sandbox;
    ASSERT_TRUE(sandbox.Ready());
    const fs::path levels = sandbox.Root() / "Levels";
    const fs::path parked = sandbox.Root() / "Parked";
    if (!MakeDirectoryLink(sandbox.Outside(), sandbox.Root() / "probe"))
        SKIP_TEST("cannot create a directory symlink or junction on this host");
    RemoveDirectoryLink(sandbox.Root() / "probe");

    GraphicsEngine graphics;
    InputManager input;
    SceneManager scene(&graphics, &input);
    PopulateScene(scene);

    std::atomic<bool> stop{false};
    std::atomic<int> swaps{0};
    std::thread attacker(
        [&]
        {
            while (!stop.load())
            {
                std::error_code error;
                fs::rename(levels, parked, error); // refused on Windows while a save holds Levels/
                if (error)
                {
                    std::this_thread::yield();
                    continue;
                }
                if (MakeDirectoryLink(sandbox.Outside(), levels))
                {
                    swaps.fetch_add(1);
                    std::this_thread::yield();
                    RemoveDirectoryLink(levels);
                }
                do
                {
                    error.clear();
                    fs::rename(parked, levels, error);
                } while (error && !stop.load());
            }
        });

    int attempts = 0;
    int successes = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline && (attempts < 400 || successes < 5 || swaps.load() < 5))
    {
        ++attempts;
        if (scene.SaveSceneWithinRoot(sandbox.Root(), fs::path("Levels") / "race.scene"))
            ++successes;
    }
    stop.store(true);
    attacker.join();
    // Restore the layout if the attacker stopped mid-swap.
    std::error_code error;
    if (!fs::exists(fs::symlink_status(levels, error)))
        fs::rename(parked, levels, error);

    EXPECT_EQ(CountEntries(sandbox.Outside()), static_cast<size_t>(0));
    EXPECT_GT(swaps.load(), 0);                                  // the attack actually ran
    EXPECT_GT(successes, 0);                                     // and saves still succeed between swaps
    EXPECT_TRUE(CountEntries(levels) <= static_cast<size_t>(1)); // no stray temp files
}
