/**
 * @file TestSecDaemonCacheHardening.cpp
 * @brief Security regression tests for the SparkDaemon shader and asset cache services.
 *
 * Covers:
 *   - Finding 78: cache files are validated against their real size before any
 *     header-declared length is allocated, oversized files are rejected, and a
 *     byte budget configured before Initialize bounds the startup load.
 *
 * The services are driven directly (no socket), so every test runs on all
 * platforms.
 */

#include "TestFramework.h"

#include "AssetService.h"
#include "ShaderService.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace
{
    using namespace Spark::Daemon;

    /// Fresh, empty scratch directory under the system temp directory.
    std::filesystem::path SecCacheDir(const char* tag)
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("spark-sec-cache-" + std::string(tag) + "-" + std::to_string(stamp));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    void RemoveDir(const std::filesystem::path& dir)
    {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    /// Write an `.asset` file: [u32 declaredPathLen LE][pathBytes][blobBytes].
    void WriteAssetFile(const std::filesystem::path& file, uint32_t declaredPathLen, const std::string& pathBytes,
                        size_t blobBytes)
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        const uint8_t lenBytes[4] = {
            static_cast<uint8_t>(declaredPathLen & 0xFFu),
            static_cast<uint8_t>((declaredPathLen >> 8) & 0xFFu),
            static_cast<uint8_t>((declaredPathLen >> 16) & 0xFFu),
            static_cast<uint8_t>((declaredPathLen >> 24) & 0xFFu),
        };
        out.write(reinterpret_cast<const char*>(lenBytes), sizeof(lenBytes));
        out.write(pathBytes.data(), static_cast<std::streamsize>(pathBytes.size()));
        const std::vector<char> blob(blobBytes, 'x');
        if (!blob.empty())
            out.write(blob.data(), static_cast<std::streamsize>(blob.size()));
    }

    void WriteRawFile(const std::filesystem::path& file, size_t bytes)
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        const std::vector<char> data(bytes, 's');
        if (!data.empty())
            out.write(data.data(), static_cast<std::streamsize>(data.size()));
    }

    std::string AssetName(unsigned index)
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%016x_000.asset", index);
        return buf;
    }

    std::string ShaderName(unsigned index)
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%016x_001_002.blob", index);
        return buf;
    }
} // namespace

// =========================================================================
// Finding 78: size-first cache file validation and startup budget
// =========================================================================

TEST(SecDaemonCache_AssetHeaderLongerThanFileIsRejected)
{
    // A four-byte file whose header declares a ~4 GiB path. The loader must
    // reject it from the file size alone instead of allocating the path first.
    const auto dir = SecCacheDir("asset-header");
    WriteAssetFile(dir / AssetName(1), 0xFFFFFFFFu, "", 0);
    // Declared length one byte past the end of the file.
    WriteAssetFile(dir / AssetName(2), 12u, "eleven-byte", 0);
    // Control entry that must still load.
    WriteAssetFile(dir / AssetName(3), 6u, "ok.png", 8);

    AssetService svc;
    const auto loaded = svc.Initialize(dir);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(*loaded, 1u);
    EXPECT_EQ(svc.GetEntryCount(), 1u);
    RemoveDir(dir);
}

TEST(SecDaemonCache_AssetFileLargerThanOneFrameIsRejected)
{
    // No PutAsset can produce an entry larger than one wire frame, so a bigger
    // file is corrupt or planted and must not be loaded into memory.
    const auto dir = SecCacheDir("asset-oversize");
    WriteAssetFile(dir / AssetName(1), 7u, "big.png", kMaxPayloadSize);
    WriteAssetFile(dir / AssetName(2), 9u, "small.png", 16);

    AssetService svc;
    const auto loaded = svc.Initialize(dir);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(*loaded, 1u);
    RemoveDir(dir);
}

TEST(SecDaemonCache_AssetBudgetSetBeforeInitializeBoundsStartupLoad)
{
    const auto dir = SecCacheDir("asset-budget");
    for (unsigned i = 1; i <= 4; ++i)
        WriteAssetFile(dir / AssetName(i), 5u, "p" + std::to_string(i) + ".ab", 100);

    AssetService svc;
    svc.SetMaxBytes(250);
    const auto loaded = svc.Initialize(dir);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(*loaded, 2u);
    EXPECT_EQ(svc.GetEntryCount(), 2u);
    RemoveDir(dir);
}

TEST(SecDaemonCache_ShaderFileLargerThanOneFrameIsRejected)
{
    const auto dir = SecCacheDir("shader-oversize");
    WriteRawFile(dir / ShaderName(1), static_cast<size_t>(kMaxPayloadSize) + 1);
    WriteRawFile(dir / ShaderName(2), 32);

    ShaderService svc;
    const auto loaded = svc.Initialize(dir);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(*loaded, 1u);
    EXPECT_EQ(svc.GetEntryCount(), 1u);
    RemoveDir(dir);
}

TEST(SecDaemonCache_ShaderBudgetSetBeforeInitializeBoundsStartupLoad)
{
    const auto dir = SecCacheDir("shader-budget");
    for (unsigned i = 1; i <= 4; ++i)
        WriteRawFile(dir / ShaderName(i), 100);

    ShaderService svc;
    svc.SetMaxBytes(250);
    const auto loaded = svc.Initialize(dir);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(*loaded, 2u);
    EXPECT_EQ(svc.GetEntryCount(), 2u);
    RemoveDir(dir);
}
