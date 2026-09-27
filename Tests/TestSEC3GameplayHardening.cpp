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

// ----------------------------------------------------------------------------
// #10 NavMesh
// ----------------------------------------------------------------------------

TEST(SEC3Gameplay_NavMeshRejectsCountsBeyondFileSize)
{
    using namespace Spark::AI;
    ScratchDir dir("navmesh");
    auto& manager = NavMeshManager::GetInstance();

    // ~60 bytes declaring 10M triangles: the old loader value-initialised ~800 MB first.
    {
        std::vector<char> buf = SnavHeader();
        PutBytes(buf, uint32_t{0});          // vertexCount
        PutBytes(buf, uint32_t{10'000'000}); // triangleCount, no triangle bytes follow
        const fs::path path = dir.path / "triangles.snav";
        WriteFile(path, buf);

        LogCapture log;
        EXPECT_FALSE(manager.LoadNavMesh("sec3_triangles", path.string()));
        EXPECT_TRUE(log.Contains("10000000 triangles"));
        EXPECT_TRUE(log.Contains("remain in the file"));
        EXPECT_TRUE(manager.GetNavMesh("sec3_triangles") == nullptr);
    }

    // 10M declared vertices with no vertex bytes (~120 MB up front before the fix).
    {
        std::vector<char> buf = SnavHeader();
        PutBytes(buf, uint32_t{10'000'000});
        const fs::path path = dir.path / "vertices.snav";
        WriteFile(path, buf);

        LogCapture log;
        EXPECT_FALSE(manager.LoadNavMesh("sec3_vertices", path.string()));
        EXPECT_TRUE(log.Contains("10000000 vertices"));
    }

    // Per-triangle adjacency count larger than the rest of the file.
    {
        std::vector<char> buf = SnavHeader();
        PutBytes(buf, uint32_t{3});
        PutBytes(buf, XMFLOAT3{0, 0, 0});
        PutBytes(buf, XMFLOAT3{1, 0, 0});
        PutBytes(buf, XMFLOAT3{0, 0, 1});
        PutBytes(buf, uint32_t{1});
        PutBytes(buf, uint32_t{0});
        PutBytes(buf, uint32_t{1});
        PutBytes(buf, uint32_t{2});
        PutBytes(buf, XMFLOAT3{0.3f, 0, 0.3f});
        PutBytes(buf, XMFLOAT3{0, 1, 0});
        PutBytes(buf, 0.5f);
        PutBytes(buf, uint32_t{9'999}); // adjacency entries, none present
        const fs::path path = dir.path / "adjacency.snav";
        WriteFile(path, buf);

        LogCapture log;
        EXPECT_FALSE(manager.LoadNavMesh("sec3_adjacency", path.string()));
        EXPECT_TRUE(log.Contains("9999 adjacency entries"));
    }
}

TEST(SEC3Gameplay_NavMeshStillLoadsExactlySizedFile)
{
    using namespace Spark::AI;
    ScratchDir dir("navmesh_ok");

    // Two triangles; the last record ends exactly at end of file, with one adjacency entry
    // each so both the triangle and adjacency bounds are hit at equality.
    std::vector<char> buf = SnavHeader();
    PutBytes(buf, uint32_t{4});
    PutBytes(buf, XMFLOAT3{0, 0, 0});
    PutBytes(buf, XMFLOAT3{1, 0, 0});
    PutBytes(buf, XMFLOAT3{0, 0, 1});
    PutBytes(buf, XMFLOAT3{1, 0, 1});
    PutBytes(buf, uint32_t{2});
    const uint32_t tris[2][3] = {{0, 1, 2}, {1, 3, 2}};
    for (uint32_t t = 0; t < 2; ++t)
    {
        PutBytes(buf, tris[t][0]);
        PutBytes(buf, tris[t][1]);
        PutBytes(buf, tris[t][2]);
        PutBytes(buf, XMFLOAT3{0.5f, 0, 0.5f});
        PutBytes(buf, XMFLOAT3{0, 1, 0});
        PutBytes(buf, 0.5f);
        PutBytes(buf, uint32_t{1});
        PutBytes(buf, uint32_t{1u - t});
    }
    const fs::path path = dir.path / "valid.snav";
    WriteFile(path, buf);

    auto& manager = NavMeshManager::GetInstance();
    ASSERT_TRUE(manager.LoadNavMesh("sec3_valid", path.string()));
    const NavMeshData* mesh = manager.GetNavMesh("sec3_valid");
    ASSERT_TRUE(mesh != nullptr);
    EXPECT_EQ(mesh->vertices.size(), size_t{4});
    EXPECT_EQ(mesh->triangles.size(), size_t{2});
    EXPECT_EQ(mesh->triangles[1].adjacency.size(), size_t{1});
    manager.RemoveNavMesh("sec3_valid");
}

// ----------------------------------------------------------------------------
// #12 Replay
// ----------------------------------------------------------------------------

TEST(SEC3Gameplay_ReplayRejectsCountsBeyondFileSize)
{
    ScratchDir dir("replay");
    Spark::ReplaySystem replay;

    {
        std::vector<char> buf = ReplayHeader();
        PutBytes(buf, uint32_t{1'000'000}); // frames, none present
        const fs::path path = dir.path / "frames.replay";
        WriteFile(path, buf);
        LogCapture log;
        EXPECT_FALSE(replay.LoadFromFile(path.string()));
        EXPECT_TRUE(log.Contains("1000000 frames"));
    }
    {
        std::vector<char> buf = ReplayHeader();
        PutBytes(buf, uint32_t{1});
        PutBytes(buf, 0.0f);              // timestamp
        PutBytes(buf, uint32_t{0});       // frameNumber
        PutBytes(buf, uint32_t{100'000}); // entities, none present
        const fs::path path = dir.path / "entities.replay";
        WriteFile(path, buf);
        LogCapture log;
        EXPECT_FALSE(replay.LoadFromFile(path.string()));
        EXPECT_TRUE(log.Contains("100000 entities"));
    }
    {
        std::vector<char> buf = ReplayHeader();
        PutBytes(buf, uint32_t{0});         // frames
        PutBytes(buf, uint32_t{1'000'000}); // events, none present
        const fs::path path = dir.path / "events.replay";
        WriteFile(path, buf);
        LogCapture log;
        EXPECT_FALSE(replay.LoadFromFile(path.string()));
        EXPECT_TRUE(log.Contains("1000000 events"));
    }
}

TEST(SEC3Gameplay_ReplayRoundTripStillLoads)
{
    ScratchDir dir("replay_ok");
    const fs::path path = dir.path / "round_trip.replay";

    Spark::ReplaySystem writer;
    writer.SetMetadata("sec3_map", "sec3_mode");
    writer.StartRecording();
    Spark::ReplayEntityState a;
    a.entityId = 7;
    Spark::ReplayEntityState b;
    b.entityId = 9;
    writer.RecordFrame({a, b}, 0.0f);
    writer.RecordFrame({}, 1.0f);
    Spark::ReplayEvent named;
    named.timestamp = 0.5f;
    named.type = "kill";
    named.data = "headshot";
    writer.RecordEvent(named);
    writer.RecordEvent(Spark::ReplayEvent{}); // empty strings: the minimum-size record ends the file
    writer.StopRecording();
    ASSERT_TRUE(writer.SaveToFile(path.string()));

    Spark::ReplaySystem reader;
    ASSERT_TRUE(reader.LoadFromFile(path.string()));
    EXPECT_EQ(reader.GetFrameCount(), size_t{2});
}

// ----------------------------------------------------------------------------
// #13 Localization
// ----------------------------------------------------------------------------

TEST(SEC3Gameplay_LocalizationParsesLongValueWithoutRecursion)
{
    ScratchDir dir("loc_long");
    // One 1 MiB value: std::regex recursed per character here (stack overflow on
    // libstdc++, uncaught regex_error on MSVC).
    const std::string longValue(size_t{1} << 20, 'a');
    const fs::path path = dir.path / "long.json";
    WriteText(path, "{\n  \"greeting\": \"He said \\\"hi\\\"\",\n  \"long\": \"" + longValue +
                        "\",\n  \"section\": { \"nested\": \"ok\" },\n  \"count\": 5\n}\n");

    Spark::StringTable table;
    ASSERT_TRUE(table.LoadFromFile(path.string()));
    EXPECT_EQ(table.GetEntry("long").size(), longValue.size());
    EXPECT_EQ(table.GetEntry("greeting"), std::string("He said \"hi\""));
    EXPECT_EQ(table.GetEntry("nested"), std::string("ok"));
    EXPECT_FALSE(table.HasEntry("count"));
    EXPECT_EQ(table.GetEntryCount(), size_t{3});
}

TEST(SEC3Gameplay_LocalizationRejectsOversizedFile)
{
    ScratchDir dir("loc_big");
    const fs::path path = dir.path / "big.json";
    std::string text = "{ \"key\": \"value\",";
    text.append(Spark::StringTable::kMaxFileBytes, ' ');
    text += "}";
    WriteText(path, text);

    Spark::StringTable table;
    EXPECT_FALSE(table.LoadFromFile(path.string()));
    EXPECT_EQ(table.GetEntryCount(), size_t{0});
}

// ----------------------------------------------------------------------------
// #11 / #15 ModSystem
// ----------------------------------------------------------------------------

TEST(SEC3Gameplay_ModWithScriptContentIsNotReportedActive)
{
    ScratchDir dir("mods_scripts");
    const fs::path scripted = dir.path / "Scripted";
    const fs::path looseScript = dir.path / "LooseScript";
    const fs::path assetsOnly = dir.path / "AssetsOnly";
    fs::create_directories(scripted / "Scripts");
    fs::create_directories(looseScript / "Data");
    fs::create_directories(assetsOnly / "Assets");
    WriteText(scripted / "mod.json", R"({"id":"scripted","name":"Scripted","version":"1.0"})");
    WriteText(scripted / "Scripts" / "main.as", "void main() {}\n");
    WriteText(looseScript / "mod.json", R"({"id":"loose","name":"Loose","version":"1.0"})");
    WriteText(looseScript / "Data" / "Hook.AS", "void hook() {}\n");
    WriteText(assetsOnly / "mod.json", R"({"id":"assets","name":"Assets","version":"1.0"})");
    WriteText(assetsOnly / "Assets" / "readme.txt", "texture pack\n");

    Spark::ModSystem mods;
    ASSERT_EQ(mods.ScanForMods(dir.path.string()), size_t{3});

    EXPECT_FALSE(mods.LoadMod("scripted"));
    EXPECT_FALSE(mods.IsModActive("scripted"));
    EXPECT_TRUE(mods.GetModState("scripted") == Spark::ModState::Error);

    EXPECT_FALSE(mods.LoadMod("loose"));
    EXPECT_FALSE(mods.IsModActive("loose"));

    std::vector<std::string> announced;
    mods.OnModLoaded([&announced](const std::string& id) { announced.push_back(id); });
    EXPECT_TRUE(mods.LoadMod("assets"));
    EXPECT_TRUE(mods.IsModActive("assets"));
    ASSERT_EQ(announced.size(), size_t{1});
    EXPECT_EQ(announced[0], std::string("assets"));
}

TEST(SEC3Gameplay_ModScanRefusesSymlinkedManifest)
{
    ScratchDir dir("mods_symlink");
    const fs::path modsRoot = dir.path / "Mods";
    const fs::path outside = dir.path / "outside.json";
    fs::create_directories(modsRoot / "Linked");
    WriteText(outside, R"({"id":"linked","name":"Linked","version":"1.0"})");

    std::error_code linkError;
    fs::create_symlink(outside, modsRoot / "Linked" / "mod.json", linkError);
    if (linkError)
        SKIP_TEST("cannot create a file symlink here (Windows without Developer Mode): " + linkError.message());

    Spark::ModSystem mods;
    EXPECT_EQ(mods.ScanForMods(modsRoot.string()), size_t{0});
    EXPECT_TRUE(mods.GetModInfo("linked") == nullptr);
}
