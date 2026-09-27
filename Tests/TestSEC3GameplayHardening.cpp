// TestSEC3GameplayHardening.cpp - regression tests for the SEC3 gameplay-systems security findings.
//
//   #9  DirectStorageLoader started one std::thread per flushed request and kept every
//       finished thread until process exit; the fallback now runs on a bounded pool.
//   #10 NavMeshManager::LoadNavMesh resized to header counts (up to 10M triangles) before
//       checking that the file actually held them.
//   #11 ModSystem::LoadMod reported a mod Active even when it shipped scripts that no
//       sandboxed loader runs.
//   #12 ReplaySystem::LoadFromFile resized frames/entities/events to header counts before
//       checking that the file actually held them.
//   #13 StringTable::LoadFromFile read files of any size and scanned them with a recursive
//       std::regex that a single long value could drive into stack exhaustion.
//   #15 ModSystem accepted a symlinked mod.json and read manifests with an unbounded read
//       after a stat-time size check.
//
// Registered as the pinned SEC3Gameplay_ family in Tests/CMakeLists.txt.

#include "TestFramework.h"
#include "ScopedLoggerBaseline.h"

#include "Engine/AI/NavMesh.h"
#include "Engine/Localization/LocalizationSystem.h"
#include "Engine/Modding/ModSystem.h"
#include "Engine/Replay/ReplaySystem.h"
#include "Engine/Streaming/DirectStorageLoader.h"
#include "Utils/Logger.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    /// A fresh, empty scratch directory under the system temp directory, removed on scope exit.
    struct ScratchDir
    {
        fs::path path;

        explicit ScratchDir(const char* tag)
        {
            const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
            path = fs::temp_directory_path() / ("spark_sec3_" + std::string(tag) + "_" + std::to_string(ticks));
            std::error_code ec;
            fs::remove_all(path, ec);
            fs::create_directories(path, ec);
        }
        ScratchDir(const ScratchDir&) = delete;
        ScratchDir& operator=(const ScratchDir&) = delete;
        ~ScratchDir()
        {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    };

    template <typename T> void PutBytes(std::vector<char>& buf, const T& value)
    {
        const char* bytes = reinterpret_cast<const char*>(&value);
        buf.insert(buf.end(), bytes, bytes + sizeof(T));
    }

    void WriteFile(const fs::path& path, const std::vector<char>& bytes)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    void WriteText(const fs::path& path, const std::string& text)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
    }

    /// Records every log message while installed; the baseline logger is restored afterwards.
    class LogCapture
    {
      public:
        LogCapture()
        {
            auto lines = m_lines;
            Spark::Logger::Get().AddSink(std::make_unique<Spark::CallbackSink>([lines](const Spark::LogMessage& msg)
                                                                               { lines->push_back(msg.message); }));
        }

        bool Contains(const std::string& needle) const
        {
            for (const auto& line : *m_lines)
            {
                if (line.find(needle) != std::string::npos)
                    return true;
            }
            return false;
        }

      private:
        ScopedLoggerBaseline m_baseline; // declared first: restores before the sink is added
        std::shared_ptr<std::vector<std::string>> m_lines = std::make_shared<std::vector<std::string>>();
    };

    /// .snav header up to (not including) the vertex count.
    std::vector<char> SnavHeader()
    {
        std::vector<char> buf = {'S', 'N', 'A', 'V'};
        PutBytes(buf, uint32_t{1}); // version
        PutBytes(buf, 0.3f);        // cellSize
        PutBytes(buf, 2.0f);        // agentHeight
        PutBytes(buf, 0.5f);        // agentRadius
        PutBytes(buf, XMFLOAT3{0, 0, 0});
        PutBytes(buf, XMFLOAT3{1, 1, 1});
        return buf;
    }

    /// Replay header up to (not including) the frame count.
    std::vector<char> ReplayHeader()
    {
        std::vector<char> buf;
        PutBytes(buf, Spark::kReplayMagic);
        PutBytes(buf, uint32_t{1}); // version
        PutBytes(buf, uint32_t{0}); // mapName length
        PutBytes(buf, uint32_t{0}); // gameMode length
        PutBytes(buf, 1.0f);        // duration
        return buf;
    }
} // namespace

// ----------------------------------------------------------------------------
// #9 DirectStorageLoader
// ----------------------------------------------------------------------------

TEST(SEC3Gameplay_DirectStorageFallbackReusesBoundedWorkerPool)
{
    using namespace Spark::Streaming;

    ScratchDir dir("dstorage");
    const fs::path asset = dir.path / "asset.bin";
    WriteFile(asset, std::vector<char>(256, 'x'));

    auto& loader = DirectStorageLoader::GetInstance();
    loader.Shutdown();
    ASSERT_TRUE(loader.Initialize());
    ASSERT_EQ(loader.GetIoWorkerCount(), size_t{0});

    constexpr int kRounds = 3;
    constexpr int kRequestsPerRound = 40; // well above kMaxIoWorkers
    std::atomic<int> completed{0};
    std::atomic<int> failed{0};

    for (int round = 0; round < kRounds; ++round)
    {
        for (int i = 0; i < kRequestsPerRound; ++i)
        {
            LoadRequest request;
            request.filePath = asset.string();
            request.callback = [&completed, &failed](LoadRequestHandle, LoadStatus status)
            {
                if (status == LoadStatus::Completed)
                    completed.fetch_add(1);
                else
                    failed.fetch_add(1);
            };
            loader.Submit(request);
        }
        loader.Flush();

        // Thread-per-request would have started 40 threads here and kept all of them.
        EXPECT_LE(loader.GetIoWorkerCount(), DirectStorageLoader::kMaxIoWorkers);

        const int target = (round + 1) * kRequestsPerRound;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (completed.load() + failed.load() < target && std::chrono::steady_clock::now() < deadline)
        {
            loader.ProcessCompletions();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        EXPECT_EQ(completed.load() + failed.load(), target);
    }

    EXPECT_EQ(completed.load(), kRounds * kRequestsPerRound);
    EXPECT_EQ(failed.load(), 0);
    // Repeated area loads reuse the same pool instead of accumulating finished threads.
    EXPECT_GE(loader.GetIoWorkerCount(), size_t{1});
    EXPECT_LE(loader.GetIoWorkerCount(), DirectStorageLoader::kMaxIoWorkers);

    loader.Shutdown();
    EXPECT_EQ(loader.GetIoWorkerCount(), size_t{0});
}
