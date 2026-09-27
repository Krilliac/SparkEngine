// TestCrashArtifactRetention.cpp - SEC2 finding 15: the spark_crash_<pid>_<random>
// directories InstallCrashHandler() leaves in the temp directory are pruned by
// an owner-verified, age/count/byte-bounded policy (Utils/CrashArtifactRetention.h).

#include "TestFramework.h"
#include "Utils/CrashArtifactDirectory.h"
#include "Utils/CrashArtifactRetention.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace
{
    namespace fs = std::filesystem;
    using Spark::CrashHandlerDetail::CrashArtifactRetention;
    using Spark::CrashHandlerDetail::ParseCrashArtifactDirectoryName;
    using Spark::CrashHandlerDetail::PruneStaleCrashArtifactDirectories;

    // No process ever has this PID: it is beyond Linux pid_max and macOS
    // PID_MAX, and far above any PID Windows hands out.
    constexpr unsigned long kExitedPid = 2147483644UL;

    unsigned long CurrentPid()
    {
#ifdef _WIN32
        return static_cast<unsigned long>(_getpid());
#else
        return static_cast<unsigned long>(getpid());
#endif
    }

    class RetentionScratch
    {
      public:
        RetentionScratch()
        {
            const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
            path = fs::temp_directory_path() / ("spark-crash-retention-test-" + std::to_string(nonce));
            fs::create_directories(path);
        }
        ~RetentionScratch()
        {
            std::error_code error;
            fs::remove_all(path, error);
        }
        RetentionScratch(const RetentionScratch&) = delete;
        RetentionScratch& operator=(const RetentionScratch&) = delete;

        fs::path path;
    };

    std::string Suffix(unsigned index)
    {
        std::string suffix(32, '0');
        constexpr char hex[] = "0123456789abcdef";
        for (int digit = 31; digit >= 0 && index != 0; --digit, index >>= 4)
            suffix[static_cast<size_t>(digit)] = hex[index & 0xF];
        return suffix;
    }

    /// Create an owner-only artifact directory, optionally holding one file,
    /// and backdate it by @p age.
    fs::path MakeArtifactDirectory(const fs::path& base, unsigned long pid, unsigned index, std::uintmax_t bytes,
                                   std::chrono::hours age)
    {
        const fs::path directory = base / ("spark_crash_" + std::to_string(pid) + "_" + Suffix(index));
        if (!Spark::CrashHandlerDetail::TryCreatePrivateCrashArtifactDirectory(directory))
            return {};
        if (bytes != 0)
        {
            std::ofstream output(directory / "GameEngineCrash_0000000000000001.log", std::ios::binary);
            const std::string content(static_cast<size_t>(bytes), 'x');
            output.write(content.data(), static_cast<std::streamsize>(content.size()));
        }
        std::error_code error;
        fs::last_write_time(directory, fs::file_time_type::clock::now() - age, error);
        return error ? fs::path{} : directory;
    }
} // namespace

TEST(CrashRetention_ParsesOnlyGeneratedDirectoryNames)
{
    unsigned long pid = 0;
    EXPECT_TRUE(ParseCrashArtifactDirectoryName("spark_crash_4242_" + Suffix(7), pid));
    EXPECT_EQ(pid, 4242UL);
    EXPECT_FALSE(ParseCrashArtifactDirectoryName("spark_crash__" + Suffix(7), pid));
    EXPECT_FALSE(ParseCrashArtifactDirectoryName("spark_crash_12a_" + Suffix(7), pid));
    EXPECT_FALSE(ParseCrashArtifactDirectoryName("spark_crash_0_" + Suffix(7), pid));
    EXPECT_FALSE(ParseCrashArtifactDirectoryName("spark_crash_99999999999_" + Suffix(7), pid));
    EXPECT_FALSE(ParseCrashArtifactDirectoryName("spark_crash_12_" + Suffix(7).substr(1), pid));
    EXPECT_FALSE(ParseCrashArtifactDirectoryName("spark_crash_12_" + std::string(32, 'A'), pid));
    EXPECT_FALSE(ParseCrashArtifactDirectoryName("other_crash_12_" + Suffix(7), pid));
}

// The leaf name used to be read through a reference into the temporary that
// path::filename() returns, a heap-use-after-free once the name outgrew the
// small-string buffer (every generated name does). ASan flags that read, and
// the MSVC Debug CRT's 0xDD fill made every name fail the ASCII check, so
// the prune tests below removed nothing. This pins the converted value.
TEST(CrashRetention_LeafNameIsCopiedFromALiveFilename)
{
    namespace Detail = Spark::CrashHandlerDetail::Private;
    const std::string generated = "spark_crash_" + std::to_string(kExitedPid) + "_" + Suffix(0xABCDEF);
    std::string name = "stale";
    ASSERT_TRUE(Detail::TryGetAsciiLeafName(fs::temp_directory_path() / generated, name));
    EXPECT_EQ(name, generated);
    unsigned long pid = 0;
    EXPECT_TRUE(ParseCrashArtifactDirectoryName(name, pid));
    EXPECT_EQ(pid, kExitedPid);

    EXPECT_FALSE(Detail::TryGetAsciiLeafName(fs::path("base") / std::string(65, 'a'), name));
    EXPECT_FALSE(Detail::TryGetAsciiLeafName(fs::path("base") / fs::path(u8"spark_crash_é"), name));
}

