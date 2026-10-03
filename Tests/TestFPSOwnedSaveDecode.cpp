// Scoped native content verifier. Called only in its own SparkTests process by
// run_fps_owned_save_decode.py; never reads a game or user-profile directory.
#include "TestFramework.h"
#include "Engine/ECS/Components.h"
#include "Engine/SaveSystem/SaveSystem.h"
#include "Game/FPSQuickLoad.h"

#include <nlohmann_json.h>
#include <sodium.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>
#ifdef _WIN32
#include <Windows.h>
#endif

namespace
{
    namespace fs = std::filesystem;
    constexpr size_t kInputCap = 16 * 1024 * 1024;

    void RequirePlainPath(const fs::path& path)
    {
        ASSERT_TRUE(path.is_absolute());
        for (auto current = path; !current.empty(); current = current.parent_path())
        {
            ASSERT_FALSE(fs::is_symlink(fs::symlink_status(current)));
#ifdef _WIN32
            const auto attributes = GetFileAttributesW(current.c_str());
            ASSERT_TRUE(attributes != INVALID_FILE_ATTRIBUTES);
            ASSERT_TRUE((attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0);
#endif
            if (current == current.parent_path())
                break;
        }
    }

    void RequirePrimaryOnly(const fs::path& directory)
    {
        size_t count = 0;
        for (const auto& entry : fs::directory_iterator(directory))
        {
            ++count;
            ASSERT_TRUE(entry.path().filename() == "fps_quicksave.spark_save");
            RequirePlainPath(entry.path());
            ASSERT_TRUE(entry.is_regular_file());
        }
        ASSERT_EQ(count, static_cast<size_t>(1));
    }

    std::vector<unsigned char> ReadPrimary(const fs::path& path)
    {
        const auto size = fs::file_size(path);
        ASSERT_TRUE(size > 0 && size <= kInputCap);
        std::vector<unsigned char> bytes(static_cast<size_t>(size));
        std::ifstream input(path, std::ios::binary);
        ASSERT_TRUE(input.good());
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(input.good());
        ASSERT_EQ(input.peek(), std::char_traits<char>::eof());
        return bytes;
    }
} // namespace

TEST(FPSInputDispatch_DecodeOwnedPrimary)
{
    const char* configured = std::getenv("SPARK_FPS_DECODE_DIRECTORY");
    if (!configured || !*configured)
        SKIP_TEST("Requires a copied primary in an explicitly owned decoder directory");
    const std::string directoryText(configured);
    // SaveSystem's path API is narrow; avoid lossy Windows path conversion.
    for (unsigned char character : directoryText)
        ASSERT_TRUE(character > 0 && character < 128);
    const fs::path directory(directoryText);
    ASSERT_TRUE(directory == directory.lexically_normal());
    RequirePlainPath(directory);
    ASSERT_TRUE(fs::is_directory(directory));
    RequirePrimaryOnly(directory);
    const auto primary = directory / "fps_quicksave.spark_save";
    const auto before = ReadPrimary(primary);
    ASSERT_TRUE(sodium_init() >= 0);
    std::array<unsigned char, crypto_hash_sha256_BYTES> hash{};
    ASSERT_EQ(crypto_hash_sha256(hash.data(), before.data(), before.size()), 0);
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> digest{};
    sodium_bin2hex(digest.data(), digest.size(), hash.data(), hash.size());

    auto& saves = Spark::SaveSystem::GetInstance();
    saves.SetFileCache(nullptr);
    ASSERT_TRUE(saves.Initialize(directoryText));
    ASSERT_EQ(saves.GetSaveDirectory(), directoryText);
    World scratch(World::EntityEventCleanupMode::Suppressed);
    Spark::FPSLocalProfile profile;
    std::string error;
    ASSERT_TRUE(Spark::LoadSlotWithProfile(saves, "fps_quicksave", scratch, profile, error) ==
                Spark::FPSQuickLoadStatus::Loaded);
    std::unordered_map<std::string, std::string> canonical;
    profile.WriteTo(canonical); // Existing persisted-profile schema and float precision.
    RequirePrimaryOnly(directory);
    ASSERT_TRUE(ReadPrimary(primary) == before);
    nlohmann::json receipt = {
        {"inputSha256", digest.data()}, {"inputBytes", before.size()}, {"canonicalProfile", canonical}};
    const auto text = receipt.dump();
    ASSERT_TRUE(!text.empty() && text.size() <= 16384);
    ASSERT_TRUE(std::printf("SPARK_FPS_SAVE_DECODE %s\n", text.c_str()) > 0);
    ASSERT_EQ(std::fflush(stdout), 0);
}
