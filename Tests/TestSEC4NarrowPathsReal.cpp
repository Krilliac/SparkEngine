/**
 * @file TestSEC4NarrowPathsReal.cpp
 * @brief SEC4 narrow-path regressions: directory scans must survive non-ANSI names.
 *
 * On Windows, std::filesystem::path::string() converts through the active ANSI code
 * page and throws std::system_error ("No mapping for the Unicode character exists in
 * the target multi-byte code page") for a name that code page cannot spell. Each scan
 * below called it on every entry, so one file named U+96EA U+2603 (outside code
 * page 1252, 936 and every other common ANSI page) aborted the whole scan: the save
 * list came back empty, the script watcher threw out of PollChanges(), the asset
 * validator, pak writer and packager threw out of their loops, and so on.
 *
 * The fixtures put that name next to an ASCII one and check that the scan finishes,
 * keeps the ASCII entry, and either carries the non-ANSI name as UTF-8 or leaves it
 * out when its consumer can only reopen a narrow path. The RED case is Windows with a
 * non-UTF-8 ANSI code page (the default everywhere but opt-in UTF-8 systems); on
 * POSIX and on a UTF-8 code page path::string() cannot throw, so the tests pass
 * before and after the fix there and only pin the behaviour.
 *
 * Registered as the SEC4NarrowPathsReal CTest with a pinned count.
 */

#include "TestFramework.h"

#include "Core/AssetValidator.h"
#include "Core/SparkPak.h"
#include "Core/SparkPakWriter.h"
#include "Engine/Build/GamePackager.h"
#include "Engine/ECS/Components.h"
#include "Engine/Modding/VirtualFileSystem.h"
#include "Engine/SaveSystem/SaveSystem.h"
#include "Engine/Scripting/ScriptHotReload.h"
#include "Graphics/MaterialLoader.h"
#include "SceneManager/SceneManager.h"
#include "Utils/FileUtils.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{
    namespace sfs = std::filesystem;

    /// U+96EA U+2603 in UTF-8: no ANSI code page other than UTF-8 spells both.
    const std::string kSnowUtf8 = "\xE9\x9B\xAA\xE2\x98\x83";

    sfs::path SnowPath(const char* extension)
    {
        const std::string utf8 = kSnowUtf8 + extension;
        return sfs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()),
                                       reinterpret_cast<const char8_t*>(utf8.data()) + utf8.size()));
    }

    /// True when this process's narrow code page can spell the snow name, i.e. the
    /// narrow-only consumers are expected to see it.
    bool SnowIsNarrowSpellable()
    {
        return Spark::FileUtils::TryPathToNarrow(SnowPath(".txt")).has_value();
    }

    void WriteFile(const sfs::path& path, const std::string& content)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << content;
    }

    /// A fresh scratch directory holding one ASCII-named and one non-ANSI-named file.
    struct NarrowPathFixture
    {
        sfs::path root;
        std::string narrowRoot; ///< The root in the narrow form the std::string APIs take.

        NarrowPathFixture(const char* name, const char* extension, const std::string& content)
        {
            root = sfs::temp_directory_path() / (std::string("spark_sec4_narrow_") + name);
            std::error_code ec;
            sfs::remove_all(root, ec);
            sfs::create_directories(root, ec);
            narrowRoot = Spark::FileUtils::TryPathToNarrow(root).value_or(std::string());
            WriteFile(root / (std::string("plain") + extension), content);
            WriteFile(root / SnowPath(extension), content);
        }

        ~NarrowPathFixture()
        {
            std::error_code ec;
            sfs::remove_all(root, ec);
        }

        NarrowPathFixture(const NarrowPathFixture&) = delete;
        NarrowPathFixture& operator=(const NarrowPathFixture&) = delete;
    };
} // namespace

TEST(SEC4NarrowPath_HelpersSpellNonAnsiNamesWithoutThrowing)
{
    const sfs::path snow = SnowPath(".txt");

    const std::optional<std::string> utf8 = Spark::FileUtils::TryPathToUtf8(snow);
    ASSERT_TRUE(utf8.has_value());
    EXPECT_EQ(*utf8, kSnowUtf8 + ".txt");
    EXPECT_TRUE(Spark::FileUtils::PathFromUtf8(*utf8) == snow);

    const std::optional<std::string> ascii = Spark::FileUtils::TryPathToNarrow(sfs::path("plain.txt"));
    ASSERT_TRUE(ascii.has_value());
    EXPECT_EQ(*ascii, std::string("plain.txt"));

    // A narrow spelling is only offered when it reopens the same path.
    const std::optional<std::string> narrow = Spark::FileUtils::TryPathToNarrow(snow);
    if (narrow)
        EXPECT_TRUE(sfs::path(*narrow) == snow);
#ifdef _WIN32
    EXPECT_EQ(narrow.has_value(), GetACP() == CP_UTF8);
#else
    EXPECT_TRUE(narrow.has_value());
#endif
}

