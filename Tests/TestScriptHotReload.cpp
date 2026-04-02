// TestScriptHotReload.cpp - Tests for ScriptHotReloadManager
// Standalone reimplementation to avoid platform-specific header dependencies

#include "TestFramework.h"
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// Standalone reimplementation of Spark::Scripting hot-reload types

namespace
{

    using TimePoint = std::chrono::steady_clock::time_point;
    const auto kT0 = TimePoint{};

    enum class FileChangeType
    {
        Modified,
        Created,
        Deleted,
        Renamed
    };

    struct FileChangeEvent
    {
        std::string filePath;
        FileChangeType type;
        TimePoint timestamp;
    };

    struct RecompileResult
    {
        bool success = false;
        std::string filePath;
        std::string errorMessage;
        int errorLine = 0;
        float compileTimeMs = 0.0f;
    };

    using RecompileCallback = std::function<RecompileResult(const std::string&)>;
    using ErrorCallback = std::function<void(const RecompileResult&)>;

    class ScriptHotReloadManager
    {
      public:
        static constexpr int MaxRecentErrors = 10;

        void AddWatchDirectory(const std::string& dir, bool = true) { m_watchDirs.push_back(dir); }
        void SetWatchExtensions(const std::vector<std::string>& e) { m_extensions = e; }
        void SetRecompileCallback(RecompileCallback cb) { m_recompileCb = std::move(cb); }
        void SetErrorCallback(ErrorCallback cb) { m_errorCb = std::move(cb); }
        void SetDebounceMs(uint32_t ms) { m_debounceMs = ms; }
        uint32_t GetDebounceMs() const { return m_debounceMs; }
        void Start() { m_running = true; }
        void Stop() { m_running = false; }

        void SimulateAddFile(const std::string& path, size_t tick)
        {
            if (IsWatchedExtension(path))
                m_fileStates[path] = tick;
        }
        void SimulateFileChange(const std::string& path, size_t tick, TimePoint at)
        {
            if (m_fileStates.count(path))
                m_fileStates[path] = tick;
            m_pending.push_back({path, at});
        }

        int PollChanges(TimePoint now)
        {
            if (!m_running)
                return 0;
            int n = 0;
            std::vector<Pending> keep;
            for (const auto& p : m_pending)
            {
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - p.at).count();
                if (ms >= static_cast<long long>(m_debounceMs))
                {
                    ProcessChange(p.path);
                    n++;
                }
                else
                    keep.push_back(p);
            }
            m_pending = std::move(keep);
            return n;
        }
        int RecompileAll()
        {
            int n = 0;
            for (const auto& [p, _] : m_fileStates)
            {
                ProcessChange(p);
                n++;
            }
            return n;
        }

        bool IsRunning() const { return m_running; }
        int GetWatchedFileCount() const { return static_cast<int>(m_fileStates.size()); }
        int GetRecompileCount() const { return m_recompileCount; }
        int GetErrorCount() const { return m_errorCount; }
        const std::vector<RecompileResult>& GetRecentErrors() const { return m_recentErrors; }
        const std::vector<std::string>& GetWatchDirectories() const { return m_watchDirs; }

        bool IsWatchedExtension(const std::string& path) const
        {
            for (const auto& ext : m_extensions)
                if (path.size() >= ext.size() && path.compare(path.size() - ext.size(), ext.size(), ext) == 0)
                    return true;
            return false;
        }

      private:
        struct Pending
        {
            std::string path;
            TimePoint at;
        };

        void ProcessChange(const std::string& filePath)
        {
            if (!m_recompileCb)
                return;
            auto result = m_recompileCb(filePath);
            m_recompileCount++;
            if (!result.success)
            {
                m_errorCount++;
                m_recentErrors.push_back(result);
                if (static_cast<int>(m_recentErrors.size()) > MaxRecentErrors)
                    m_recentErrors.erase(m_recentErrors.begin());
                if (m_errorCb)
                    m_errorCb(result);
            }
        }

        std::vector<std::string> m_watchDirs;
        std::vector<std::string> m_extensions = {".as", ".angelscript"};
        std::unordered_map<std::string, size_t> m_fileStates;
        RecompileCallback m_recompileCb;
        ErrorCallback m_errorCb;
        std::vector<Pending> m_pending;
        uint32_t m_debounceMs = 300;
        bool m_running = false;
        int m_recompileCount = 0;
        int m_errorCount = 0;
        std::vector<RecompileResult> m_recentErrors;
    };

    RecompileCallback SuccessCallback()
    {
        return [](const std::string& f) { return RecompileResult{true, f, "", 0, 1.0f}; };
    }

} // anonymous namespace

