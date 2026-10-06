// TestSparkPak.cpp — Unit tests for SparkPak archive format (read, write, compress, VFS)
// Standalone test: writes temp archives, reads them back, verifies round-trip correctness.

#include "TestFilesystemLinks.h"
#include "TestFramework.h"
#include "Core/SparkPak.h"
#include "Core/SparkPakWriter.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

// ============================================================================
// Standalone reimplementation of SparkPak core types for test isolation
// ============================================================================

namespace
{

    constexpr uint32_t kMagic = 0x314B5053; // "SPK1"
    constexpr uint32_t kVersion = 1;

    enum class Compression : uint8_t
    {
        Stored = 0,
        Deflate = 1,
    };

#pragma pack(push, 1)
    struct Header
    {
        uint32_t magic = kMagic;
        uint32_t version = kVersion;
        uint32_t fileCount = 0;
        uint32_t reserved1 = 0;
        uint64_t tocOffset = 0;
        uint32_t tocSize = 0;
        uint32_t tocRawSize = 0;
    };
#pragma pack(pop)

    constexpr uint64_t FNV1a(std::string_view str)
    {
        constexpr uint64_t kOffsetBasis = 14695981039346656037ULL;
        constexpr uint64_t kPrime = 1099511628211ULL;
        uint64_t h = kOffsetBasis;
        for (char c : str)
        {
            h ^= static_cast<uint64_t>(c);
            h *= kPrime;
        }
        return h;
    }

    // Minimal archive writer for testing (no compression, stored-only)
    class TestPakWriter
    {
      public:
        void AddFile(const std::string& virtualPath, const std::vector<uint8_t>& data)
        {
            m_files.push_back({virtualPath, data});
        }

        bool Write(const std::string& path)
        {
            FILE* f = std::fopen(path.c_str(), "wb");
            if (!f)
                return false;

            Header hdr;
            hdr.fileCount = static_cast<uint32_t>(m_files.size());
            std::fwrite(&hdr, sizeof(Header), 1, f);

            struct TOCEntry
            {
                uint64_t hash, offset;
                uint32_t compSize, origSize;
                std::string path;
            };
            std::vector<TOCEntry> toc;

            for (const auto& [vpath, data] : m_files)
            {
                TOCEntry e;
                e.hash = FNV1a(vpath);
                e.offset = static_cast<uint64_t>(std::ftell(f));
                e.compSize = static_cast<uint32_t>(data.size());
                e.origSize = static_cast<uint32_t>(data.size());
                e.path = vpath;
                if (!data.empty())
                {
                    std::fwrite(data.data(), 1, data.size(), f);
                }
                toc.push_back(std::move(e));
            }

            // Serialize TOC (uncompressed)
            std::vector<uint8_t> tocRaw;
            for (const auto& e : toc)
            {
                auto pathLen = static_cast<uint16_t>(e.path.size());
                size_t off = tocRaw.size();
                tocRaw.resize(off + 27 + pathLen);
                uint8_t* p = tocRaw.data() + off;
                std::memcpy(p, &e.hash, 8);
                p += 8;
                std::memcpy(p, &e.offset, 8);
                p += 8;
                std::memcpy(p, &e.compSize, 4);
                p += 4;
                std::memcpy(p, &e.origSize, 4);
                p += 4;
                *p = 0; // Stored
                p += 1;
                std::memcpy(p, &pathLen, 2);
                p += 2;
                std::memcpy(p, e.path.data(), pathLen);
            }

            hdr.tocOffset = static_cast<uint64_t>(std::ftell(f));
            hdr.tocSize = static_cast<uint32_t>(tocRaw.size());
            hdr.tocRawSize = hdr.tocSize; // uncompressed TOC
            if (!tocRaw.empty())
            {
                std::fwrite(tocRaw.data(), 1, tocRaw.size(), f);
            }

            // Rewrite header
            std::fseek(f, 0, SEEK_SET);
            std::fwrite(&hdr, sizeof(Header), 1, f);
            std::fclose(f);
            return true;
        }

      private:
        struct Entry
        {
            std::string path;
            std::vector<uint8_t> data;
        };
        std::vector<Entry> m_files;
    };

    // Minimal archive reader for testing
    class TestPakReader
    {
      public:
        struct TOCEntry
        {
            uint64_t hash = 0;
            uint64_t offset = 0;
            uint32_t compSize = 0;
            uint32_t origSize = 0;
            Compression compression = Compression::Stored;
            std::string path;
        };

