/**
 * @file TestSecDaemonCacheHardening.cpp
 * @brief Security regression tests for the SparkDaemon shader and asset cache services.
 *
 * Covers:
 *   - Finding 78: cache files are validated against their real size before any
 *     header-declared length is allocated, oversized files are rejected, and a
 *     byte budget configured before Initialize bounds the startup load.
 *   - Finding 79: a Put whose disk write fails reports an error and drops the
 *     entry, and concurrent mutations leave disk agreeing with memory.
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
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/resource.h>
#endif

namespace
{
    using namespace Spark::Daemon;

    /// The sanitizer wrapper caps the soft RLIMIT_FSIZE at 16 MiB, which silently
    /// truncates the oversize fixtures to exactly kMaxPayloadSize and makes them
    /// legal. Lift the soft limit to the hard limit while a fixture is written.
    class ScopedUnboundedFileSize
    {
      public:
        ScopedUnboundedFileSize()
        {
#if !defined(_WIN32)
            m_saved = ::getrlimit(RLIMIT_FSIZE, &m_previous) == 0;
            if (m_saved && m_previous.rlim_cur != m_previous.rlim_max)
            {
                rlimit raised = m_previous;
                raised.rlim_cur = m_previous.rlim_max;
                ::setrlimit(RLIMIT_FSIZE, &raised);
            }
#endif
        }
        ~ScopedUnboundedFileSize()
        {
#if !defined(_WIN32)
            if (m_saved)
                ::setrlimit(RLIMIT_FSIZE, &m_previous);
#endif
        }
        ScopedUnboundedFileSize(const ScopedUnboundedFileSize&) = delete;
        ScopedUnboundedFileSize& operator=(const ScopedUnboundedFileSize&) = delete;

      private:
#if !defined(_WIN32)
        rlimit m_previous{};
        bool m_saved = false;
#endif
    };

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
    {
        ScopedUnboundedFileSize unbounded;
        WriteAssetFile(dir / AssetName(1), 7u, "big.png", kMaxPayloadSize);
    }
    // A truncated fixture would be legal and prove nothing.
    ASSERT_EQ(std::filesystem::file_size(dir / AssetName(1)), 4u + 7u + kMaxPayloadSize);
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
    {
        ScopedUnboundedFileSize unbounded;
        WriteRawFile(dir / ShaderName(1), static_cast<size_t>(kMaxPayloadSize) + 1);
    }
    // A truncated fixture would be legal and prove nothing.
    ASSERT_EQ(std::filesystem::file_size(dir / ShaderName(1)), static_cast<std::uintmax_t>(kMaxPayloadSize) + 1u);
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

// =========================================================================
// Finding 79: persistence failures fail closed; disk follows memory order
// =========================================================================

namespace
{
    /// FNV-1a 64-bit, mirroring AssetService's on-disk file naming.
    uint64_t SecFnv1a(const std::string& text)
    {
        uint64_t hash = 0xCBF29CE484222325ull;
        for (unsigned char c : text)
        {
            hash ^= static_cast<uint64_t>(c);
            hash *= 0x100000001B3ull;
        }
        return hash;
    }

    std::string AssetFileFor(const std::string& path, unsigned platform)
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%016llx_%03u.asset", static_cast<unsigned long long>(SecFnv1a(path)),
                      platform);
        return buf;
    }

    /// Occupy a cache file name with a non-empty directory so the final rename fails.
    void BlockCacheFile(const std::filesystem::path& file)
    {
        std::filesystem::create_directories(file);
        std::ofstream(file / "occupant") << "x";
    }

    std::optional<ServiceResponse> SecAssetPut(AssetService& svc, const std::string& path, std::vector<uint8_t> blob)
    {
        PutAssetRequest req;
        req.key.path = path;
        req.key.platform = 0;
        req.blob = std::move(blob);
        return svc.HandleMessage(static_cast<uint16_t>(AssetMessage::PutAssetRequest), EncodePutAssetRequest(req));
    }

    std::optional<std::vector<uint8_t>> SecAssetGet(AssetService& svc, const std::string& path)
    {
        GetAssetRequest req;
        req.key.path = path;
        req.key.platform = 0;
        const auto resp =
            svc.HandleMessage(static_cast<uint16_t>(AssetMessage::GetAssetRequest), EncodeGetAssetRequest(req));
        GetAssetResponse out;
        if (!resp || !DecodeGetAssetResponse(resp->payload, out) || !out.found)
            return std::nullopt;
        return out.blob;
    }
} // namespace

TEST(SecDaemonCache_AssetPutDiskFailureReturnsErrorAndDropsEntry)
{
    const auto dir = SecCacheDir("asset-put-fail");
    AssetService svc;
    ASSERT_TRUE(svc.Initialize(dir).has_value());
    BlockCacheFile(dir / AssetFileFor("blocked.png", 0));

    const auto resp = SecAssetPut(svc, "blocked.png", std::vector<uint8_t>(16, 0xAB));
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->messageType, static_cast<uint16_t>(ControlMessage::ErrorResponse));
    EXPECT_FALSE(SecAssetGet(svc, "blocked.png").has_value());
    EXPECT_EQ(svc.GetEntryCount(), 0u);

    // A key whose file is writable still persists normally.
    const auto ok = SecAssetPut(svc, "fine.png", std::vector<uint8_t>(16, 0xCD));
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(ok->messageType, static_cast<uint16_t>(AssetMessage::PutAssetResponse));
    RemoveDir(dir);
}

TEST(SecDaemonCache_ShaderPutDiskFailureReturnsErrorAndDropsEntry)
{
    const auto dir = SecCacheDir("shader-put-fail");
    ShaderService svc;
    ASSERT_TRUE(svc.Initialize(dir).has_value());
    BlockCacheFile(dir / ShaderName(0x42));

    PutCacheEntryRequest req;
    req.key.sourceHash = 0x42;
    req.key.target = 1;
    req.key.stage = 2;
    req.blob = std::vector<uint8_t>(16, 0xEF);
    const auto resp =
        svc.HandleMessage(static_cast<uint16_t>(ShaderMessage::PutCacheEntryRequest), EncodePutCacheEntryRequest(req));
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->messageType, static_cast<uint16_t>(ControlMessage::ErrorResponse));
    EXPECT_EQ(svc.GetEntryCount(), 0u);
    RemoveDir(dir);
}

TEST(SecDaemonCache_AssetConcurrentMutationsLeaveDiskMatchingMemory)
{
    // Writers race each other and a clearer on one key. Whatever mutation was
    // last in memory must also be what a restarted daemon loads from disk.
    const auto dir = SecCacheDir("asset-race");
    AssetService svc;
    ASSERT_TRUE(svc.Initialize(dir).has_value());

    constexpr int kWriters = 4;
    constexpr int kPutsPerWriter = 100;
    std::vector<std::thread> threads;
    for (int writer = 0; writer < kWriters; ++writer)
    {
        threads.emplace_back(
            [&svc, writer]
            {
                for (int i = 0; i < kPutsPerWriter; ++i)
                {
                    const auto fill = static_cast<uint8_t>((writer * kPutsPerWriter + i) & 0xFF);
                    (void)SecAssetPut(svc, "race.png", std::vector<uint8_t>(256, fill));
                }
            });
    }
    threads.emplace_back(
        [&svc]
        {
            for (int i = 0; i < 50; ++i)
                (void)svc.HandleMessage(static_cast<uint16_t>(AssetMessage::ClearCacheRequest), {});
        });
    for (auto& thread : threads)
        thread.join();

    const auto inMemory = SecAssetGet(svc, "race.png");
    AssetService restarted;
    ASSERT_TRUE(restarted.Initialize(dir).has_value());
    const auto onDisk = SecAssetGet(restarted, "race.png");
    EXPECT_EQ(inMemory.has_value(), onDisk.has_value());
    if (inMemory && onDisk)
        EXPECT_TRUE(*inMemory == *onDisk);
    RemoveDir(dir);
}
