/**
 * @file TestSceneManagerUnicodeReal.cpp
 * @brief Real-class coverage for Unicode scene persistence paths on Windows.
 */

#include "TestFramework.h"

#include "Graphics/GraphicsEngine.h"
#include "Input/InputManager.h"
#include "SceneManager/SceneManager.h"
#include "Utils/LocalFileCache.h"

#include <chrono>
#include <filesystem>
#include <string>

namespace
{
    std::filesystem::path MakeSceneDirectory(const char* name)
    {
        std::error_code error;
        const auto root = std::filesystem::temp_directory_path(error);
        if (error || root.empty())
            return {};

        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::string prefix = std::string("spark_scene_") + name + "_" + std::to_string(nonce) + "_";
        for (int attempt = 0; attempt < 64; ++attempt)
        {
            const auto directory = root / (prefix + std::to_string(attempt));
            error.clear();
            if (std::filesystem::create_directory(directory, error))
                return directory;
            if (error && error != std::errc::file_exists)
                return {};
        }
        return {};
    }

    void RemoveSceneDirectory(const std::filesystem::path& directory)
    {
        std::error_code error;
        const auto tempDirectory = std::filesystem::temp_directory_path(error);
        if (error || tempDirectory.empty())
            return;
        const auto tempRoot = std::filesystem::weakly_canonical(tempDirectory, error);
        if (error || std::filesystem::is_symlink(directory, error) || error)
            return;
        const auto resolved = std::filesystem::weakly_canonical(directory, error);
        const auto filename = resolved.filename().string();
        if (error || resolved.parent_path() != tempRoot || filename.rfind("spark_scene_", 0) != 0)
            return;
        std::filesystem::remove_all(resolved, error);
    }

    void PopulateScene(SceneManager& scene)
    {
        scene.GetMetadata().sceneName = "Unicode persistence";
        scene.GetMetadata().author = "Unicode tester";
        scene.GetMetadata().description = "Non-ASCII path regression";

        SceneNode node;
        node.type = "model";
        node.name = "UnicodeProp";
        node.position = {1.0f, 2.0f, 3.0f};
        node.modelPath = "Models/props/lantern.obj";
        node.materialPath = "Materials/painted metal.json";
        node.properties["displayName"] = "Unicode lantern";
        node.properties["owner"] = "Unicode tester";
        ASSERT_TRUE(scene.AddNode(node) >= 0);
    }
} // namespace

TEST(SceneManager_UnicodePathRoundTripsWithCacheAndAsciiControl)
{
#if !defined(SPARK_PLATFORM_WINDOWS)
    SKIP_TEST("Unicode user-profile path regression is Windows-specific");
    return;
#else
    const auto root = MakeSceneDirectory("unicode");
    const auto unicodePath = root / L"\x7528\x6237_\x0442\x0435\x0441_\U0001F642" / L"scene.json";
    const auto asciiPath = root / L"ascii" / L"scene.json";
    ASSERT_FALSE(root.empty());
    std::error_code error;
    std::filesystem::create_directories(unicodePath.parent_path(), error);
    std::filesystem::create_directories(asciiPath.parent_path(), error);

    GraphicsEngine graphics;
    InputManager input;
    SceneManager scene(&graphics, &input);
    PopulateScene(scene);

    ASSERT_TRUE(scene.SaveScene(unicodePath.wstring()));
    ASSERT_TRUE(std::filesystem::exists(unicodePath));

    Spark::LocalFileCache cache;
    SceneManager unicodeReload(&graphics, &input);
    unicodeReload.SetFileCache(&cache);
    ASSERT_TRUE(unicodeReload.LoadScene(unicodePath.wstring()));
    ASSERT_EQ(unicodeReload.GetNodeCount(), 1);
    EXPECT_EQ(unicodeReload.GetMetadata().author, "Unicode tester");
    ASSERT_TRUE(unicodeReload.GetNode(0) != nullptr);
    EXPECT_EQ(unicodeReload.GetNode(0)->materialPath, "Materials/painted metal.json");
    EXPECT_EQ(unicodeReload.GetNode(0)->properties.at("displayName"), "Unicode lantern");
    EXPECT_EQ(unicodeReload.GetNode(0)->properties.at("owner"), "Unicode tester");

    ASSERT_TRUE(scene.SaveScene(asciiPath.wstring()));
    ASSERT_TRUE(std::filesystem::exists(asciiPath));
    SceneManager asciiReload(&graphics, &input);
    asciiReload.SetFileCache(&cache);
    ASSERT_TRUE(asciiReload.LoadScene(asciiPath.wstring()));
    EXPECT_EQ(asciiReload.GetNodeCount(), 1);
    EXPECT_EQ(asciiReload.GetMetadata().sceneName, "Unicode persistence");
    EXPECT_TRUE(cache.Contains(asciiPath.string()));

    std::filesystem::create_directories(root / "sub", error);
    ASSERT_FALSE(error);
    SceneManager traversalReload(&graphics, &input);
    EXPECT_FALSE(traversalReload.LoadScene((root / "sub" / ".." / "ascii" / "scene.json").wstring()));

    RemoveSceneDirectory(root);
#endif
}
