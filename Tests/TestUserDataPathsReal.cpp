/**
 * @file TestUserDataPathsReal.cpp
 * @brief Real Spark::UserPaths / EngineSettings settings-path resolution tests.
 *
 * An installed game under a read-only prefix (Program Files) cannot persist
 * settings or saves beside its binaries, and an upgrade or uninstall deletes
 * anything co-located with them. These tests exercise the production
 * Spark::UserPaths helpers and EngineSettings::FindSettingsPath directly.
 */

#include "TestFramework.h"

#include "Core/EngineSettings.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
    /** @brief RAII override of one environment variable, restored on scope exit. */
    class ScopedEnvironmentVariable
    {
      public:
        ScopedEnvironmentVariable(const char* name, const std::string& value) : m_name(name)
        {
            if (const char* existing = std::getenv(name))
            {
                m_hadValue = true;
                m_previous = existing;
            }
            Apply(value);
        }

        ~ScopedEnvironmentVariable() { Apply(m_hadValue ? m_previous : std::string{}); }

        ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
        ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

      private:
        void Apply(const std::string& value) const
        {
#ifdef _WIN32
            _putenv_s(m_name, value.c_str());
#else
            if (value.empty())
                unsetenv(m_name);
            else
                setenv(m_name, value.c_str(), 1);
#endif
        }

        const char* m_name;
        bool m_hadValue = false;
        std::string m_previous;
    };

    /// The variable Spark::UserPaths reads for the per-user configuration root.
#ifdef _WIN32
    constexpr const char* UserConfigRootVariable = "LOCALAPPDATA";
#else
    constexpr const char* UserConfigRootVariable = "XDG_CONFIG_HOME";
#endif

    /// The variable Spark::UserPaths reads for the per-user data root, which is
    /// what GetUserSavesDir and ResolveSaveDirectory hang off (not the config root).
#ifdef _WIN32
    constexpr const char* UserDataRootVariable = "LOCALAPPDATA";
#else
    constexpr const char* UserDataRootVariable = "XDG_DATA_HOME";
#endif

    /**
     * @brief RAII override of the process working directory, restored on scope exit.
     *
     * ResolveSaveDirectory looks for the legacy save tree at <cwd>/Saves, so the
     * migration path can only be exercised from a scratch working directory.
     */
    class ScopedWorkingDirectory
    {
      public:
        explicit ScopedWorkingDirectory(const std::filesystem::path& directory)
        {
            std::error_code ec;
            m_previous = std::filesystem::current_path(ec);
            std::filesystem::current_path(directory, ec);
        }

        ~ScopedWorkingDirectory()
        {
            std::error_code ec;
            std::filesystem::current_path(m_previous, ec);
        }

        ScopedWorkingDirectory(const ScopedWorkingDirectory&) = delete;
        ScopedWorkingDirectory& operator=(const ScopedWorkingDirectory&) = delete;

      private:
        std::filesystem::path m_previous;
    };

    std::filesystem::path MakeScratchDirectory(const char* name)
    {
        const std::filesystem::path directory = std::filesystem::temp_directory_path() / name;
        std::error_code ec;
        std::filesystem::remove_all(directory, ec);
        std::filesystem::create_directories(directory, ec);
        return directory;
    }
} // namespace

TEST(UserDataPaths_UserRootsAreAbsoluteAndNested)
{
    const std::filesystem::path dataDir = Spark::UserPaths::GetUserDataDir();
    if (dataDir.empty())
    {
        SKIP_TEST("Platform supplied no per-user data location for this process");
        return;
    }

    EXPECT_TRUE(dataDir.is_absolute());
    EXPECT_STR_CONTAINS(dataDir.generic_string(), "SparkEngine");

    // Saves belong under the writable user root, never beside the executable.
    const std::filesystem::path savesDir = Spark::UserPaths::GetUserSavesDir();
    ASSERT_FALSE(savesDir.empty());
    EXPECT_EQ(savesDir.parent_path().generic_string(), dataDir.generic_string());

    const std::filesystem::path configDir = Spark::UserPaths::GetUserConfigDir();
    EXPECT_FALSE(configDir.empty());
    EXPECT_TRUE(configDir.is_absolute());
}

TEST(UserDataPaths_WritableProbeAcceptsCreatableDirectoriesAndRejectsEmptyPaths)
{
    const std::filesystem::path directory = MakeScratchDirectory("SparkUserPathsWritable");
    EXPECT_TRUE(Spark::UserPaths::IsDirectoryWritable(directory));

    // The probe leaves nothing behind.
    EXPECT_TRUE(std::filesystem::is_empty(directory));

    // A directory that can be created is writable; an empty path never is.
    EXPECT_TRUE(Spark::UserPaths::IsDirectoryWritable(directory / "Nested" / "Deeper"));
    EXPECT_FALSE(Spark::UserPaths::IsDirectoryWritable(std::filesystem::path{}));

    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
}

