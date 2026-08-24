/**
 * @file TestTFSocialStore.cpp
 * @brief Fail-closed schema and corruption-recovery tests for TERRAFRONT social persistence.
 */
#include "TestFramework.h"

#include "Game/TFSocialSystem.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

using Terrafront::TFSocialSystem;

static_assert(std::is_same_v<decltype(&TFSocialSystem::Shutdown), bool (TFSocialSystem::*)()>);

namespace
{
    namespace fs = std::filesystem;

    fs::path TempStorePath(const char* stem)
    {
        static std::atomic_uint64_t sequence{0};
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        return fs::temp_directory_path() / (std::string("spark_social_") + stem + "_" + std::to_string(tick) + "_" +
                                            std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + ".json");
    }

    std::string ReadText(const fs::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        std::ostringstream stream;
        stream << input.rdbuf();
        return stream.str();
    }

    std::vector<fs::path> RecoveryBackups(const fs::path& path)
    {
        std::vector<fs::path> backups;
        const auto prefix = (path.filename().string() + ".corrupt-");
        std::error_code ec;
        for (fs::directory_iterator it(path.parent_path(), ec), end; !ec && it != end; it.increment(ec))
        {
            const std::string name = it->path().filename().string();
            if (name.starts_with(prefix) && name.ends_with(".bak"))
                backups.push_back(it->path());
        }
        return backups;
    }

    void RemoveStoreArtifacts(const fs::path& path)
    {
        std::error_code ec;
        fs::remove(path, ec);
        fs::path temporary = path;
        temporary += ".tmp";
        fs::remove_all(temporary, ec);
        for (const fs::path& backup : RecoveryBackups(path))
            fs::remove(backup, ec);
    }

    std::string OneRecord(const std::string& charId = "42", const std::string& lastSeenMs = "1700000000000")
    {
        return "{\"characters\":[{\"charId\":" + charId +
               ",\"friends\":[\"Alice One\"],\"blocked\":[\"Bob2\"],"
               "\"recent\":[{\"name\":\"Carol 3\",\"lastSeenMs\":" +
               lastSeenMs + "}]}]}";
    }
} // namespace

TEST(TFSocialStore_CompleteSchemaLoadsAtomically)
{
    const std::string valid =
        R"({"characters":[{"charId":42,"friends":["Alice One"],"blocked":["Bob2"],"recent":[{"name":"Carol 3","lastSeenMs":1700000000000}]},{"charId":43,"friends":[],"blocked":[],"recent":[]}]})";
    std::string detail;
    EXPECT_TRUE(TFSocialSystem::ValidateStoreJsonForTesting(valid, &detail));
    EXPECT_TRUE(detail.empty());

    const fs::path path = TempStorePath("valid");
    RemoveStoreArtifacts(path);
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << valid;
    }
    const auto result = TFSocialSystem::LoadStoreForTesting(path);
    EXPECT_TRUE(result.accepted);
    EXPECT_FALSE(result.missing);
    EXPECT_EQ(result.recordCount, size_t{2});
    RemoveStoreArtifacts(path);
}

TEST(TFSocialStore_InvalidRowsAndFieldsRejectTheWholeDocument)
{
    const std::vector<std::string> invalid = {
        R"({"characters":[null]})",
        R"({"characters":[{"charId":1,"friends":[],"blocked":[]}]})",
        R"({"characters":[{"charId":1,"friends":[],"blocked":[],"recent":[],"unknown":true}]})",
        R"({"characters":[{"charId":1,"friends":[7],"blocked":[],"recent":[]}]})",
        R"({"characters":[{"charId":1,"friends":["A"],"blocked":[],"recent":[]}]})",
        R"({"characters":[{"charId":1,"friends":[],"blocked":[],"recent":[{"name":"Valid Name"}]}]})",
        R"({"characters":[{"charId":1,"friends":["Same Name","same name"],"blocked":[],"recent":[]}]})",
        R"({"characters":[{"charId":1,"friends":["Same Name"],"blocked":["same name"],"recent":[]}]})",
        R"({"characters":[],"unknown":0})",
        R"({"characters":[],"characters":[]})",
        R"({"characters":[{"charId":1,"charId":2,"friends":[],"blocked":[],"recent":[]}]})",
        R"({"characters":[{"charId":1,"friends":[],"blocked":[],"recent":[{"name":"Valid Name","name":"Other Name","lastSeenMs":1}]}]})",
    };

    for (const std::string& document : invalid)
        EXPECT_FALSE(TFSocialSystem::ValidateStoreJsonForTesting(document));
}

