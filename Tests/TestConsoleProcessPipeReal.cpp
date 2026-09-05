// TestConsoleProcessPipeReal.cpp - Real ConsoleProcessManager executable
// resolution and real SimpleConsole command-ownership rules.

#include "TestFramework.h"
#include "Utils/ConsoleProcessManager.h"
#include "Utils/SparkConsole.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    std::filesystem::path MakeUniqueDirectory(const char* tag)
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        std::filesystem::path directory =
            std::filesystem::temp_directory_path() / (std::string(tag) + std::to_string(stamp));
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        return directory;
    }

    void WritePlaceholderExecutable(const std::filesystem::path& path)
    {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        std::ofstream file(path, std::ios::binary);
        file << "not a real executable";
    }

    /// Restores the working directory even when an assertion aborts the test body.
    struct WorkingDirectoryGuard final
    {
        std::filesystem::path previous;

        explicit WorkingDirectoryGuard(const std::filesystem::path& next) : previous(std::filesystem::current_path())
        {
            std::filesystem::current_path(next);
        }

        ~WorkingDirectoryGuard()
        {
            std::error_code error;
            std::filesystem::current_path(previous, error);
        }
    };

    constexpr const char* kConsoleFileName =
#ifdef _WIN32
        "SparkConsole.exe";
#else
        "SparkConsole";
#endif
} // namespace

// =============================================================================
// utils-11 — the console executable must never be resolved from the CWD
// =============================================================================

TEST(ConsoleProcess_PlantedBinaryInWorkingDirectoryIsNotResolved)
{
    namespace fs = std::filesystem;
    const fs::path plantedDirectory = MakeUniqueDirectory("spark_console_planted_");
    const fs::path trustedDirectory = MakeUniqueDirectory("spark_console_trusted_");
    WritePlaceholderExecutable(plantedDirectory / kConsoleFileName);

    {
        // A launcher or the editor can leave the process CWD pointing at a user
        // project folder. Nothing in it may be launched.
        WorkingDirectoryGuard workingDirectory(plantedDirectory);
        EXPECT_TRUE(fs::exists(kConsoleFileName));
        EXPECT_TRUE(
            Spark::ConsoleProcessManager::ResolveConsoleExecutable(trustedDirectory.string(), kConsoleFileName)
                .empty());
        // An unknown executable directory must not silently fall back to ".".
        EXPECT_TRUE(Spark::ConsoleProcessManager::ResolveConsoleExecutable(std::string{}, kConsoleFileName).empty());
    }

    std::error_code error;
    fs::remove_all(plantedDirectory, error);
    fs::remove_all(trustedDirectory, error);
}

TEST(ConsoleProcess_ExecutableBesideTheBinaryIsResolved)
{
    namespace fs = std::filesystem;
    const fs::path trustedDirectory = MakeUniqueDirectory("spark_console_trusted_");
    WritePlaceholderExecutable(trustedDirectory / kConsoleFileName);

    const std::string resolved =
        Spark::ConsoleProcessManager::ResolveConsoleExecutable(trustedDirectory.string(), kConsoleFileName);
    ASSERT_FALSE(resolved.empty());
    EXPECT_TRUE(fs::equivalent(fs::path(resolved), trustedDirectory / kConsoleFileName));

    // The bin child of the trusted directory is also allowed.
    const fs::path binDirectory = MakeUniqueDirectory("spark_console_bin_");
    WritePlaceholderExecutable(binDirectory / "bin" / kConsoleFileName);
    const std::string resolvedFromBin =
        Spark::ConsoleProcessManager::ResolveConsoleExecutable(binDirectory.string(), kConsoleFileName);
    ASSERT_FALSE(resolvedFromBin.empty());
    EXPECT_TRUE(fs::equivalent(fs::path(resolvedFromBin), binDirectory / "bin" / kConsoleFileName));

    std::error_code error;
    fs::remove_all(trustedDirectory, error);
    fs::remove_all(binDirectory, error);
}

// =============================================================================
// utils-12 — registration must not silently replace or downgrade a command
// =============================================================================

namespace
{
    struct ConsoleLifecycleGuard final
    {
        Spark::SimpleConsole& console;
        bool restoreUninitialized;
        std::vector<std::string> registered;

        explicit ConsoleLifecycleGuard(Spark::SimpleConsole& target)
            : console(target), restoreUninitialized(!target.IsInitialized())
        {
        }

        ~ConsoleLifecycleGuard()
        {
            for (const std::string& name : registered)
            {
                console.UnregisterCommand(name);
            }
            console.SetCurrentPermissionLevel(Spark::CommandPermission::Developer);
            if (restoreUninitialized)
            {
                console.Shutdown();
            }
        }
    };
} // namespace