        TestPakReader() = default;
        TestPakReader(const TestPakReader&) = delete;
        TestPakReader& operator=(const TestPakReader&) = delete;

        ~TestPakReader()
        {
            if (m_file)
                std::fclose(m_file);
        }

        bool Open(const std::string& path)
        {
            // Release any archive opened by a previous call before reopening.
            if (m_file)
            {
                std::fclose(m_file);
                m_entries.clear();
            }
            m_file = std::fopen(path.c_str(), "rb");
            if (!m_file)
                return false;
            if (std::fread(&m_header, sizeof(Header), 1, m_file) != 1)
                return false;
            if (m_header.magic != kMagic || m_header.version != kVersion)
                return false;

            // Read TOC
            std::fseek(m_file, static_cast<long>(m_header.tocOffset), SEEK_SET);
            std::vector<uint8_t> tocRaw(m_header.tocRawSize);
            if (std::fread(tocRaw.data(), 1, m_header.tocRawSize, m_file) != m_header.tocRawSize)
                return false;

            const uint8_t* p = tocRaw.data();
            const uint8_t* end = p + tocRaw.size();
            for (uint32_t i = 0; i < m_header.fileCount && p + 27 <= end; ++i)
            {
                TOCEntry e;
                std::memcpy(&e.hash, p, 8);
                p += 8;
                std::memcpy(&e.offset, p, 8);
                p += 8;
                std::memcpy(&e.compSize, p, 4);
                p += 4;
                std::memcpy(&e.origSize, p, 4);
                p += 4;
                e.compression = static_cast<Compression>(*p);
                p += 1;
                uint16_t pathLen = 0;
                std::memcpy(&pathLen, p, 2);
                p += 2;
                if (p + pathLen > end)
                    break;
                e.path.assign(reinterpret_cast<const char*>(p), pathLen);
                p += pathLen;
                m_entries[e.hash] = std::move(e);
            }
            return true;
        }

        bool Exists(const std::string& vpath) const { return m_entries.contains(FNV1a(vpath)); }

        std::vector<uint8_t> ReadFile(const std::string& vpath) const
        {
            auto it = m_entries.find(FNV1a(vpath));
            if (it == m_entries.end())
                return {};
            const auto& e = it->second;
            std::fseek(m_file, static_cast<long>(e.offset), SEEK_SET);
            std::vector<uint8_t> data(e.compSize);
            if (std::fread(data.data(), 1, e.compSize, m_file) != e.compSize)
                return {};
            return data;
        }

        uint32_t GetEntryCount() const { return static_cast<uint32_t>(m_entries.size()); }
        const Header& GetHeader() const { return m_header; }

        std::vector<std::string> ListFiles(const std::string& dir) const
        {
            std::vector<std::string> result;
            for (const auto& [hash, entry] : m_entries)
            {
                if (dir.empty() || entry.path.find(dir) == 0)
                    result.push_back(entry.path);
            }
            return result;
        }

      private:
        FILE* m_file = nullptr;
        Header m_header{};
        std::unordered_map<uint64_t, TOCEntry> m_entries;
    };

    std::filesystem::path TempRoot()
    {
#if defined(_WIN32)
        const auto processId = static_cast<unsigned long>(::_getpid());
#else
        const auto processId = static_cast<unsigned long>(::getpid());
#endif
        return std::filesystem::temp_directory_path() / ("sparkpak_tests_" + std::to_string(processId));
    }

    std::string TempPath(const std::string& name)
    {
        const auto dir = TempRoot();
        std::filesystem::create_directories(dir);
        return (dir / name).string();
    }

    void Cleanup()
    {
        std::error_code ec;
        std::filesystem::remove_all(TempRoot(), ec);
    }

} // anonymous namespace

// ============================================================================
// Tests
// ============================================================================

TEST(SparkPak_HeaderSize)
{
    EXPECT_EQ(sizeof(Header), 32u);
}

TEST(SparkPak_FNV1aConsistency)
{
    // Same string should always produce the same hash
    auto h1 = FNV1a("textures/brick.png");
    auto h2 = FNV1a("textures/brick.png");
    EXPECT_EQ(h1, h2);

    // Different strings should produce different hashes
    auto h3 = FNV1a("textures/wood.png");
    EXPECT_NE(h1, h3);

    // Empty string has a known hash (the offset basis)
    auto h4 = FNV1a("");
    EXPECT_EQ(h4, 14695981039346656037ULL);
}