TEST(SEC4NarrowPath_SaveSlotListingSurvivesNonAnsiStrayFile)
{
    const auto dir = sfs::temp_directory_path() / "spark_sec4_narrow_saves";
    std::error_code ec;
    sfs::remove_all(dir, ec);
    const std::optional<std::string> narrowDir = Spark::FileUtils::TryPathToNarrow(dir);
    if (!narrowDir)
    {
        SKIP_TEST("temp directory has no narrow spelling");
    }

    Spark::SaveSystem& saveSystem = Spark::SaveSystem::GetInstance();
    ASSERT_TRUE(saveSystem.Initialize(*narrowDir));

    World source;
    source.AddComponent<Transform>(source.CreateEntity("sec4-owner"));
    Spark::SaveMetadata metadata;
    metadata.saveName = "SEC4 slot";
    ASSERT_TRUE(saveSystem.Save("alpha-slot", source, metadata));

    // A stray primary and a stray retained copy whose stems are not slot names at
    // all; before the fix either one threw out of the listing on Windows.
    WriteFile(dir / SnowPath(".spark_save"), "not a save");
    WriteFile(dir / SnowPath(".spark_save.bak"), "not a save either");

    std::vector<Spark::SaveMetadata> slots;
    EXPECT_NO_THROW(slots = saveSystem.GetSaveSlots());
    ASSERT_EQ(slots.size(), static_cast<size_t>(1));
    EXPECT_EQ(slots.front().slotName, std::string("alpha-slot"));
    EXPECT_EQ(slots.front().saveName, std::string("SEC4 slot"));

    sfs::remove_all(dir, ec);
}

TEST(SEC4NarrowPath_ScriptWatcherSkipsOnlyUnopenableScript)
{
    NarrowPathFixture fixture("scripts", ".as", "void main() {}\n");
    if (fixture.narrowRoot.empty())
    {
        SKIP_TEST("temp directory has no narrow spelling");
    }

    Spark::Scripting::ScriptHotReloadManager manager;
    manager.AddWatchDirectory(fixture.narrowRoot);
    EXPECT_NO_THROW(manager.Start());
    EXPECT_NO_THROW(manager.PollChanges());

    const int expected = SnowIsNarrowSpellable() ? 2 : 1;
    EXPECT_EQ(manager.GetWatchedFileCount(), expected);
    manager.Stop();
}

TEST(SEC4NarrowPath_AssetValidatorReportsNonAnsiAssetAsUtf8)
{
    NarrowPathFixture fixture("assets", ".txt", "");

    Spark::AssetValidator& validator = Spark::AssetValidator::GetInstance();
    validator.Initialize();

    Spark::ValidationReport report;
    EXPECT_NO_THROW(report = validator.ValidateDirectory(fixture.root));
    EXPECT_EQ(report.totalAssets, static_cast<uint32_t>(2));

    // Both files are empty, so the metadata rule reports each one by path.
    const bool snowReported =
        std::any_of(report.results.begin(), report.results.end(), [](const Spark::ValidationResult& result)
                    { return result.assetPath.find(kSnowUtf8 + ".txt") != std::string::npos; });
    EXPECT_TRUE(snowReported);
}

TEST(SEC4NarrowPath_PakWriterStoresNonAnsiNamesAsUtf8)
{
    NarrowPathFixture fixture("pak", ".txt", "payload");
    const sfs::path archive = sfs::temp_directory_path() / "spark_sec4_narrow_pak.spk";
    const std::optional<std::string> narrowArchive = Spark::FileUtils::TryPathToNarrow(archive);
    if (!narrowArchive)
    {
        SKIP_TEST("temp directory has no narrow spelling");
    }

    Spark::SparkPakWriter writer;
    EXPECT_NO_THROW(writer.AddDirectory(fixture.root));
    EXPECT_EQ(writer.GetFileCount(), static_cast<uint32_t>(2));
    ASSERT_TRUE(writer.Finalize(*narrowArchive));

    Spark::SparkPakReader reader;
    ASSERT_TRUE(reader.Open(*narrowArchive));
    EXPECT_TRUE(reader.Exists("plain.txt"));
    EXPECT_TRUE(reader.Exists(kSnowUtf8 + ".txt"));
    reader.Close();

    std::error_code ec;
    sfs::remove(archive, ec);
}

