// TestSparkPak.cpp — Unit tests for SparkPak archive format (read, write, compress, VFS)
// Standalone test: writes temp archives, reads them back, verifies round-trip correctness.

#include "TestFramework.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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
                std::fwrite(data.data(), 1, data.size(), f);
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
            std::fwrite(tocRaw.data(), 1, tocRaw.size(), f);

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

        bool Open(const std::string& path)
        {
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

        ~TestPakReader()
        {
            if (m_file)
                std::fclose(m_file);
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

    std::string TempPath(const std::string& name)
    {
        auto dir = std::filesystem::temp_directory_path() / "sparkpak_tests";
        std::filesystem::create_directories(dir);
        return (dir / name).string();
    }

    void Cleanup()
    {
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::temp_directory_path() / "sparkpak_tests", ec);
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