TEST(SparkPak_RoundTrip_SingleFile)
{
    auto path = TempPath("single.spk");

    std::vector<uint8_t> content = {0x48, 0x65, 0x6C, 0x6C, 0x6F}; // "Hello"

    TestPakWriter writer;
    writer.AddFile("greeting.txt", content);
    EXPECT_TRUE(writer.Write(path));

    TestPakReader reader;
    EXPECT_TRUE(reader.Open(path));
    EXPECT_EQ(reader.GetEntryCount(), 1u);
    EXPECT_TRUE(reader.Exists("greeting.txt"));
    EXPECT_FALSE(reader.Exists("nonexistent.txt"));

    auto readBack = reader.ReadFile("greeting.txt");
    EXPECT_EQ(readBack.size(), content.size());
    EXPECT_TRUE(readBack == content);

    Cleanup();
}

TEST(SparkPak_RoundTrip_MultipleFiles)
{
    auto path = TempPath("multi.spk");

    std::vector<uint8_t> data1 = {1, 2, 3, 4, 5};
    std::vector<uint8_t> data2 = {10, 20, 30};
    std::vector<uint8_t> data3 = {0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA};

    TestPakWriter writer;
    writer.AddFile("textures/a.png", data1);
    writer.AddFile("models/b.obj", data2);
    writer.AddFile("audio/c.wav", data3);
    EXPECT_TRUE(writer.Write(path));

    TestPakReader reader;
    EXPECT_TRUE(reader.Open(path));
    EXPECT_EQ(reader.GetEntryCount(), 3u);

    EXPECT_TRUE(reader.ReadFile("textures/a.png") == data1);
    EXPECT_TRUE(reader.ReadFile("models/b.obj") == data2);
    EXPECT_TRUE(reader.ReadFile("audio/c.wav") == data3);

    Cleanup();
}

TEST(SparkPak_EmptyArchive)
{
    auto path = TempPath("empty.spk");

    TestPakWriter writer;
    EXPECT_TRUE(writer.Write(path));

    TestPakReader reader;
    EXPECT_TRUE(reader.Open(path));
    EXPECT_EQ(reader.GetEntryCount(), 0u);
    EXPECT_FALSE(reader.Exists("anything"));

    Cleanup();
}

TEST(SparkPak_EmptyFile)
{
    auto path = TempPath("emptyfile.spk");

    TestPakWriter writer;
    writer.AddFile("empty.txt", {});
    EXPECT_TRUE(writer.Write(path));

    TestPakReader reader;
    EXPECT_TRUE(reader.Open(path));
    EXPECT_TRUE(reader.Exists("empty.txt"));

    auto data = reader.ReadFile("empty.txt");
    EXPECT_EQ(data.size(), 0u);

    Cleanup();
}

TEST(SparkPak_ListFiles)
{
    auto path = TempPath("listing.spk");

    TestPakWriter writer;
    writer.AddFile("textures/a.png", {1});
    writer.AddFile("textures/b.png", {2});
    writer.AddFile("models/c.obj", {3});
    EXPECT_TRUE(writer.Write(path));

    TestPakReader reader;
    EXPECT_TRUE(reader.Open(path));

    auto texFiles = reader.ListFiles("textures/");
    EXPECT_EQ(texFiles.size(), 2u);

    auto allFiles = reader.ListFiles("");
    EXPECT_EQ(allFiles.size(), 3u);

    auto modelFiles = reader.ListFiles("models/");
    EXPECT_EQ(modelFiles.size(), 1u);

    Cleanup();
}

TEST(SparkPak_InvalidMagic)
{
    auto path = TempPath("badmagic.spk");

    // Write garbage
    std::ofstream ofs(path, std::ios::binary);
    uint32_t badMagic = 0xDEADBEEF;
    ofs.write(reinterpret_cast<const char*>(&badMagic), 4);
    ofs.close();

    TestPakReader reader;
    EXPECT_FALSE(reader.Open(path));

    Cleanup();
}

