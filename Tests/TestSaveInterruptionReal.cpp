/**
 * @file TestSaveInterruptionReal.cpp
 * @brief SAVE-230: process-interruption rehearsal for the SaveSystem atomic write path.
 *
 * AtomicWrite_* (POSIX) run a writer child that saves successive generations of one slot
 * through the production SaveSystem::Save path, and SIGKILL it at seeded, randomized
 * offsets. The child is a freshly exec'd SparkTests process that runs only this test in
 * its writer role, not a bare fork() of this multi-threaded runner: the runner's other
 * threads (async logger, job workers) may hold locks at fork time, and a forked child that
 * logs or allocates could deadlock (same reasoning as TestDATA120BackupRestore.cpp).
 *
 * The child prints ATOMICWRITE_START=<n> (flushed) before Save(n) begins and
 * ATOMICWRITE_DONE=<n> after it returned true, so after the kill the parent knows the
 * last completed generation D and whether generation D+1 was in flight. It then requires
 * the documented contract of the atomic write (tmp file, fsync, staged-and-renamed .bak,
 * atomic rename, directory fsync):
 *   - Load() of the slot succeeds, and the loaded generation is D or the in-flight D+1;
 *   - every payload value belongs to that one generation and its CRC-32 trailer verifies;
 *   - a stray `<slot>.spark_save.tmp` means the rename never happened, so the loaded
 *     generation is D - the temp file is never promoted, listed, or read;
 *   - the retained `<slot>.spark_save.bak` is a complete save of the preceding revision;
 *   - the next writer (and finally this process) saves over the leftovers and loads back.
 *
 * Found by this rehearsal: the retained copy used to be refreshed with an in-place
 * copy_file, which truncates `.bak` first, so a kill mid-copy left a torn `.bak` (about
 * 4 kills in 1000). SaveFileDurability::CopyFileAtomically now stages and renames it.
 *
 * Reproduce a failure with the logged seed: SPARK_ATOMICWRITE_SEED=<seed>. Raise the
 * iteration count with SPARK_ATOMICWRITE_ITERATIONS=<n>. Windows needs a separate
 * TerminateProcess rehearsal; this file compiles to nothing there.
 */

#include "TestFramework.h"

#ifndef _WIN32
#include "Engine/ECS/Components.h"
#include "Engine/SaveSystem/SaveSystem.h"
#include "Utils/CRC32.h"
#include "Utils/Process.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

using namespace Spark;

namespace
{
    namespace fs = std::filesystem;

    constexpr const char* kTestName = "AtomicWrite_KilledSaveWriterNeverLosesTheLastCompletedGeneration";
    constexpr const char* kRoleEnv = "SPARK_ATOMICWRITE_ROLE";
    constexpr const char* kDirEnv = "SPARK_ATOMICWRITE_DIR";
    constexpr const char* kStartEnv = "SPARK_ATOMICWRITE_START";
    constexpr const char* kSlot = "interrupted";
    constexpr const char* kProbeSlot = "retained_probe";

    constexpr uint32_t kDefaultIterations = 100;
    constexpr uint32_t kDefaultSeed = 0x5A7E230u;

    // Enough payload that the temp write, the retained-copy copy and the rename each take
    // a measurable share of a save, so randomized kill offsets land inside every phase.
    constexpr int kEntityCount = 256;
    constexpr int kFillerKeys = 12;
    constexpr size_t kFillerBytes = 32 * 1024;

    std::string EnvOrEmpty(const char* name)
    {
        const char* value = std::getenv(name);
        return value ? value : "";
    }

    uint32_t EnvOrDefault(const char* name, uint32_t fallback)
    {
        const std::string value = EnvOrEmpty(name);
        if (value.empty())
            return fallback;
        return static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 0));
    }

    fs::path TestBinaryPath()
    {
#ifdef __APPLE__
        uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size);
        std::string buffer(size, '\0');
        if (_NSGetExecutablePath(buffer.data(), &size) != 0)
            return {};
        return fs::path(buffer.c_str());
