// TestFileWatcher.cpp - Tests for Spark::Utils::FileWatcher
#include "TestFramework.h"
#include "Utils/FileWatcher/FileWatcher.h"

#include <filesystem>
#include <fstream>
#include <thread>

// Helper to create a temporary directory for file watcher tests
static std::string CreateTempDir()
{
    auto path = std::filesystem::temp_directory_path() / "spark_fw_test";
    std::filesystem::create_directories(path);
    return path.string();
}

static void CleanupTempDir(const std::string& path)
{
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

static void WriteFile(const std::string& path, const std::string& content)
{
    std::ofstream f(path);
    f << content;
}

// ============================================================================
// Initialization
// ============================================================================

TEST(FileWatcher_Initialize)
{
    auto& watcher = Spark::Utils::FileWatcher::GetInstance();
    watcher.Initialize(0.5f);
    EXPECT_EQ(static_cast<size_t>(0), watcher.GetWatchCount());
    watcher.Shutdown();
}

TEST(FileWatcher_ShutdownClearsWatches)
{
    auto& watcher = Spark::Utils::FileWatcher::GetInstance();
    watcher.Initialize(1.0f);
    std::string dir = CreateTempDir();
    watcher.Watch(dir, [](const Spark::Utils::FileChangeEvent&) {});
    EXPECT_EQ(static_cast<size_t>(1), watcher.GetWatchCount());
    watcher.Shutdown();
    EXPECT_EQ(static_cast<size_t>(0), watcher.GetWatchCount());
    CleanupTempDir(dir);
}

// ============================================================================
// Watch registration
// ============================================================================

TEST(FileWatcher_WatchDirectory)
{
    auto& watcher = Spark::Utils::FileWatcher::GetInstance();
    watcher.Initialize(1.0f);
    std::string dir = CreateTempDir();

    uint32_t id = watcher.Watch(dir, [](const Spark::Utils::FileChangeEvent&) {});
    EXPECT_TRUE(id > 0);
    EXPECT_EQ(static_cast<size_t>(1), watcher.GetWatchCount());

    watcher.Shutdown();
    CleanupTempDir(dir);
}

TEST(FileWatcher_WatchFile)
{
    auto& watcher = Spark::Utils::FileWatcher::GetInstance();
    watcher.Initialize(1.0f);
    std::string dir = CreateTempDir();
    std::string filePath = dir + "/test.txt";
    WriteFile(filePath, "initial");

    uint32_t id = watcher.WatchFile(filePath, [](const Spark::Utils::FileChangeEvent&) {});
    EXPECT_TRUE(id > 0);
    EXPECT_EQ(static_cast<size_t>(1), watcher.GetWatchCount());

    watcher.Shutdown();
    CleanupTempDir(dir);
}

TEST(FileWatcher_Unwatch)
{
    auto& watcher = Spark::Utils::FileWatcher::GetInstance();
    watcher.Initialize(1.0f);
    std::string dir = CreateTempDir();

    uint32_t id = watcher.Watch(dir, [](const Spark::Utils::FileChangeEvent&) {});
    EXPECT_EQ(static_cast<size_t>(1), watcher.GetWatchCount());
    watcher.Unwatch(id);
    EXPECT_EQ(static_cast<size_t>(0), watcher.GetWatchCount());

    watcher.Shutdown();
    CleanupTempDir(dir);
}

// ============================================================================
// Change detection
// ============================================================================

TEST(FileWatcher_DetectFileCreation)
{
    auto& watcher = Spark::Utils::FileWatcher::GetInstance();
    watcher.Initialize(0.01f);
    std::string dir = CreateTempDir();

    bool created = false;
    watcher.Watch(dir,
                  [&](const Spark::Utils::FileChangeEvent& e)
                  {
                      if (e.type == Spark::Utils::FileChangeType::Created)
                          created = true;
                  });

    // Initial poll to establish baseline
    watcher.Update(0.1f);

    // Create a new file
    WriteFile(dir + "/new_file.txt", "hello");

    // Poll again to detect creation
    watcher.Update(0.1f);
    EXPECT_TRUE(created);

    watcher.Shutdown();
    CleanupTempDir(dir);
}

TEST(FileWatcher_DetectFileModification)
{
    auto& watcher = Spark::Utils::FileWatcher::GetInstance();
    watcher.Initialize(0.01f);
    std::string dir = CreateTempDir();
    std::string filePath = dir + "/existing.txt";
    WriteFile(filePath, "original");

    bool modified = false;
    watcher.WatchFile(filePath,
                      [&](const Spark::Utils::FileChangeEvent& e)
                      {
                          if (e.type == Spark::Utils::FileChangeType::Modified)
                              modified = true;
                      });

    // Wait briefly to ensure different timestamp
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    WriteFile(filePath, "modified content");

    watcher.Update(0.1f);
    EXPECT_TRUE(modified);

    watcher.Shutdown();
    CleanupTempDir(dir);
}

TEST(FileWatcher_DetectFileDeletion)
{
    auto& watcher = Spark::Utils::FileWatcher::GetInstance();
    watcher.Initialize(0.01f);
    std::string dir = CreateTempDir();
    std::string filePath = dir + "/to_delete.txt";
    WriteFile(filePath, "delete me");

    bool deleted = false;
    watcher.WatchFile(filePath,
                      [&](const Spark::Utils::FileChangeEvent& e)
                      {
                          if (e.type == Spark::Utils::FileChangeType::Deleted)
                              deleted = true;
                      });

    std::filesystem::remove(filePath);
    watcher.Update(0.1f);
    EXPECT_TRUE(deleted);

    watcher.Shutdown();
    CleanupTempDir(dir);
}

TEST(FileWatcher_PollIntervalRespected)
{
    auto& watcher = Spark::Utils::FileWatcher::GetInstance();
    watcher.Initialize(10.0f); // Very long poll interval
    std::string dir = CreateTempDir();

    int callCount = 0;
    watcher.Watch(dir, [&](const Spark::Utils::FileChangeEvent&) { ++callCount; });

    // Create a file but update with tiny dt (won't reach poll interval)
    WriteFile(dir + "/test.txt", "hello");
    watcher.Update(0.001f);
    EXPECT_EQ(0, callCount);

    watcher.Shutdown();
    CleanupTempDir(dir);
}

TEST(FileWatcher_TrackedFileCount)
{
    auto& watcher = Spark::Utils::FileWatcher::GetInstance();
    watcher.Initialize(1.0f);
    std::string dir = CreateTempDir();
    WriteFile(dir + "/a.txt", "a");
    WriteFile(dir + "/b.txt", "b");

    watcher.Watch(dir, [](const Spark::Utils::FileChangeEvent&) {});
    EXPECT_EQ(static_cast<size_t>(2), watcher.GetTrackedFileCount());

    watcher.Shutdown();
    CleanupTempDir(dir);
}