TEST(SparkPak_LargeFile)
{
    auto path = TempPath("large.spk");

    // 1MB file
    std::vector<uint8_t> bigData(1024 * 1024);
    for (size_t i = 0; i < bigData.size(); ++i)
        bigData[i] = static_cast<uint8_t>(i & 0xFF);

    TestPakWriter writer;
    writer.AddFile("data/large.bin", bigData);
    EXPECT_TRUE(writer.Write(path));

    TestPakReader reader;
    EXPECT_TRUE(reader.Open(path));
    auto readBack = reader.ReadFile("data/large.bin");
    EXPECT_EQ(readBack.size(), bigData.size());
    EXPECT_TRUE(readBack == bigData);

    Cleanup();
}

TEST(SparkPak_HeaderValidation)
{
    auto path = TempPath("headerval.spk");

    TestPakWriter writer;
    writer.AddFile("a.txt", {65, 66, 67});
    writer.Write(path);

    TestPakReader reader;
    reader.Open(path);

    EXPECT_EQ(reader.GetHeader().magic, kMagic);
    EXPECT_EQ(reader.GetHeader().version, kVersion);
    EXPECT_EQ(reader.GetHeader().fileCount, 1u);

    Cleanup();
}

TEST(SparkPak_NonexistentFile)
{
    TestPakReader reader;
    EXPECT_FALSE(reader.Open("/tmp/sparkpak_tests/does_not_exist.spk"));
}

TEST(SparkPak_ReadMissingEntry)
{
    auto path = TempPath("missing_entry.spk");

    TestPakWriter writer;
    writer.AddFile("exists.txt", {1, 2, 3});
    writer.Write(path);

    TestPakReader reader;
    reader.Open(path);

    auto data = reader.ReadFile("does_not_exist.txt");
    EXPECT_EQ(data.size(), 0u);

    Cleanup();
}

// ============================================================================
// Production SparkPak hardening regressions
// ============================================================================

TEST(SparkPak_ProductionRejectsTinyArchiveWith2GiBEntryDeclaration)
{
    const auto path = TempPath("declared_2gib.spk");
    const std::string virtualPath = "huge.bin";

    Spark::PakHeader header;
    header.fileCount = 1;
    header.tocOffset = sizeof(Spark::PakHeader) + 1; // Physical data is one byte.
    header.tocSize = static_cast<uint32_t>(27 + virtualPath.size());
    header.tocRawSize = header.tocSize;

    std::vector<uint8_t> toc(header.tocSize);
    uint8_t* cursor = toc.data();
    const uint64_t hash = Spark::PakFNV1a(virtualPath);
    const uint64_t dataOffset = sizeof(Spark::PakHeader);
    const uint32_t declaredSize = 2u * 1024u * 1024u * 1024u;
    const uint8_t compression = static_cast<uint8_t>(Spark::PakCompression::Stored);
    const uint16_t pathLen = static_cast<uint16_t>(virtualPath.size());
    std::memcpy(cursor, &hash, sizeof(hash));
    cursor += sizeof(hash);
    std::memcpy(cursor, &dataOffset, sizeof(dataOffset));
    cursor += sizeof(dataOffset);
    std::memcpy(cursor, &declaredSize, sizeof(declaredSize));
    cursor += sizeof(declaredSize);
    std::memcpy(cursor, &declaredSize, sizeof(declaredSize));
    cursor += sizeof(declaredSize);
    std::memcpy(cursor, &compression, sizeof(compression));
    cursor += sizeof(compression);
    std::memcpy(cursor, &pathLen, sizeof(pathLen));
    cursor += sizeof(pathLen);
    std::memcpy(cursor, virtualPath.data(), virtualPath.size());

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        const uint8_t oneByte = 0x5a;
        out.write(reinterpret_cast<const char*>(&oneByte), 1);
        out.write(reinterpret_cast<const char*>(toc.data()), static_cast<std::streamsize>(toc.size()));
    }

    Spark::SparkPakReader reader;
    EXPECT_FALSE(reader.Open(path));
    Cleanup();
}