#else
        std::error_code error;
        const fs::path self = fs::read_symlink("/proc/self/exe", error);
        return error ? fs::path{} : self;
#endif
    }

    /// Filler byte for @p generation, so a value from another generation is detectable.
    char FillerByte(uint32_t generation)
    {
        return static_cast<char>('a' + generation % 26u);
    }

    std::unordered_map<std::string, std::string> MakeGenerationState(uint32_t generation)
    {
        std::unordered_map<std::string, std::string> state;
        state["generation"] = std::to_string(generation);
        for (int key = 0; key < kFillerKeys; ++key)
            state["filler" + std::to_string(key)] = std::string(kFillerBytes, FillerByte(generation));
        return state;
    }

    /// Generation recorded in @p state, or nullopt when any filler disagrees with it.
    std::optional<uint32_t> ConsistentGeneration(const std::unordered_map<std::string, std::string>& state)
    {
        const auto found = state.find("generation");
        if (found == state.end() || found->second.empty())
            return std::nullopt;
        const uint32_t generation = static_cast<uint32_t>(std::strtoul(found->second.c_str(), nullptr, 10));
        for (int key = 0; key < kFillerKeys; ++key)
        {
            const auto filler = state.find("filler" + std::to_string(key));
            if (filler == state.end() || filler->second != std::string(kFillerBytes, FillerByte(generation)))
                return std::nullopt;
        }
        return generation;
    }

    /// Per-process scratch directory: SparkEngineTests and SparkSaveInterruptionTests both
    /// run AtomicWrite_* and CI runs ctest in parallel, so a shared path would let one run
    /// delete or overwrite the other's slot mid-iteration.
    fs::path UniqueScratchDirectory(std::string_view purpose)
    {
        return fs::temp_directory_path() /
               ("spark_save230_" + std::string(purpose) + "_" + std::to_string(static_cast<long>(::getpid())));
    }

    void PopulateWorld(World& world)
    {
        for (int index = 0; index < kEntityCount; ++index)
        {
            const EntityID entity = world.CreateEntity("entity_" + std::to_string(index));
            world.AddComponent<Transform>(entity);
        }
    }

    bool SaveGeneration(SaveSystem& saveSystem, const char* slot, World& world, uint32_t generation)
    {
        SaveMetadata metadata;
        metadata.saveName = "generation " + std::to_string(generation);
        return saveSystem.Save(slot, world, metadata, MakeGenerationState(generation));
    }

    /// Load @p slot through the production reader; the generation it holds, or nullopt.
    std::optional<uint32_t> LoadGeneration(SaveSystem& saveSystem, const char* slot)
    {
        World world;
        std::unordered_map<std::string, std::string> state;
        if (!saveSystem.Load(slot, world, state))
            return std::nullopt;
        return ConsistentGeneration(state);
    }

    std::vector<char> ReadBytes(const fs::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    /// True when @p path is a SPRK file of this build's version whose CRC-32 trailer verifies.
    bool HasValidChecksum(const fs::path& path)
    {
        const std::vector<char> bytes = ReadBytes(path);
        if (bytes.size() < 12u || std::memcmp(bytes.data(), "SPRK", 4) != 0)
            return false;
        auto readLE32 = [&](size_t offset)
        {
            uint32_t value = 0;
            for (size_t index = 0; index < 4u; ++index)
                value |= static_cast<uint32_t>(static_cast<uint8_t>(bytes[offset + index])) << (8u * index);
            return value;
        };
        const size_t trailer = bytes.size() - sizeof(uint32_t);
        return readLE32(4) == kCurrentSaveVersion && readLE32(trailer) == ComputeCRC32(bytes.data(), trailer);
    }

    /// Writer role: save generation start, start+1, ... until SIGKILLed.
    int RunWriterRole()
    {
        const std::string directory = EnvOrEmpty(kDirEnv);
        const uint32_t start = EnvOrDefault(kStartEnv, 0);
        SaveSystem& saveSystem = SaveSystem::GetInstance();
        if (directory.empty() || start == 0 || !saveSystem.Initialize(directory))
        {
            std::printf("ATOMICWRITE_SETUP_FAILED\n");
            std::fflush(stdout);
            return 2;
        }

        World world;
        PopulateWorld(world);
        for (uint32_t generation = start; generation < start + 100000u; ++generation)
        {
            std::printf("ATOMICWRITE_START=%u\n", generation);
            std::fflush(stdout);
            if (!SaveGeneration(saveSystem, kSlot, world, generation))
            {
                std::printf("ATOMICWRITE_FAIL=%u\n", generation);
                std::fflush(stdout);
                return 3;
            }
            std::printf("ATOMICWRITE_DONE=%u\n", generation);
            std::fflush(stdout);
        }
        return 0;
    }

    std::expected<Spark::Process, std::string> SpawnWriter(const fs::path& directory, uint32_t startGeneration)
    {
        const fs::path self = TestBinaryPath();
        if (self.empty())
            return std::unexpected(std::string("cannot resolve the test binary path"));

        Spark::Process::Builder builder("env");
        // Drop the parent's test selection so the child runs exactly this test.
        for (const char* selection : {"SPARK_TEST_FILE", "SPARK_TEST_NAME_PREFIX", "SPARK_TEST_EXPECT_COUNT",
                                      "SPARK_TEST_EXCLUDE", "SPARK_TEST_LIMIT"})
        {
            builder.Arg("-u").Arg(selection);
        }
        builder.Arg(std::string("SPARK_TEST_NAME=") + kTestName)
            .Arg(std::string(kRoleEnv) + "=writer")
            .Arg(std::string(kDirEnv) + "=" + fs::absolute(directory).string())
            .Arg(std::string(kStartEnv) + "=" + std::to_string(startGeneration))
            .Arg(self.string())
            .WorkingDirectory(fs::current_path().string())
            .CaptureStdout()
            .MergeStderrIntoStdout();
        return builder.Launch();
    }

    struct WriterProgress
    {
        uint32_t lastStarted = 0;
        uint32_t lastDone = 0;
        bool failed = false;
        std::string log;
    };

    void FoldLine(const std::string& line, WriterProgress& progress)
    {
        auto valueOf = [&](std::string_view key) -> std::optional<uint32_t>
        {
            const size_t at = line.find(key);
            if (at == std::string::npos)
                return std::nullopt;
            return static_cast<uint32_t>(std::strtoul(line.c_str() + at + key.size(), nullptr, 10));
        };
        if (const auto started = valueOf("ATOMICWRITE_START="))
            progress.lastStarted = std::max(progress.lastStarted, *started);
        else if (const auto done = valueOf("ATOMICWRITE_DONE="))
            progress.lastDone = std::max(progress.lastDone, *done);
        else if (line.find("ATOMICWRITE_FAIL") != std::string::npos ||
                 line.find("ATOMICWRITE_SETUP_FAILED") != std::string::npos)
            progress.failed = true;
        progress.log += line + '\n';
    }
} // namespace

