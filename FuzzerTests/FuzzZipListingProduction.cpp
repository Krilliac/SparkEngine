/**
 * @file FuzzZipListingProduction.cpp
 * @brief libc++-compiled production adapter for the SparkBuild ZIP listing libFuzzer harness.
 *
 * SparkBuild downloads MinGit and CMake archives and, before any extractor
 * runs, lists the ZIP in process with ArchiveExtraction::ListZipMembers (the
 * end-of-central-directory record, the ZIP64 locator and record, the central
 * directory and every local-header name) and gates extraction on
 * ValidateMemberNames. ListZipMembers takes a path, so the fuzz input is placed
 * in an anonymous memfd and the shipped reader opens it through /proc/self/fd,
 * exactly as it opens the downloaded file. For each accepted listing the
 * adapter aborts, so libFuzzer records a crash rather than a silent pass, when:
 *  - the listing holds more members than kMaxZipEntries, or more than the input
 *    can encode at 46 central-directory bytes per member,
 *  - a name passes the ZIP member policy but an independent lexical walk here
 *    (both separators, '..', a leading separator, a drive prefix, NUL or ':')
 *    says it could leave the extraction root,
 *  - a name passes the ZIP policy but not the looser TAR policy, which would
 *    mean the two syntaxes disagree about containment,
 *  - ValidateMemberNames disagrees with the per-name policy over the same list.
 *
 * The fuzz targets build only on Linux Clang (FuzzerTests/CMakeLists.txt), so
 * the memfd path is the only staging this adapter needs.
 */

#include "FuzzZipListingProduction.h"

#include "ArchiveExtraction.h"

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
    constexpr std::size_t kMaxInputBytes = 128u * 1024u;

    // Mirrors of the reader's structural limits (ArchiveExtraction.cpp).
    constexpr std::uint64_t kMaxZipEntries = 1u << 20;
    constexpr std::size_t kCentralHeaderSize = 46;

    namespace AE = SparkBuild::ArchiveExtraction;

    [[noreturn]] void InfrastructureFailure(const char* operation)
    {
        std::fprintf(stderr, "SparkFuzzZipListing infrastructure failure during %s: %s\n", operation,
                     std::strerror(errno));
        std::_Exit(70);
    }

    [[noreturn]] void InvariantFailure(const char* what, const std::string& name)
    {
        std::fprintf(stderr,
                     "SparkFuzzZipListing: ListZipMembers accepted an archive that violates: %s (%zu-byte name)\n",
                     what, name.size());
        std::abort();
    }

    /// Anonymous in-memory file: no disk writes, no name another process can
    /// race, and the descriptor is released when the input is done.
    class ArchiveInput
    {
      public:
        ArchiveInput(const std::uint8_t* data, std::size_t size)
        {
            m_descriptor = ::memfd_create("sparkbuild-zip-fuzz", MFD_CLOEXEC);
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

        ArchiveInput(const ArchiveInput&) = delete;
        ArchiveInput& operator=(const ArchiveInput&) = delete;

        ~ArchiveInput() { ::close(m_descriptor); }

        const std::string& Path() const { return m_path; }

      private:
        int m_descriptor = -1;
        std::string m_path;
    };

    /// Containment decided without the production helper: components are split
    /// on both separators, and any spelling a Windows or POSIX extractor could
    /// read as rooted, drive-relative, stream-qualified or climbing is refused.
    bool StaysInsideRootOnEveryHost(std::string_view name)
    {
        if (name.empty() || name.front() == '/' || name.front() == '\\')
            return false;
        if (name.find('\0') != std::string_view::npos || name.find(':') != std::string_view::npos)
            return false;

        std::size_t componentStart = 0;
        while (componentStart <= name.size())
        {
            std::size_t componentEnd = name.find_first_of("/\\", componentStart);
            if (componentEnd == std::string_view::npos)
                componentEnd = name.size();
            if (name.substr(componentStart, componentEnd - componentStart) == "..")
                return false;
            componentStart = componentEnd + 1;
        }
        return true;
    }

    void CheckListing(const std::vector<std::string>& names, std::size_t inputSize)
    {
        if (names.size() > kMaxZipEntries)
            InvariantFailure("more members than kMaxZipEntries", std::string());
        if (names.size() > inputSize / kCentralHeaderSize)
            InvariantFailure("more members than the central-directory bytes can encode", std::string());

        bool everyNameSafe = !names.empty();
        for (const std::string& name : names)
        {
            const bool zipSafe = AE::IsSafeMemberName(name, AE::MemberSyntax::Zip);
            if (zipSafe && !StaysInsideRootOnEveryHost(name))
                InvariantFailure("a name the ZIP policy accepts can leave the extraction root", name);
            if (zipSafe && !AE::IsSafeMemberName(name, AE::MemberSyntax::Tar))
                InvariantFailure("a name the ZIP policy accepts is refused by the looser TAR policy", name);
            everyNameSafe = everyNameSafe && zipSafe;
        }

        std::string error;
        if (AE::ValidateMemberNames(names, AE::MemberSyntax::Zip, error) != everyNameSafe)
            InvariantFailure("ValidateMemberNames disagrees with the per-name policy", std::string());
    }
} // namespace

// This C ABI is the only boundary between the libstdc++ libFuzzer executable
// and the libc++-compiled SparkBuild archive code.
extern "C" int SparkFuzzListZipMembers(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes)
        return 0;
    if (data == nullptr && size != 0)
        return 0;

    ArchiveInput input(data, size);

    std::vector<std::string> names;
    std::string error;
    if (AE::ListZipMembers(input.Path(), names, error))
        CheckListing(names, size);
    return 0;
}