TEST(SEC4NarrowPath_VfsListingKeepsEveryReopenableName)
{
    NarrowPathFixture fixture("vfs", ".txt", "payload");
    if (fixture.narrowRoot.empty())
    {
        SKIP_TEST("temp directory has no narrow spelling");
    }

    Spark::LocalFileProvider provider(fixture.narrowRoot);
    std::vector<std::string> listed;
    EXPECT_NO_THROW(listed = provider.ListFiles("", ".txt"));

    const size_t expected = SnowIsNarrowSpellable() ? 2u : 1u;
    EXPECT_EQ(listed.size(), expected);
    EXPECT_TRUE(std::find(listed.begin(), listed.end(), std::string("plain.txt")) != listed.end());
    // Everything listed must reopen through the same provider.
    for (const std::string& name : listed)
        EXPECT_FALSE(provider.ReadFile(name).empty());
}

TEST(SEC4NarrowPath_GamePackagerCopiesNonAnsiAsset)
{
    NarrowPathFixture fixture("package_assets", ".txt", "payload");
    const sfs::path work = sfs::temp_directory_path() / "spark_sec4_narrow_package";
    std::error_code ec;
    sfs::remove_all(work, ec);
    sfs::create_directories(work, ec);
    WriteFile(work / "SEC4Game.exe", "exe");

    const std::optional<std::string> narrowWork = Spark::FileUtils::TryPathToNarrow(work);
    if (!narrowWork || fixture.narrowRoot.empty())
    {
        SKIP_TEST("temp directory has no narrow spelling");
    }

    auto& packager = Spark::Build::GamePackager::GetInstance();
    packager.Initialize();

    Spark::Build::PackageConfig config;
    config.projectName = "SEC4Game";
    config.executablePath = *narrowWork + "/SEC4Game.exe";
    config.outputDirectory = *narrowWork + "/out";
    config.assetDirectory = fixture.narrowRoot;
    config.dataDirectory.clear();

    Spark::Build::PackageResult result;
    EXPECT_NO_THROW(result = packager.Package(config));
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.filesCopied, static_cast<uint32_t>(3));
    EXPECT_TRUE(sfs::exists(work / "out" / "SEC4Game" / "Assets" / "plain.txt"));
    EXPECT_TRUE(sfs::exists(work / "out" / "SEC4Game" / "Assets" / SnowPath(".txt")));

    packager.Shutdown();
    sfs::remove_all(work, ec);
}

TEST(SEC4NarrowPath_MaterialScanSkipsOnlyUnopenableMaterial)
{
    NarrowPathFixture fixture("materials", ".sparkmat", "roughness = 0.5\n");
    if (fixture.narrowRoot.empty())
    {
        SKIP_TEST("temp directory has no narrow spelling");
    }
    // Only the non-ANSI material is left, so the result says whether it loaded.
    std::error_code ec;
    sfs::remove(fixture.root / "plain.sparkmat", ec);

    auto& loader = Spark::Graphics::MaterialLoader::GetInstance();
    bool anyLoaded = false;
    EXPECT_NO_THROW(anyLoaded = loader.LoadMaterialsFromDirectory(fixture.narrowRoot));
    EXPECT_EQ(anyLoaded, SnowIsNarrowSpellable());
    loader.Shutdown();
}

TEST(SEC4NarrowPath_SceneListingCarriesNonAnsiNamesAsUtf8)
{
    NarrowPathFixture fixture("scenes", ".scene", "");

    SceneManager scenes(nullptr, nullptr);
    std::vector<std::string> listed;
    EXPECT_NO_THROW(listed = scenes.GetAvailableScenes(fixture.root.wstring()));
    ASSERT_EQ(listed.size(), static_cast<size_t>(2));
    EXPECT_TRUE(std::find(listed.begin(), listed.end(), std::string("plain.scene")) != listed.end());
    EXPECT_TRUE(std::find(listed.begin(), listed.end(), kSnowUtf8 + ".scene") != listed.end());
}