TEST(AtomicWrite_KilledSaveWriterNeverLosesTheLastCompletedGeneration)
{
    if (EnvOrEmpty(kRoleEnv) == "writer")
    {
        std::fflush(stdout);
        ::_exit(RunWriterRole());
    }

    const fs::path directory = UniqueScratchDirectory("atomicwrite");
    fs::remove_all(directory);
    fs::create_directories(directory);
    const fs::path primary = directory / (std::string(kSlot) + ".spark_save");
    const fs::path temporary = directory / (std::string(kSlot) + ".spark_save.tmp");
    const fs::path retained = directory / (std::string(kSlot) + ".spark_save.bak");
    const fs::path probe = directory / (std::string(kProbeSlot) + ".spark_save");

    SaveSystem& saveSystem = SaveSystem::GetInstance();
    ASSERT_TRUE(saveSystem.Initialize(directory.string()));

    // Seed generation 1 and time one full save, which scales the kill offsets below.
    World seedWorld;
    PopulateWorld(seedWorld);
    const auto seedStart = std::chrono::steady_clock::now();
    ASSERT_TRUE(SaveGeneration(saveSystem, kSlot, seedWorld, 1));
    const auto saveMicros = std::max<int64_t>(
        200,
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - seedStart).count());

    const uint32_t iterations = std::max<uint32_t>(1, EnvOrDefault("SPARK_ATOMICWRITE_ITERATIONS", kDefaultIterations));
    const uint32_t seed = EnvOrDefault("SPARK_ATOMICWRITE_SEED", kDefaultSeed);
    std::mt19937 random(seed);
    // Offsets span three saves, so kills land before, inside and after every write phase.
    std::uniform_int_distribution<int64_t> killOffset(0, 3 * saveMicros);
    std::printf("[AtomicWrite] seed=0x%X iterations=%u saveMicros=%lld\n", seed, iterations,
                static_cast<long long>(saveMicros));

    uint32_t onDisk = 1;
    uint32_t killsWithStrayTemp = 0;
    uint32_t killsAfterInFlightRename = 0;
    for (uint32_t iteration = 0; iteration < iterations; ++iteration)
    {
        const int64_t offsetMicros = killOffset(random);
        auto launched = SpawnWriter(directory, onDisk + 1);
        ASSERT_TRUE(launched.has_value());
        Spark::Process& writer = *launched;

        // Wait for the writer's first completed save, so the kill always lands inside its
        // save loop. Bounded: a wedged child fails the test instead of hanging the run.
        WriterProgress progress;
        std::string line;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (progress.lastDone == 0 && !progress.failed && writer.IsRunning() &&
               std::chrono::steady_clock::now() < deadline)
        {
            while (writer.TryReadLine(line))
                FoldLine(line, progress);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        const bool reachedLoop = progress.lastDone != 0;
        if (reachedLoop)
            std::this_thread::sleep_for(std::chrono::microseconds(offsetMicros));
        const bool stillSaving = writer.IsRunning();
        writer.Kill();
        // Markers written before the kill still count.
        std::istringstream rest(writer.ReadAllStdout());
        while (std::getline(rest, line))
            FoldLine(line, progress);

        auto dump = [&](const char* reason)
        {
            std::fprintf(stderr,
                         "[AtomicWrite] iteration %u (seed=0x%X offset=%lldus): %s; started=%u done=%u\n"
                         "---- writer output ----\n%s----\n",
                         iteration, seed, static_cast<long long>(offsetMicros), reason, progress.lastStarted,
                         progress.lastDone, progress.log.c_str());
        };
        if (!reachedLoop || !stillSaving || progress.failed)
        {
            dump("writer did not reach or stay in its save loop");
            ASSERT_TRUE(reachedLoop && stillSaving && !progress.failed);
        }

        const uint32_t done = progress.lastDone;
        const bool inFlight = progress.lastStarted > done;
        const bool strayTemp = fs::exists(temporary);

        // Exactly one slot is listed; the temp file is never presented as a save.
        const size_t listedSlots = saveSystem.GetSaveSlots().size();
        const std::optional<uint32_t> loaded = LoadGeneration(saveSystem, kSlot);
        const bool primaryValid = HasValidChecksum(primary);
        const bool loadedExpected = loaded && (*loaded == done || (inFlight && *loaded == done + 1));
        // A leftover temp file means its rename never ran, so the slot still holds `done`.
        const bool tempNotPromoted = !strayTemp || (inFlight && loaded && *loaded == done);

        // The retained copy is a complete save of the revision the last rename replaced.
        std::optional<uint32_t> retainedGeneration;
        const bool retainedExists = fs::exists(retained);
        if (retainedExists)
        {
            fs::copy_file(retained, probe, fs::copy_options::overwrite_existing);
            retainedGeneration = LoadGeneration(saveSystem, kProbeSlot);
            fs::remove(probe);
        }
        // Killed after the in-flight rename: it holds `done`. Killed before that rename:
        // `done - 1`, or `done` once the in-flight save already refreshed it.
        const bool retainedExpected =
            retainedGeneration && loaded &&
            (*loaded == done + 1 ? *retainedGeneration == done
                                 : (*retainedGeneration + 1 == done || (inFlight && *retainedGeneration == done)));

        if (listedSlots != 1u || !loadedExpected || !primaryValid || !tempNotPromoted || !retainedExpected)
        {
            dump("post-kill invariant violated");
            std::fprintf(stderr,
                         "[AtomicWrite] listed=%zu loaded=%d primaryCrc=%d strayTemp=%d retainedExists=%d "
                         "retained=%d\n",
                         listedSlots, loaded ? static_cast<int>(*loaded) : -1, primaryValid ? 1 : 0, strayTemp ? 1 : 0,
                         retainedExists ? 1 : 0, retainedGeneration ? static_cast<int>(*retainedGeneration) : -1);
        }
        EXPECT_EQ(listedSlots, size_t{1});
        ASSERT_TRUE(loadedExpected);
        EXPECT_TRUE(primaryValid);
        EXPECT_TRUE(tempNotPromoted);
        EXPECT_TRUE(retainedExpected);

        killsWithStrayTemp += strayTemp ? 1u : 0u;
        killsAfterInFlightRename += (inFlight && *loaded == done + 1) ? 1u : 0u;
        onDisk = *loaded;
    }
    std::printf("[AtomicWrite] %u kills: %u left a stray temp file, %u landed after the in-flight rename\n", iterations,
                killsWithStrayTemp, killsAfterInFlightRename);

    // The surviving process saves over whatever the last kill left behind and reads it back.
    const uint32_t finalGeneration = onDisk + 1;
    ASSERT_TRUE(SaveGeneration(saveSystem, kSlot, seedWorld, finalGeneration));
    EXPECT_FALSE(fs::exists(temporary));
    EXPECT_TRUE(HasValidChecksum(primary));
    EXPECT_TRUE(HasValidChecksum(retained));
    const std::optional<uint32_t> reloaded = LoadGeneration(saveSystem, kSlot);
    ASSERT_TRUE(reloaded.has_value());
    EXPECT_EQ(*reloaded, finalGeneration);

    fs::remove_all(directory);
}