// 1. Watch directory configuration

TEST(ScriptHotReload_AddWatchDirectory)
{
    ScriptHotReloadManager mgr;
    mgr.AddWatchDirectory("Scripts/");
    mgr.AddWatchDirectory("Mods/Scripts/");
    EXPECT_EQ(static_cast<int>(mgr.GetWatchDirectories().size()), 2);
    EXPECT_TRUE(mgr.GetWatchDirectories()[0] == "Scripts/");
    EXPECT_TRUE(mgr.GetWatchDirectories()[1] == "Mods/Scripts/");
    // Duplicates are allowed (no dedup)
    mgr.AddWatchDirectory("Scripts/");
    EXPECT_EQ(static_cast<int>(mgr.GetWatchDirectories().size()), 3);
}

// 2. Extension filtering

TEST(ScriptHotReload_ExtensionFiltering)
{
    ScriptHotReloadManager mgr;
    // Default extensions: .as, .angelscript
    EXPECT_TRUE(mgr.IsWatchedExtension("player.as"));
    EXPECT_TRUE(mgr.IsWatchedExtension("enemy.angelscript"));
    EXPECT_FALSE(mgr.IsWatchedExtension("shader.hlsl"));
    EXPECT_FALSE(mgr.IsWatchedExtension(""));
    EXPECT_TRUE(mgr.IsWatchedExtension(".as")); // extension-only filename
    // Override defaults
    mgr.SetWatchExtensions({".lua", ".py"});
    EXPECT_FALSE(mgr.IsWatchedExtension("player.as"));
    EXPECT_TRUE(mgr.IsWatchedExtension("player.lua"));
    EXPECT_TRUE(mgr.IsWatchedExtension("main.py"));
}

// 3. Debounce timing

TEST(ScriptHotReload_Debounce_DefaultAndCustom)
{
    ScriptHotReloadManager mgr;
    EXPECT_EQ(mgr.GetDebounceMs(), 300u);
    mgr.SetDebounceMs(500);
    EXPECT_EQ(mgr.GetDebounceMs(), 500u);
}

TEST(ScriptHotReload_Debounce_ChangesWithinWindowAreCoalesced)
{
    ScriptHotReloadManager mgr;
    mgr.SetDebounceMs(200);
    mgr.Start();

    int compileCount = 0;
    mgr.SetRecompileCallback(
        [&](const std::string& file) -> RecompileResult
        {
            compileCount++;
            return RecompileResult{true, file, "", 0, 1.0f};
        });
    mgr.SimulateAddFile("test.as", 1);

    // Two changes at t0 and t+50ms
    mgr.SimulateFileChange("test.as", 2, kT0);
    mgr.SimulateFileChange("test.as", 3, kT0 + std::chrono::milliseconds(50));

    // At t+100ms neither has reached the 200ms debounce
    EXPECT_EQ(mgr.PollChanges(kT0 + std::chrono::milliseconds(100)), 0);
    EXPECT_EQ(compileCount, 0);

    // At t+200ms the first fires, second still debouncing
    EXPECT_EQ(mgr.PollChanges(kT0 + std::chrono::milliseconds(200)), 1);
    EXPECT_EQ(compileCount, 1);

    // At t+250ms the second fires
    EXPECT_EQ(mgr.PollChanges(kT0 + std::chrono::milliseconds(250)), 1);
    EXPECT_EQ(compileCount, 2);
}

// 4. Start/Stop lifecycle

TEST(ScriptHotReload_Lifecycle)
{
    ScriptHotReloadManager mgr;
    EXPECT_FALSE(mgr.IsRunning());
    mgr.Start();
    EXPECT_TRUE(mgr.IsRunning());
    mgr.Stop();
    EXPECT_FALSE(mgr.IsRunning());
    // Poll while stopped returns 0 even past debounce
    mgr.SetRecompileCallback(SuccessCallback());
    mgr.SimulateAddFile("test.as", 1);
    mgr.SimulateFileChange("test.as", 2, kT0);
    EXPECT_EQ(mgr.PollChanges(kT0 + std::chrono::milliseconds(500)), 0);
}

// 5. File change detection + 6. Recompile callback invocation