TEST(SparkPak_ProductionRejectsUnsafeTOCEntryPath)
{
    const auto path = TempPath("unsafe_path.spk");
    const std::string virtualPath = "../outside.bin";

    Spark::PakHeader header;
    header.fileCount = 1;
    header.tocOffset = sizeof(Spark::PakHeader) + 1;
    header.tocSize = static_cast<uint32_t>(27 + virtualPath.size());
    header.tocRawSize = header.tocSize;

    std::vector<uint8_t> toc(header.tocSize);
    uint8_t* cursor = toc.data();
    const uint64_t hash = Spark::PakFNV1a(virtualPath);
    const uint64_t dataOffset = sizeof(Spark::PakHeader);
    const uint32_t size = 1;
    const uint8_t compression = static_cast<uint8_t>(Spark::PakCompression::Stored);
    const uint16_t pathLen = static_cast<uint16_t>(virtualPath.size());
    std::memcpy(cursor, &hash, sizeof(hash));
    cursor += sizeof(hash);
    std::memcpy(cursor, &dataOffset, sizeof(dataOffset));
    cursor += sizeof(dataOffset);
    std::memcpy(cursor, &size, sizeof(size));
    cursor += sizeof(size);
    std::memcpy(cursor, &size, sizeof(size));
    cursor += sizeof(size);
    std::memcpy(cursor, &compression, sizeof(compression));
    cursor += sizeof(compression);
    std::memcpy(cursor, &pathLen, sizeof(pathLen));
    cursor += sizeof(pathLen);
    std::memcpy(cursor, virtualPath.data(), virtualPath.size());

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        const uint8_t payload = 0x5a;
        out.write(reinterpret_cast<const char*>(&payload), 1);
        out.write(reinterpret_cast<const char*>(toc.data()), static_cast<std::streamsize>(toc.size()));
    }

    Spark::SparkPakReader reader;
    EXPECT_FALSE(reader.Open(path));
    Cleanup();
}

TEST(SparkPak_ProductionRefusesHostileCompressionRatioPerEntry)
{
    // A ~1 KB archive whose TOC declares a 100-byte deflate entry expanding to
    // 2 GB. ReadFile allocated entry.originalSize BEFORE decompression could fail,
    // so a handful of such entries demanded gigabytes from an asset-loading path
    // with no bad_alloc handler. The per-entry decompression budget refuses the
    // bomb at ReadFile; the archive itself still opens, so a hostile entry cannot
    // unmount every other asset in the pak.
    const auto path = TempPath("hostile_ratio.spk");
    const std::string virtualPath = "bomb.bin";
    const uint32_t compressedSize = 100;

    Spark::PakHeader header;
    header.fileCount = 1;
    header.tocOffset = sizeof(Spark::PakHeader) + compressedSize;
    header.tocSize = static_cast<uint32_t>(27 + virtualPath.size());
    header.tocRawSize = header.tocSize;

    std::vector<uint8_t> toc(header.tocSize);
    uint8_t* cursor = toc.data();
    const uint64_t hash = Spark::PakFNV1a(virtualPath);
    const uint64_t dataOffset = sizeof(Spark::PakHeader);
    const uint32_t originalSize = 2147483647u;
    const uint8_t compression = static_cast<uint8_t>(Spark::PakCompression::Deflate);
    const uint16_t pathLen = static_cast<uint16_t>(virtualPath.size());
    std::memcpy(cursor, &hash, sizeof(hash));
    cursor += sizeof(hash);
    std::memcpy(cursor, &dataOffset, sizeof(dataOffset));
    cursor += sizeof(dataOffset);
    std::memcpy(cursor, &compressedSize, sizeof(compressedSize));
    cursor += sizeof(compressedSize);
    std::memcpy(cursor, &originalSize, sizeof(originalSize));
    cursor += sizeof(originalSize);
    std::memcpy(cursor, &compression, sizeof(compression));
    cursor += sizeof(compression);
    std::memcpy(cursor, &pathLen, sizeof(pathLen));
    cursor += sizeof(pathLen);
    std::memcpy(cursor, virtualPath.data(), virtualPath.size());

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        const std::vector<uint8_t> payload(compressedSize, 0x00);
        out.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
        out.write(reinterpret_cast<const char*>(toc.data()), static_cast<std::streamsize>(toc.size()));
    }

    Spark::SparkPakReader reader;
    EXPECT_TRUE(reader.Open(path));
    EXPECT_TRUE(reader.Exists("bomb.bin"));
    EXPECT_TRUE(reader.ReadFile("bomb.bin").empty());
    Cleanup();
}