// Deterministic guards for the torn-.bak fix. The SIGKILL rehearsal above hits the
// in-place-copy window only about 4 times in 1000 kills, so on its own it would rarely
// catch a revert of the staged copy. These two tests fail on every run if the retained
// copy is ever rewritten in place again.

TEST(AtomicWrite_RetentionRefreshReplacesTheBackupByRenameNotInPlace)
{
    const fs::path directory = UniqueScratchDirectory("retention_rename");
    fs::remove_all(directory);
    fs::create_directories(directory);
    const fs::path retained = directory / (std::string(kSlot) + ".spark_save.bak");

    SaveSystem& saveSystem = SaveSystem::GetInstance();
    ASSERT_TRUE(saveSystem.Initialize(directory.string()));
    World world;
    PopulateWorld(world);
    ASSERT_TRUE(SaveGeneration(saveSystem, kSlot, world, 1));
    ASSERT_TRUE(SaveGeneration(saveSystem, kSlot, world, 2));
    ASSERT_TRUE(fs::exists(retained));

    // Keep the old .bak inode alive through a hard link. A rename puts a new inode at the
    // .bak name and leaves the link holding generation 1; an in-place copy truncates and
    // rewrites the shared inode, so the link would change to generation 2.
    const fs::path previousRetained = directory / "previous_bak_link";
    fs::create_hard_link(retained, previousRetained);
    struct stat before
    {
    };
    ASSERT_EQ(::stat(retained.c_str(), &before), 0);
    const std::vector<char> previousBytes = ReadBytes(previousRetained);

    ASSERT_TRUE(SaveGeneration(saveSystem, kSlot, world, 3));

    struct stat after
    {
    };
    ASSERT_EQ(::stat(retained.c_str(), &after), 0);
    EXPECT_TRUE(before.st_ino != after.st_ino);
    EXPECT_TRUE(ReadBytes(previousRetained) == previousBytes);
    EXPECT_TRUE(HasValidChecksum(retained));
    fs::copy_file(retained, directory / (std::string(kProbeSlot) + ".spark_save"));
    const std::optional<uint32_t> retainedGeneration = LoadGeneration(saveSystem, kProbeSlot);
    ASSERT_TRUE(retainedGeneration.has_value());
    EXPECT_EQ(*retainedGeneration, 2u);

    fs::remove_all(directory);
}