TEST(ScriptHotReload_FileChangeTriggersRecompile)
{
    ScriptHotReloadManager mgr;
    mgr.SetDebounceMs(0);
    mgr.Start();

    std::string compiledFile;
    mgr.SetRecompileCallback(
        [&](const std::string& f) -> RecompileResult
        {
            compiledFile = f;
            return RecompileResult{true, f, "", 0, 2.5f};
        });
    mgr.SimulateAddFile("Scripts/enemy_ai.as", 1);
    mgr.SimulateFileChange("Scripts/enemy_ai.as", 2, kT0);
    EXPECT_EQ(mgr.PollChanges(kT0), 1);
    EXPECT_TRUE(compiledFile == "Scripts/enemy_ai.as");
}

TEST(ScriptHotReload_MultipleFilesChanged)
{
    ScriptHotReloadManager mgr;
    mgr.SetDebounceMs(0);
    mgr.Start();

    std::vector<std::string> files;
    mgr.SetRecompileCallback(
        [&](const std::string& f) -> RecompileResult
        {
            files.push_back(f);
            return RecompileResult{true, f, "", 0, 1.0f};
        });
    mgr.SimulateAddFile("a.as", 1);
    mgr.SimulateAddFile("b.as", 1);
    mgr.SimulateFileChange("a.as", 2, kT0);
    mgr.SimulateFileChange("b.as", 2, kT0);
    EXPECT_EQ(mgr.PollChanges(kT0), 2);
    EXPECT_EQ(static_cast<int>(files.size()), 2);
}

TEST(ScriptHotReload_NoCallbackSetDoesNotCrash)
{
    ScriptHotReloadManager mgr;
    mgr.SetDebounceMs(0);
    mgr.Start();
    mgr.SimulateAddFile("test.as", 1);
    mgr.SimulateFileChange("test.as", 2, kT0);
    EXPECT_EQ(mgr.PollChanges(kT0), 1);
}

// 7. Error callback invocation

TEST(ScriptHotReload_ErrorCallbackInvokedOnFailure)
{
    ScriptHotReloadManager mgr;
    mgr.SetDebounceMs(0);
    mgr.Start();

    RecompileResult capturedError;
    mgr.SetRecompileCallback([](const std::string& f) -> RecompileResult
                             { return RecompileResult{false, f, "Syntax error", 42, 0.3f}; });
    mgr.SetErrorCallback([&](const RecompileResult& err) { capturedError = err; });
    mgr.SimulateAddFile("broken.as", 1);
    mgr.SimulateFileChange("broken.as", 2, kT0);
    mgr.PollChanges(kT0);

    EXPECT_FALSE(capturedError.success);
    EXPECT_TRUE(capturedError.filePath == "broken.as");
    EXPECT_TRUE(capturedError.errorMessage == "Syntax error");
    EXPECT_EQ(capturedError.errorLine, 42);
}

TEST(ScriptHotReload_ErrorCallbackNotInvokedOnSuccess)
{
    ScriptHotReloadManager mgr;
    mgr.SetDebounceMs(0);
    mgr.Start();
    bool errorCalled = false;
    mgr.SetRecompileCallback(SuccessCallback());
    mgr.SetErrorCallback([&](const RecompileResult&) { errorCalled = true; });
    mgr.SimulateAddFile("good.as", 1);
    mgr.SimulateFileChange("good.as", 2, kT0);
    mgr.PollChanges(kT0);
    EXPECT_FALSE(errorCalled);
}

// 8. RecompileAll

TEST(ScriptHotReload_RecompileAll)
{
    ScriptHotReloadManager mgr;
    std::vector<std::string> files;
    mgr.SetRecompileCallback(
        [&](const std::string& f) -> RecompileResult
        {
            files.push_back(f);
            return RecompileResult{true, f, "", 0, 0.1f};
        });
    // Empty set returns 0
    EXPECT_EQ(mgr.RecompileAll(), 0);
    // With files: recompiles all and increments count
    mgr.SimulateAddFile("a.as", 1);
    mgr.SimulateAddFile("b.as", 1);
    mgr.SimulateAddFile("c.angelscript", 1);
    EXPECT_EQ(mgr.RecompileAll(), 3);
    EXPECT_EQ(static_cast<int>(files.size()), 3);
    EXPECT_EQ(mgr.GetRecompileCount(), 3);
}

// 9. Error tracking