TEST(UserDataPaths_UserRootFollowsTheEnvironment)
{
    const std::filesystem::path root = MakeScratchDirectory("SparkUserPathsRoot");
    const ScopedEnvironmentVariable envOverride(UserConfigRootVariable, root.string());

    const std::filesystem::path configDir = Spark::UserPaths::GetUserConfigDir();
    ASSERT_FALSE(configDir.empty());
    EXPECT_STR_CONTAINS(configDir.generic_string(), root.generic_string());

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(EngineSettings_FindSettingsPathPrefersAnExistingUserSettingsFile)
{
    const std::filesystem::path root = MakeScratchDirectory("SparkUserPathsSettings");
    const ScopedEnvironmentVariable envOverride(UserConfigRootVariable, root.string());

    const std::filesystem::path userConfigDir = Spark::UserPaths::GetUserConfigDir();
    ASSERT_FALSE(userConfigDir.empty());

    std::error_code ec;
    std::filesystem::create_directories(userConfigDir, ec);
    const std::filesystem::path userSettings = userConfigDir / "settings.ini";
    {
        std::ofstream stream(userSettings, std::ios::trunc);
        stream << "[Graphics]\nWindowWidth=1280\n";
    }
    ASSERT_TRUE(std::filesystem::exists(userSettings));

    // The player's own settings.ini is the only copy that survives an upgrade
    // or reinstall, so it must win over any file shipped in the install tree.
    EXPECT_EQ(EngineSettings::FindSettingsPath(), userSettings.string());

    std::filesystem::remove_all(root, ec);
}

TEST(UserDataPaths_ResolveSaveDirectoryUsesThePerUserDirectoryWithNoLegacySaves)
{
    const std::filesystem::path root = MakeScratchDirectory("SparkUserPathsSavesFresh");
    const std::filesystem::path cwd = MakeScratchDirectory("SparkUserPathsSavesFreshCwd");
    const ScopedEnvironmentVariable envOverride(UserDataRootVariable, root.string());
    const ScopedWorkingDirectory cwdOverride(cwd);

    const std::filesystem::path expected =
        Spark::UserPaths::NarrowSafeDirectory(Spark::UserPaths::GetUserSavesDir());
    if (expected.empty())
    {
        SKIP_TEST("Platform supplied no narrow-safe per-user saves location for this process");
        return;
    }

    const std::string resolved = Spark::UserPaths::ResolveSaveDirectory();

    // With a usable per-user location the resolver must never fall back to the
    // relative legacy "Saves" path: an installed game cannot write beside its
    // binaries, and an upgrade deletes anything that lands there.
    EXPECT_TRUE(resolved != std::string("Saves"));
    EXPECT_EQ(resolved, expected.string());
    EXPECT_TRUE(std::filesystem::is_directory(std::filesystem::path(resolved)));

    // The marker is written even when there was nothing to migrate, so a later
    // launch does not rescan the working directory.
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(resolved) / ".migrated-from-working-directory"));

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(UserDataPaths_ResolveSaveDirectoryMigratesLegacyWorkingDirectorySaves)
{
    const std::filesystem::path root = MakeScratchDirectory("SparkUserPathsSavesMigrate");
    const std::filesystem::path cwd = MakeScratchDirectory("SparkUserPathsSavesMigrateCwd");
    const ScopedEnvironmentVariable envOverride(UserDataRootVariable, root.string());

    std::error_code ec;
    const std::filesystem::path legacy = cwd / "Saves";
    std::filesystem::create_directories(legacy, ec);
    {
        std::ofstream stream(legacy / "slot0.sav", std::ios::trunc);
        stream << "legacy-slot-payload";
    }
    ASSERT_TRUE(std::filesystem::exists(legacy / "slot0.sav"));

    const ScopedWorkingDirectory cwdOverride(cwd);

    const std::filesystem::path expected =
        Spark::UserPaths::NarrowSafeDirectory(Spark::UserPaths::GetUserSavesDir());
    if (expected.empty())
    {
        SKIP_TEST("Platform supplied no narrow-safe per-user saves location for this process");
        return;
    }

    const std::string resolved = Spark::UserPaths::ResolveSaveDirectory();
    EXPECT_EQ(resolved, expected.string());

    // RED proof for the one-time migration: without the copy, an upgrading
    // player's slot is simply absent from the directory the new build reads.
    const std::filesystem::path migrated = std::filesystem::path(resolved) / "slot0.sav";
    ASSERT_TRUE(std::filesystem::exists(migrated));
    {
        std::ifstream stream(migrated);
        std::string contents;
        std::getline(stream, contents);
        EXPECT_EQ(contents, std::string("legacy-slot-payload"));
    }

    // Copied, not moved: the legacy tree survives, so a rollback loses nothing.
    EXPECT_TRUE(std::filesystem::exists(legacy / "slot0.sav"));

    // And it is one-time: the marker means deleting a save does not resurrect it
    // from the legacy tree on the next launch.
    ASSERT_TRUE(std::filesystem::exists(std::filesystem::path(resolved) / ".migrated-from-working-directory"));
    std::filesystem::remove(migrated, ec);
    EXPECT_EQ(Spark::UserPaths::ResolveSaveDirectory(), expected.string());
    EXPECT_FALSE(std::filesystem::exists(migrated));

    // Leave the scratch working directory in place: it is still this process's
    // cwd until cwdOverride unwinds, and MakeScratchDirectory clears it next run.
    std::filesystem::remove_all(root, ec);
}

TEST(EngineSettings_FindSettingsPathResolvesToAWritableDirectory)
{
    const std::string resolved = EngineSettings::FindSettingsPath();
    ASSERT_FALSE(resolved.empty());

    const std::filesystem::path parent = std::filesystem::path(resolved).parent_path();
    EXPECT_FALSE(parent.empty());
    EXPECT_TRUE(Spark::UserPaths::IsDirectoryWritable(parent));
}