TEST(AtomicWrite_FailedRetentionStagingLeavesBackupAndSlotUntouched)
{
    const fs::path directory = UniqueScratchDirectory("retention_failure");
    fs::remove_all(directory);
    fs::create_directories(directory);
    const fs::path primary = directory / (std::string(kSlot) + ".spark_save");
    const fs::path retained = directory / (std::string(kSlot) + ".spark_save.bak");
    const fs::path retainedStaging = directory / (std::string(kSlot) + ".spark_save.bak.tmp");
    const fs::path primaryStaging = directory / (std::string(kSlot) + ".spark_save.tmp");

    SaveSystem& saveSystem = SaveSystem::GetInstance();
    ASSERT_TRUE(saveSystem.Initialize(directory.string()));
    World world;
    PopulateWorld(world);
    ASSERT_TRUE(SaveGeneration(saveSystem, kSlot, world, 1));
    ASSERT_TRUE(SaveGeneration(saveSystem, kSlot, world, 2));
    const std::vector<char> primaryBytes = ReadBytes(primary);
    const std::vector<char> retainedBytes = ReadBytes(retained);

    // A non-empty directory at the staging name makes the staged copy fail. The save must
    // then abort before touching .bak or the slot; an in-place copy would have overwritten
    // .bak with generation 2 and gone on to write generation 3.
    fs::create_directories(retainedStaging / "blocker");
    EXPECT_FALSE(SaveGeneration(saveSystem, kSlot, world, 3));
    EXPECT_TRUE(ReadBytes(retained) == retainedBytes);
    EXPECT_TRUE(ReadBytes(primary) == primaryBytes);
    EXPECT_FALSE(fs::exists(primaryStaging));
    const std::optional<uint32_t> loaded = LoadGeneration(saveSystem, kSlot);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(*loaded, 2u);

    // With the obstruction gone the slot saves again, and DeleteSave removes the slot,
    // its retained copy and both staging names a killed writer can leave behind.
    fs::remove_all(retainedStaging);
    ASSERT_TRUE(SaveGeneration(saveSystem, kSlot, world, 3));
    std::ofstream(primaryStaging, std::ios::binary) << "orphaned primary staging";
    std::ofstream(retainedStaging, std::ios::binary) << "orphaned retained staging";
    ASSERT_TRUE(saveSystem.DeleteSave(kSlot));
    EXPECT_FALSE(fs::exists(primary));
    EXPECT_FALSE(fs::exists(retained));
    EXPECT_FALSE(fs::exists(primaryStaging));
    EXPECT_FALSE(fs::exists(retainedStaging));

    fs::remove_all(directory);
}
#endif