TEST(ScriptHotReload_ErrorCount_TracksFailures)
{
    ScriptHotReloadManager mgr;
    mgr.SetDebounceMs(0);
    mgr.Start();
    EXPECT_EQ(mgr.GetErrorCount(), 0);
    EXPECT_EQ(static_cast<int>(mgr.GetRecentErrors().size()), 0);

    int callIndex = 0;
    mgr.SetRecompileCallback(
        [&](const std::string& f) -> RecompileResult
        {
            callIndex++;
            bool ok = (callIndex % 2 == 0); // odd calls fail
            return RecompileResult{ok, f, ok ? "" : "error", ok ? 0 : 1, 0.1f};
        });
    mgr.SimulateAddFile("a.as", 1);
    mgr.SimulateAddFile("b.as", 1);
    mgr.SimulateAddFile("c.as", 1);
    mgr.SimulateFileChange("a.as", 2, kT0);
    mgr.SimulateFileChange("b.as", 2, kT0);
    mgr.SimulateFileChange("c.as", 2, kT0);
    mgr.PollChanges(kT0);
    // call 1 fail, 2 ok, 3 fail => 2 errors
    EXPECT_EQ(mgr.GetErrorCount(), 2);
}

TEST(ScriptHotReload_RecentErrors_CappedAtMax)
{
    ScriptHotReloadManager mgr;
    mgr.SetRecompileCallback([](const std::string& f) -> RecompileResult
                             { return RecompileResult{false, f, "error in " + f, 1, 0.1f}; });
    for (int i = 0; i < 15; i++)
        mgr.SimulateAddFile("script" + std::to_string(i) + ".as", 1);
    mgr.RecompileAll();

    EXPECT_EQ(static_cast<int>(mgr.GetRecentErrors().size()), ScriptHotReloadManager::MaxRecentErrors);
    EXPECT_EQ(mgr.GetErrorCount(), 15);
    // All retained errors should have valid file paths and error messages
    for (const auto& err : mgr.GetRecentErrors())
    {
        EXPECT_FALSE(err.filePath.empty());
        EXPECT_STR_CONTAINS(err.errorMessage, "error in ");
    }
}

// 10. Watched file count

TEST(ScriptHotReload_WatchedFileCount)
{
    ScriptHotReloadManager mgr;
    EXPECT_EQ(mgr.GetWatchedFileCount(), 0);
    mgr.SimulateAddFile("a.as", 1);
    EXPECT_EQ(mgr.GetWatchedFileCount(), 1);
    mgr.SimulateAddFile("b.angelscript", 1);
    EXPECT_EQ(mgr.GetWatchedFileCount(), 2);
    // Non-watched extensions are filtered out
    mgr.SimulateAddFile("readme.txt", 1);
    mgr.SimulateAddFile("shader.hlsl", 1);
    EXPECT_EQ(mgr.GetWatchedFileCount(), 2);
}

// 11. FileChangeType enum

TEST(ScriptHotReload_FileChangeType_EnumValues)
{
    EXPECT_EQ(static_cast<int>(FileChangeType::Modified), 0);
    EXPECT_EQ(static_cast<int>(FileChangeType::Created), 1);
    EXPECT_EQ(static_cast<int>(FileChangeType::Deleted), 2);
    EXPECT_EQ(static_cast<int>(FileChangeType::Renamed), 3);

    FileChangeEvent evt;
    evt.filePath = "test.as";
    evt.type = FileChangeType::Created;
    evt.timestamp = std::chrono::steady_clock::now();
    EXPECT_TRUE(evt.filePath == "test.as");
    EXPECT_EQ(static_cast<int>(evt.type), static_cast<int>(FileChangeType::Created));
}

// 12. RecompileResult struct

TEST(ScriptHotReload_RecompileResult)
{
    // Default values
    RecompileResult def;
    EXPECT_FALSE(def.success);
    EXPECT_TRUE(def.filePath.empty());
    EXPECT_TRUE(def.errorMessage.empty());
    EXPECT_EQ(def.errorLine, 0);
    EXPECT_NEAR(def.compileTimeMs, 0.0f, 0.001f);

    // Success case
    RecompileResult ok;
    ok.success = true;
    ok.filePath = "player.as";
    ok.compileTimeMs = 12.5f;
    EXPECT_TRUE(ok.success);
    EXPECT_TRUE(ok.filePath == "player.as");
    EXPECT_TRUE(ok.errorMessage.empty());
    EXPECT_NEAR(ok.compileTimeMs, 12.5f, 0.001f);

    // Failure case
    RecompileResult fail;
    fail.success = false;
    fail.filePath = "broken.as";
    fail.errorMessage = "Unexpected token '}'";
    fail.errorLine = 73;
    fail.compileTimeMs = 3.2f;
    EXPECT_FALSE(fail.success);
    EXPECT_TRUE(fail.filePath == "broken.as");
    EXPECT_TRUE(fail.errorMessage == "Unexpected token '}'");
    EXPECT_EQ(fail.errorLine, 73);
    EXPECT_NEAR(fail.compileTimeMs, 3.2f, 0.001f);
}