TEST(SimpleConsole_ForeignOwnerCannotReplaceRegisteredCommand)
{
    auto& console = Spark::SimpleConsole::GetInstance();
    ConsoleLifecycleGuard guard(console);
    ASSERT_TRUE(console.Initialize());

    const std::string name = "utils12_owner_probe";
    ASSERT_FALSE(console.HasCommand(name));
    ASSERT_TRUE(console.RegisterCommand(
        name, [](const std::vector<std::string>&) { return std::string("engine"); }, "Engine owned", "Test",
        "utils12_owner_probe", Spark::CommandPermission::Admin));
    guard.registered.push_back(name);
    EXPECT_TRUE(console.GetCommandOwner(name).empty());

    // A game module sharing the host console must not be able to take the name.
    EXPECT_FALSE(console.RegisterCommand(
        name, [](const std::vector<std::string>&) { return std::string("module"); }, "Module owned", "Test",
        "utils12_owner_probe", Spark::CommandPermission::Player, "module.rogue"));
    EXPECT_TRUE(console.GetCommandOwner(name).empty());

    // Still Admin-gated: a Player-level session cannot run it.
    console.SetCurrentPermissionLevel(Spark::CommandPermission::Player);
    EXPECT_FALSE(console.ExecuteCommand(name));
    console.SetCurrentPermissionLevel(Spark::CommandPermission::Developer);
    EXPECT_TRUE(console.ExecuteCommand(name));
}

TEST(SimpleConsole_ReRegistrationCannotLowerRequiredPermission)
{
    auto& console = Spark::SimpleConsole::GetInstance();
    ConsoleLifecycleGuard guard(console);
    ASSERT_TRUE(console.Initialize());

    const std::string name = "utils12_permission_probe";
    ASSERT_TRUE(console.RegisterCommand(
        name, [](const std::vector<std::string>&) { return std::string("first"); }, "Admin only", "Test", name,
        Spark::CommandPermission::Admin, "module.a"));
    guard.registered.push_back(name);

    // The same owner may replace its own handler (hot reload) but the
    // replacement's weaker permission must not take effect.
    EXPECT_TRUE(console.RegisterCommand(
        name, [](const std::vector<std::string>&) { return std::string("second"); }, "Now player level", "Test", name,
        Spark::CommandPermission::Player, "module.a"));

    console.SetCurrentPermissionLevel(Spark::CommandPermission::Player);
    EXPECT_FALSE(console.ExecuteCommand(name));
    console.SetCurrentPermissionLevel(Spark::CommandPermission::Developer);
    EXPECT_TRUE(console.ExecuteCommand(name));
}

TEST(SimpleConsole_UnregisterCommandsByOwnerRemovesOnlyThatOwner)
{
    auto& console = Spark::SimpleConsole::GetInstance();
    ConsoleLifecycleGuard guard(console);
    ASSERT_TRUE(console.Initialize());

    const std::string hostName = "utils12_host_probe";
    const std::string moduleFirst = "utils12_module_probe_a";
    const std::string moduleSecond = "utils12_module_probe_b";
    auto handler = [](const std::vector<std::string>&) { return std::string("ok"); };

    ASSERT_TRUE(console.RegisterCommand(hostName, handler, "Host", "Test", hostName));
    guard.registered.push_back(hostName);
    ASSERT_TRUE(console.RegisterCommand(moduleFirst, handler, "Module", "Test", moduleFirst,
                                        Spark::CommandPermission::Player, "module.b"));
    ASSERT_TRUE(console.RegisterCommand(moduleSecond, handler, "Module", "Test", moduleSecond,
                                        Spark::CommandPermission::Player, "module.b"));
    EXPECT_EQ(console.GetCommandOwner(moduleFirst), std::string("module.b"));

    EXPECT_EQ(console.UnregisterCommandsByOwner("module.b"), static_cast<size_t>(2));
    EXPECT_FALSE(console.HasCommand(moduleFirst));
    EXPECT_FALSE(console.HasCommand(moduleSecond));
    // The identically categorised host command survives — this is what name-based
    // erasure on module unload used to get wrong.
    EXPECT_TRUE(console.HasCommand(hostName));
}

TEST(SimpleConsole_UnregisterCommandsByOwnerRefusesTheEmptyToken)
{
    auto& console = Spark::SimpleConsole::GetInstance();
    ConsoleLifecycleGuard guard(console);
    ASSERT_TRUE(console.Initialize());

    const std::string hostName = "utils12_empty_owner_probe";
    auto handler = [](const std::vector<std::string>&) { return std::string("ok"); };
    ASSERT_TRUE(console.RegisterCommand(hostName, handler, "Host", "Test", hostName));
    guard.registered.push_back(hostName);

    // The empty token is the engine's own, so it is the natural value a module
    // that forgets to set its id passes. Honouring it would erase every host and
    // engine command in the registry.
    EXPECT_EQ(console.UnregisterCommandsByOwner(std::string{}), static_cast<size_t>(0));
    EXPECT_TRUE(console.HasCommand(hostName));
    // The built-in engine commands registered at Initialize() are still there.
    EXPECT_TRUE(console.HasCommand("help"));
}