TEST(CrashRetention_RemovesEmptyAndExpiredDirectoriesOfExitedProcesses)
{
    RetentionScratch scratch;
    const fs::path empty = MakeArtifactDirectory(scratch.path, kExitedPid, 1, 0, std::chrono::hours(0));
    const fs::path expired = MakeArtifactDirectory(scratch.path, kExitedPid, 2, 64, std::chrono::hours(24 * 30));
    const fs::path recent = MakeArtifactDirectory(scratch.path, kExitedPid, 3, 64, std::chrono::hours(1));
    // The running process (this test) keeps its directory even when empty.
    const fs::path live = MakeArtifactDirectory(scratch.path, CurrentPid(), 4, 0, std::chrono::hours(24 * 30));
    ASSERT_FALSE(empty.empty() || expired.empty() || recent.empty() || live.empty());
    // A name that does not match the generated pattern is never touched.
    const fs::path unrelated = scratch.path / "spark_crash_notes";
    fs::create_directories(unrelated);

    EXPECT_EQ(PruneStaleCrashArtifactDirectories(scratch.path), static_cast<std::size_t>(2));
    EXPECT_FALSE(fs::exists(empty));
    EXPECT_FALSE(fs::exists(expired));
    EXPECT_TRUE(fs::exists(recent));
    EXPECT_TRUE(fs::exists(recent / "GameEngineCrash_0000000000000001.log"));
    EXPECT_TRUE(fs::exists(live));
    EXPECT_TRUE(fs::exists(unrelated));
}

TEST(CrashRetention_KeepsOnlyTheNewestDirectoriesWithinCountAndByteBudgets)
{
    RetentionScratch scratch;
    fs::path directories[5];
    for (unsigned index = 0; index < 5; ++index)
    {
        // index 0 is the newest.
        directories[index] =
            MakeArtifactDirectory(scratch.path, kExitedPid, 10 + index, 100, std::chrono::hours(1 + index));
        ASSERT_FALSE(directories[index].empty());
    }

    CrashArtifactRetention byCount;
    byCount.maxDirectories = 3;
    EXPECT_EQ(PruneStaleCrashArtifactDirectories(scratch.path, byCount), static_cast<std::size_t>(2));
    EXPECT_TRUE(fs::exists(directories[0]) && fs::exists(directories[1]) && fs::exists(directories[2]));
    EXPECT_FALSE(fs::exists(directories[3]) || fs::exists(directories[4]));

    CrashArtifactRetention byBytes;
    byBytes.maxTotalBytes = 250; // room for two 100-byte directories
    EXPECT_EQ(PruneStaleCrashArtifactDirectories(scratch.path, byBytes), static_cast<std::size_t>(1));
    EXPECT_TRUE(fs::exists(directories[0]) && fs::exists(directories[1]));
    EXPECT_FALSE(fs::exists(directories[2]));
}

TEST(CrashRetention_NeverFollowsLinksOrRemovesForeignContent)
{
    RetentionScratch scratch;
    // A directory holding a subdirectory was not produced by the crash handler.
    const fs::path foreign = MakeArtifactDirectory(scratch.path, kExitedPid, 20, 0, std::chrono::hours(0));
    ASSERT_FALSE(foreign.empty());
    fs::create_directories(foreign / "nested");
    std::error_code error;
    fs::last_write_time(foreign, fs::file_time_type::clock::now() - std::chrono::hours(24 * 30), error);

    // A link named like an artifact directory must not be followed or removed.
    const fs::path target = scratch.path / "link-target";
    fs::create_directories(target);
    {
        std::ofstream keep(target / "keep.txt");
        keep << "must survive";
    }
    const fs::path link = scratch.path / ("spark_crash_" + std::to_string(kExitedPid) + "_" + Suffix(21));
    fs::create_directory_symlink(target, link, error);

    EXPECT_EQ(PruneStaleCrashArtifactDirectories(scratch.path), static_cast<std::size_t>(0));
    EXPECT_TRUE(fs::exists(foreign / "nested"));
    EXPECT_TRUE(fs::exists(target / "keep.txt"));
    if (!error)
        EXPECT_TRUE(fs::is_symlink(fs::symlink_status(link)));
}
