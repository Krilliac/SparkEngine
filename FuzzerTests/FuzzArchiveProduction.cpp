/**
 * @file FuzzArchiveProduction.cpp
 * @brief libc++-compiled production adapter for the SparkPak libFuzzer harness.
 *
 * SparkPakReader::Open takes a path, so the fuzz input is placed in an anonymous
 * memfd and the shipped reader opens it through /proc/self/fd, exactly as it
 * opens a .spk found under ./Data. When the archive mounts, every entry is
 * listed and read back through ReadFile (the path ArchiveResourceProvider uses),
 * so the TOC parser, the per-entry decompression budget, miniz inflate and the
 * vendored zstd decoder all see attacker-controlled bytes. The reader is then
 * checked against the guarantees its consumers rely on; a violation aborts so
 * libFuzzer records it as a crash rather than a silent pass:
 *  - every listed virtual path stays inside the mount root under Windows
 *    separator semantics, decided by an independent lexical walk here rather
 *    than by the reader's own IsVirtualPathSafe filter, and contains no control
 *    byte (an embedded NUL makes every c_str() consumer open a different file),
 *  - every listed path also passes the production IsVirtualPathSafe policy,
 *  - the listing is exactly the mounted entry set, in sorted order,
 *  - no TOC can mount more entries than its bytes can encode, even after
 *    inflating at deflate's maximum ratio,
 *  - a read never returns more than the per-entry decompression budget, nor
 *    more than the whole archive could expand to at the per-entry ratio cap.
 *
 * The 256 MB TOC and per-entry budgets lie far above the smoke's -max_len, so
 * their boundary behaviour is pinned by the SparkPak_Production* tests in
 * Tests/TestSparkPak.cpp; this target proves the reader stays inside its
 * invariants for arbitrary bytes below them.
 */

#include "FuzzArchiveProduction.h"

#include "Core/SparkPak.h"
#include "Engine/Modding/VirtualFileSystem.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;

    // Mirrors of the reader's documented on-disk limits (SparkPak.h / SparkPak.cpp).
    constexpr std::size_t kMinTocEntryBytes = 27;
    constexpr std::uint64_t kMaxDeflateExpansion = 1032;
    constexpr std::uint64_t kMaxDecompressedEntryBytes = 256ull * 1024ull * 1024ull;
    constexpr std::uint64_t kMaxCompressionRatio = 100'000ull;

    [[noreturn]] void InfrastructureFailure(const char* operation)
    {
        std::fprintf(stderr, "SparkFuzzArchive infrastructure failure during %s: %s\n", operation,
                     std::strerror(errno));
        std::_Exit(70);
    }

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzArchive: SparkPakReader mounted an archive that violates: %s\n", what);
        std::abort();
    }

    /// Anonymous in-memory file: no disk writes, no name another process can
    /// race, and the descriptor is released when the input is done.
    class MemoryInput
    {
      public:
        MemoryInput(const std::uint8_t* data, std::size_t size)
        {
            m_descriptor = ::memfd_create("spark-pak-fuzz", MFD_CLOEXEC);
            if (m_descriptor < 0)
                InfrastructureFailure("memfd_create");

            std::size_t offset = 0;
            while (offset < size)
            {
                const ssize_t written = ::write(m_descriptor, data + offset, size - offset);
                if (written < 0 && errno == EINTR)
                    continue;
                if (written <= 0)
                    InfrastructureFailure("write");
                offset += static_cast<std::size_t>(written);
            }
            m_path = "/proc/self/fd/" + std::to_string(m_descriptor);
        }

        MemoryInput(const MemoryInput&) = delete;
        MemoryInput& operator=(const MemoryInput&) = delete;

        ~MemoryInput() { ::close(m_descriptor); }

        const std::string& Path() const { return m_path; }

      private:
        int m_descriptor = -1;
        std::string m_path;
    };

    /// Containment decided without std::filesystem, so it holds identically on
    /// every host: components are split on both separators and '..' may never
    /// climb above the root.
    bool StaysInsideRootOnEveryHost(const std::string& path)
    {
        if (path.empty() || path.front() == '/' || path.front() == '\\')
            return false;
        if (path.find(':') != std::string::npos)
            return false;

        std::size_t depth = 0;
        std::size_t componentStart = 0;
        while (componentStart <= path.size())
        {
            std::size_t componentEnd = path.find_first_of("/\\", componentStart);
            if (componentEnd == std::string::npos)
                componentEnd = path.size();

            const std::string_view component(path.data() + componentStart, componentEnd - componentStart);
            if (component == "..")
            {
                if (depth == 0)
                    return false;
                --depth;
            }
            else if (!component.empty() && component != ".")
            {
                ++depth;
            }
            componentStart = componentEnd + 1;
        }
        return true;
    }

    bool ContainsControlByte(const std::string& path)
    {
        for (const char c : path)
        {
            const auto byte = static_cast<unsigned char>(c);
            if (byte < 0x20 || byte == 0x7F)
                return true;
        }
        return false;
    }

    void CheckMountedArchive(const Spark::SparkPakReader& reader, std::size_t inputSize)
    {
        const std::uint64_t maxEntryBytes =
            std::min(kMaxDecompressedEntryBytes, static_cast<std::uint64_t>(inputSize) * kMaxCompressionRatio);
        const std::uint32_t entryCount = reader.GetEntryCount();
        if (static_cast<std::uint64_t>(entryCount) * kMinTocEntryBytes >
            static_cast<std::uint64_t>(inputSize) * kMaxDeflateExpansion)
            InvariantFailure("entry count exceeds what the TOC bytes can encode");

        const std::vector<std::string> listing = reader.ListFiles("");
        if (listing.size() != entryCount)
            InvariantFailure("ListFiles(\"\") differs from the mounted entry count");
        if (!std::is_sorted(listing.begin(), listing.end()))
            InvariantFailure("ListFiles result is not sorted");

        for (const std::string& path : listing)
        {
            if (!StaysInsideRootOnEveryHost(path))
                InvariantFailure("entry path containment under Windows separator semantics");
            if (ContainsControlByte(path))
                InvariantFailure("entry path contains a control byte");
            if (!Spark::IsVirtualPathSafe(path))
                InvariantFailure("entry path containment (IsVirtualPathSafe)");

            // ReadFile resolves by FNV-1a of the path, which may select a
            // different entry when the stored hash is hostile; either way the
            // returned bytes must respect the budgets of whatever entry answered.
            const std::vector<std::uint8_t> bytes = reader.ReadFile(path);
            if (bytes.size() > maxEntryBytes)
                InvariantFailure("entry read exceeds the per-entry decompression budget");
        }
    }
} // namespace

// This C ABI is the only boundary between the libstdc++ libFuzzer executable
// and the libc++-compiled production reader, VFS policy, codecs and logger.
extern "C" int SparkFuzzOpenSparkPak(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes)
        return 0;
    if (data == nullptr && size != 0)
        return 0;

    MemoryInput input(data, size);

    Spark::SparkPakReader reader;
    if (reader.Open(input.Path()))
        CheckMountedArchive(reader, size);
    return 0;
}