TEST(TFSocialStore_DuplicateCharacterIdsRejectTheWholeDocument)
{
    const std::string duplicate =
        R"({"characters":[{"charId":42,"friends":[],"blocked":[],"recent":[]},{"charId":42,"friends":[],"blocked":[],"recent":[]}]})";
    EXPECT_FALSE(TFSocialSystem::ValidateStoreJsonForTesting(duplicate));
}

TEST(TFSocialStore_NumericsRequireSafeCanonicalIntegers)
{
    const std::vector<std::string> invalid = {
        OneRecord("1.5"),
        OneRecord("1e3"),
        OneRecord("9007199254740992"),
        OneRecord("42", "-0"),
        OneRecord("42", "1.5"),
        OneRecord("42", "1700000000000.00001"),
        OneRecord("42", "1e3"),
        OneRecord("42", "9007199254740992"),
        OneRecord("42", "1e999"),
    };

    for (const std::string& document : invalid)
        EXPECT_FALSE(TFSocialSystem::ValidateStoreJsonForTesting(document));

    EXPECT_TRUE(TFSocialSystem::ValidateStoreJsonForTesting(OneRecord("9007199254740991", "9007199254740991")));
}

TEST(TFSocialStore_CorruptPrimarySurvivesRestartWithRecoveryBackups)
{
    const fs::path path = TempStorePath("corrupt_restart");
    RemoveStoreArtifacts(path);
    const std::string corrupt = OneRecord("42", "1700000000000.00001");
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << corrupt;
    }

    const auto first = TFSocialSystem::LoadStoreForTesting(path);
    EXPECT_FALSE(first.accepted);
    EXPECT_TRUE(fs::exists(path));
    EXPECT_TRUE(ReadText(path) == corrupt);
    const auto firstBackups = RecoveryBackups(path);
    EXPECT_EQ(firstBackups.size(), size_t{1});
    if (!firstBackups.empty())
        EXPECT_TRUE(ReadText(firstBackups.front()) == corrupt);

    // A new loader sees the still-corrupt primary and fails closed again.
    const auto afterRestart = TFSocialSystem::LoadStoreForTesting(path);
    EXPECT_FALSE(afterRestart.accepted);
    EXPECT_TRUE(fs::exists(path));
    EXPECT_TRUE(ReadText(path) == corrupt);
    EXPECT_GT(RecoveryBackups(path).size(), size_t{1});
    RemoveStoreArtifacts(path);
}

TEST(TFSocialStore_MissingPrimaryWithRecoveryBackupFailsClosed)
{
    const fs::path path = TempStorePath("missing_primary");
    RemoveStoreArtifacts(path);
    fs::path backup = path;
    backup += ".corrupt-manual.bak";
    {
        std::ofstream output(backup, std::ios::binary | std::ios::trunc);
        output << OneRecord();
    }

    const auto result = TFSocialSystem::LoadStoreForTesting(path);
    EXPECT_FALSE(result.accepted);
    EXPECT_FALSE(result.missing);
    EXPECT_STR_CONTAINS(result.detail, "primary is missing");
    RemoveStoreArtifacts(path);
}

TEST(TFSocialStore_GenuinelyMissingStoreStartsEmpty)
{
    const fs::path path = TempStorePath("missing_empty");
    RemoveStoreArtifacts(path);
    const auto result = TFSocialSystem::LoadStoreForTesting(path);
    EXPECT_TRUE(result.accepted);
    EXPECT_TRUE(result.missing);
    EXPECT_EQ(result.recordCount, size_t{0});
    RemoveStoreArtifacts(path);
}