TEST(SparkPak_ProductionReadsExtremelyCompressibleEntries)
{
    // Regression: the archive-wide 1000:1 cap failed Open() for content the cooker
    // legitimately produces. 1 MB of zeros (a blank lightmap, a padded heightmap, a
    // zeroed vertex buffer) deflates at roughly deflate's 1032:1 ceiling, so the old
    // cap unmounted the WHOLE archive — including the ordinary asset beside it.
    const auto path = TempPath("compressible.spk");
    const std::vector<uint8_t> zeros(1024 * 1024, 0x00);
    std::vector<uint8_t> ordinary(4096);
    for (size_t i = 0; i < ordinary.size(); ++i)
        ordinary[i] = static_cast<uint8_t>(i * 7 + 3);

    Spark::SparkPakWriter writer;
    writer.AddFile("blank_lightmap.bin", zeros, true);
    writer.AddFile("ordinary.bin", ordinary, true);
    EXPECT_TRUE(writer.Finalize(path));

    Spark::SparkPakReader reader;
    EXPECT_TRUE(reader.Open(path));
    EXPECT_TRUE(reader.ReadFile("blank_lightmap.bin") == zeros);
    EXPECT_TRUE(reader.ReadFile("ordinary.bin") == ordinary);
    Cleanup();
}

TEST(SparkPak_ProductionAcceptsOrdinaryCompressionRatios)
{
    // The budget must not reject archives the cooker actually produces:
    // 64 KB of highly repetitive bytes compresses well and must round-trip.
    const auto path = TempPath("normal_ratio.spk");
    std::vector<uint8_t> payload(64 * 1024);
    for (size_t i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<uint8_t>(i % 251);

    Spark::SparkPakWriter writer;
    writer.AddFile("normal.bin", payload, true);
    EXPECT_TRUE(writer.Finalize(path));

    Spark::SparkPakReader reader;
    EXPECT_TRUE(reader.Open(path));
    EXPECT_TRUE(reader.ReadFile("normal.bin") == payload);
    Cleanup();
}

TEST(SparkPak_ProductionConcurrentReadsKeepEntryBoundaries)
{
    const auto path = TempPath("parallel_reads.spk");
    std::vector<uint8_t> first(64 * 1024, 0x3c);
    std::vector<uint8_t> second(64 * 1024, 0xa7);

    Spark::SparkPakWriter writer;
    writer.AddFile("first.bin", first, false);
    writer.AddFile("second.bin", second, false);
    EXPECT_TRUE(writer.Finalize(path));

    Spark::SparkPakReader reader;
    EXPECT_TRUE(reader.Open(path));

    std::atomic<bool> readsCorrect{true};
    std::vector<std::thread> threads;
    for (int threadIndex = 0; threadIndex < 6; ++threadIndex)
    {
        threads.emplace_back(
            [&, threadIndex]
            {
                for (int iteration = 0; iteration < 100; ++iteration)
                {
                    const bool chooseFirst = ((threadIndex + iteration) & 1) == 0;
                    const auto bytes = reader.ReadFile(chooseFirst ? "first.bin" : "second.bin");
                    if (bytes != (chooseFirst ? first : second))
                    {
                        readsCorrect.store(false, std::memory_order_relaxed);
                        return;
                    }
                }
            });
    }
    for (auto& thread : threads)
        thread.join();

    EXPECT_TRUE(readsCorrect.load(std::memory_order_relaxed));
    reader.Close();
    Cleanup();
}

TEST(SparkPak_ProductionAddDirectoryRejectsSymlinkOutsideRoot)
{
    namespace fs = std::filesystem;
    const fs::path base = fs::path(TempPath("symlink_case"));
    const fs::path root = base / "root";
    const fs::path outside = base / "outside.bin";
    std::error_code ec;
    // Start clean: unlink any link an aborted run left, then drop the rest.
    SparkTestLinks::RemoveDirectoryLink(root / "linked-dir");
    fs::remove_all(base, ec);
    fs::create_directories(root, ec);
    {
        std::ofstream(root / "inside.bin", std::ios::binary) << "inside";
        std::ofstream(outside, std::ios::binary) << "outside-secret";
    }

    // A directory link out of the root runs on every host: an NTFS junction on
    // Windows (no privilege needed), a symlink elsewhere.
    fs::create_directories(base / "outside-dir", ec);
    std::ofstream(base / "outside-dir" / "secret.bin", std::ios::binary) << "outside-secret";
    ASSERT_TRUE(SparkTestLinks::MakeDirectoryLink(base / "outside-dir", root / "linked-dir"));

    // A file symlink as well where the host allows one; Windows needs Developer
    // Mode or elevated symlink rights for it, so its absence is not a failure.
    fs::create_symlink(outside, root / "outside-link.bin", ec);

    Spark::SparkPakWriter writer;
    writer.AddDirectory(root);
    EXPECT_EQ(writer.GetFileCount(), 1u);

    const auto pakPath = (base / "symlink_escape.spk").string();
    EXPECT_TRUE(writer.Finalize(pakPath));
    Spark::SparkPakReader reader;
    EXPECT_TRUE(reader.Open(pakPath));
    EXPECT_TRUE(reader.Exists("inside.bin"));
    EXPECT_FALSE(reader.Exists("outside-link.bin"));
    EXPECT_FALSE(reader.Exists("linked-dir/secret.bin"));
    reader.Close();
    EXPECT_TRUE(SparkTestLinks::RemoveDirectoryLink(root / "linked-dir"));
    Cleanup();
}

#ifdef SPARK_MINIZ_AVAILABLE
TEST(SparkPak_ProductionRejectsTocHeaderThatOverstatesDeflateOutput)
{
    // Regression for the SEC-120 SparkPak fuzz target: ReadTOC allocated the
    // header's tocRawSize (up to 256 MB) before inflating a TOC stream of any
    // size, and accepted a stream that ended short of that size. A tiny archive
    // could therefore demand a 256 MB zero-filled buffer at mount time and still
    // open. Both forms are now refused before the entry parser runs.
    const auto path = TempPath("toc_overstated.spk");
    Spark::SparkPakWriter writer;
    writer.AddFile("asset.bin", std::vector<uint8_t>(16, 0x42), false);
    EXPECT_TRUE(writer.Finalize(path));

    Spark::PakHeader header;
    {
        std::ifstream in(path, std::ios::binary);
        in.read(reinterpret_cast<char*>(&header), sizeof(header));
        EXPECT_TRUE(in.good());
    }
    EXPECT_TRUE(header.tocSize != header.tocRawSize); // the writer deflates the TOC

    const auto rewriteRawSize = [&](uint32_t rawSize)
    {
        Spark::PakHeader patched = header;
        patched.tocRawSize = rawSize;
        std::fstream out(path, std::ios::binary | std::ios::in | std::ios::out);
        out.seekp(0);
        out.write(reinterpret_cast<const char*>(&patched), sizeof(patched));
    };

    {
        Spark::SparkPakReader reader;
        EXPECT_TRUE(reader.Open(path));
        EXPECT_TRUE(reader.ReadFile("asset.bin") == std::vector<uint8_t>(16, 0x42));
    }

    // The stream inflates to fewer bytes than declared (still a plausible ratio).
    rewriteRawSize(header.tocRawSize + 64u);
    {
        Spark::SparkPakReader reader;
        EXPECT_FALSE(reader.Open(path));
    }

    // A declared expansion past deflate's ~1032:1 ceiling cannot be genuine.
    rewriteRawSize(header.tocSize * 1032u + 1u);
    {
        Spark::SparkPakReader reader;
        EXPECT_FALSE(reader.Open(path));
    }

    Cleanup();
}
#endif

// ============================================================================
// SEC-assets: TOC hash binding, duplicates and count/size agreement
// ============================================================================

namespace
{
    struct RawTocEntry
    {
        std::string path;
        uint64_t hash = 0;
    };

    /// Write a stored-TOC archive whose entries all point at one shared payload
    /// byte. extraTocBytes appends that many zero bytes after the last entry.
    void WriteRawTocArchive(const std::string& archivePath, const std::vector<RawTocEntry>& entries,
                            uint32_t declaredCount, size_t extraTocBytes = 0)
    {
        std::vector<uint8_t> toc;
        const uint64_t dataOffset = sizeof(Spark::PakHeader);
        const uint32_t size = 1;
        const uint8_t compression = static_cast<uint8_t>(Spark::PakCompression::Stored);
        for (const auto& entry : entries)
        {
            const uint16_t pathLen = static_cast<uint16_t>(entry.path.size());
            const size_t offset = toc.size();
            toc.resize(offset + 27 + pathLen);
            uint8_t* cursor = toc.data() + offset;
            std::memcpy(cursor, &entry.hash, 8);
            cursor += 8;
            std::memcpy(cursor, &dataOffset, 8);
            cursor += 8;
            std::memcpy(cursor, &size, 4);
            cursor += 4;
            std::memcpy(cursor, &size, 4);
            cursor += 4;
            *cursor = compression;
            cursor += 1;
            std::memcpy(cursor, &pathLen, 2);
            cursor += 2;
            std::memcpy(cursor, entry.path.data(), pathLen);
        }
        toc.resize(toc.size() + extraTocBytes, 0);

        Spark::PakHeader header;
        header.fileCount = declaredCount;
        header.tocOffset = sizeof(Spark::PakHeader) + 1;
        header.tocSize = static_cast<uint32_t>(toc.size());
        header.tocRawSize = header.tocSize;

        std::ofstream out(archivePath, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        const uint8_t payload = 0x5a;
        out.write(reinterpret_cast<const char*>(&payload), 1);
        out.write(reinterpret_cast<const char*>(toc.data()), static_cast<std::streamsize>(toc.size()));
    }
} // namespace

TEST(AssetSec_SparkPakRejectsHashNotBoundToStoredPath)
{
    // The stored hash of "b/benign.txt" is really the hash of "a/trusted.dds":
    // before the fix the archive opened, listed only the benign name and served
    // its payload for ReadFile("a/trusted.dds").
    const auto path = TempPath("hash_unbound.spk");

    WriteRawTocArchive(path, {{"b/benign.txt", Spark::PakFNV1a("b/benign.txt")}}, 1);
    {
        Spark::SparkPakReader control;
        EXPECT_TRUE(control.Open(path));
        EXPECT_TRUE(control.Exists("b/benign.txt"));
    }

    WriteRawTocArchive(path, {{"b/benign.txt", Spark::PakFNV1a("a/trusted.dds")}}, 1);
    Spark::SparkPakReader reader;
    EXPECT_FALSE(reader.Open(path));
    EXPECT_FALSE(reader.Exists("a/trusted.dds"));
    Cleanup();
}

TEST(AssetSec_SparkPakRejectsDuplicateTocEntries)
{
    // Two entries for one path: the second was silently dropped by emplace.
    const auto path = TempPath("duplicate_entry.spk");
    const uint64_t hash = Spark::PakFNV1a("dup.bin");
    WriteRawTocArchive(path, {{"dup.bin", hash}, {"dup.bin", hash}}, 2);

    Spark::SparkPakReader reader;
    EXPECT_FALSE(reader.Open(path));
    Cleanup();
}

TEST(AssetSec_SparkPakRejectsTocBytesBeyondDeclaredCount)
{
    // fileCount = 1 but the TOC holds two entries: the second was ignored and
    // the archive mounted with a count that disagreed with its own TOC.
    const auto path = TempPath("toc_trailing.spk");
    WriteRawTocArchive(path, {{"one.bin", Spark::PakFNV1a("one.bin")}, {"two.bin", Spark::PakFNV1a("two.bin")}}, 1);
    {
        Spark::SparkPakReader reader;
        EXPECT_FALSE(reader.Open(path));
    }

    // A count the TOC cannot hold is refused before any per-entry work.
    WriteRawTocArchive(path, {{"one.bin", Spark::PakFNV1a("one.bin")}}, 1000);
    {
        Spark::SparkPakReader reader;
        EXPECT_FALSE(reader.Open(path));
    }
    Cleanup();
}

TEST(AssetSec_SparkPakWriterRefusesDuplicatePaths)
{
    // The writer must not produce an archive the reader refuses to mount.
    const auto path = TempPath("writer_duplicate.spk");
    Spark::SparkPakWriter writer;
    writer.AddFile("same.bin", std::vector<uint8_t>(4, 0x11), false);
    writer.AddFile("same.bin", std::vector<uint8_t>(4, 0x22), false);
    EXPECT_FALSE(writer.Finalize(path));
    EXPECT_FALSE(std::filesystem::exists(path));

    Spark::SparkPakWriter distinct;
    distinct.AddFile("a.bin", std::vector<uint8_t>(4, 0x11), false);
    distinct.AddFile("b.bin", std::vector<uint8_t>(4, 0x22), false);
    EXPECT_TRUE(distinct.Finalize(path));
    Spark::SparkPakReader reader;
    EXPECT_TRUE(reader.Open(path));
    EXPECT_TRUE(reader.ReadFile("b.bin") == std::vector<uint8_t>(4, 0x22));
    reader.Close();
    Cleanup();
}
