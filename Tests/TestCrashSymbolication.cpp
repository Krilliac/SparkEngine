// TestCrashSymbolication.cpp - OPS-100 build-id symbolication records written
// by the POSIX signal handler (Utils/CrashSymbolication.h): bounded ELF note
// parsing and the fixed-buffer SYMBOLIC FRAMES section consumed by
// tools/ops/symbolicate_crash.py.

#include "TestFramework.h"
#include "Utils/CrashSymbolication.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    using Spark::CrashHandlerDetail::CrashModuleIdentity;

    void AppendWord(std::vector<std::uint8_t>& out, std::uint32_t value)
    {
        std::uint8_t bytes[4];
        std::memcpy(bytes, &value, sizeof(bytes));
        out.insert(out.end(), bytes, bytes + 4);
    }

    void AppendNote(std::vector<std::uint8_t>& out, const char* name, std::uint32_t nameSize, std::uint32_t type,
                    const std::vector<std::uint8_t>& desc)
    {
        AppendWord(out, nameSize);
        AppendWord(out, static_cast<std::uint32_t>(desc.size()));
        AppendWord(out, type);
        out.insert(out.end(), name, name + nameSize);
        while (out.size() % 4 != 0)
            out.push_back(0);
        out.insert(out.end(), desc.begin(), desc.end());
        while (out.size() % 4 != 0)
            out.push_back(0);
    }

    CrashModuleIdentity MakeModule(std::uintptr_t bias, std::uintptr_t begin, std::uintptr_t end, const char* name,
                                   std::uint8_t idByte)
    {
        CrashModuleIdentity module;
        module.loadBias = bias;
        module.beginAddress = begin;
        module.endAddress = end;
        module.buildIdSize = 4;
        module.buildId = {idByte, 0x01, 0x02, 0x03};
        std::strncpy(module.name.data(), name, module.name.size() - 1);
        return module;
    }
} // namespace

TEST(CrashSymbolication_FindsGnuBuildIdAfterOtherNotes)
{
    const std::vector<std::uint8_t> buildId = {0xde, 0xad, 0xbe, 0xef, 0x01, 0x02, 0x03, 0x04, 0x05};
    std::vector<std::uint8_t> notes;
    AppendNote(notes, "GNU", 4, 1, {0, 0, 0, 0, 3, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0}); // NT_GNU_ABI_TAG
    AppendNote(notes, "GNU", 4, 3, buildId);

    std::array<std::uint8_t, Spark::CrashHandlerDetail::kMaxCrashBuildIdBytes> out{};
    ASSERT_EQ(Spark::CrashHandlerDetail::FindGnuBuildIdNote(notes.data(), notes.size(), out), buildId.size());
    EXPECT_TRUE(std::equal(buildId.begin(), buildId.end(), out.begin()));
}

TEST(CrashSymbolication_MalformedNotesYieldNoBuildId)
{
    std::array<std::uint8_t, Spark::CrashHandlerDetail::kMaxCrashBuildIdBytes> out{};
    std::vector<std::uint8_t> notes;
    AppendNote(notes, "GNU", 4, 3, {1, 2, 3, 4, 5, 6, 7, 8});

    // Truncated descriptor: the declared size runs past the region.
    EXPECT_EQ(Spark::CrashHandlerDetail::FindGnuBuildIdNote(notes.data(), notes.size() - 4, out), size_t{0});

    // Oversized name length must not be trusted.
    std::vector<std::uint8_t> hostileName = notes;
    const std::uint32_t hugeName = 0xFFFFFFF0u;
    std::memcpy(hostileName.data(), &hugeName, sizeof(hugeName));
    EXPECT_EQ(Spark::CrashHandlerDetail::FindGnuBuildIdNote(hostileName.data(), hostileName.size(), out), size_t{0});

    // A build-id wider than the fixed capacity is rejected, not truncated.
    std::vector<std::uint8_t> wide;
    AppendNote(wide, "GNU", 4, 3, std::vector<std::uint8_t>(out.size() + 1, 0xaa));
    EXPECT_EQ(Spark::CrashHandlerDetail::FindGnuBuildIdNote(wide.data(), wide.size(), out), size_t{0});

    // A 1-byte build-id is below the store reader's minimum, so the module is
    // reported as having no build-id rather than poisoning the whole section.
    std::vector<std::uint8_t> tiny;
    AppendNote(tiny, "GNU", 4, 3, {0x7f});
    EXPECT_EQ(Spark::CrashHandlerDetail::FindGnuBuildIdNote(tiny.data(), tiny.size(), out), size_t{0});

    // Wrong owner name is not a GNU build-id.
    std::vector<std::uint8_t> other;
    AppendNote(other, "XYZ", 4, 3, {1, 2, 3, 4});
    EXPECT_EQ(Spark::CrashHandlerDetail::FindGnuBuildIdNote(other.data(), other.size(), out), size_t{0});
    EXPECT_EQ(Spark::CrashHandlerDetail::FindGnuBuildIdNote(nullptr, 64, out), size_t{0});
}

TEST(CrashSymbolication_SectionListsReferencedModulesAndRelativeFrames)
{
    const std::array modules = {
        MakeModule(0x1000, 0x1000, 0x5000, "libunused.so", 0xa0),
        MakeModule(0x400000, 0x400000, 0x480000, "SparkEngine", 0xb1),
    };
    const std::uintptr_t frames[] = {0x401234, 0x900000, 0x402000};

    char buffer[1024];
    const size_t length = Spark::CrashHandlerDetail::FormatSymbolicCrashFrames(modules.data(), modules.size(), frames,
                                                                               3, true, buffer, sizeof(buffer));
    const std::string text(buffer, length);
    EXPECT_EQ(text, std::string("*** SYMBOLIC FRAMES ***\n"
                                "MODULE 1 build_id=b1010203 bias=0x400000 name=SparkEngine\n"
                                "SYMFRAME 0 kind=pc module=1 offset=0x1234\n"
                                "SYMFRAME 1 kind=ra module=- address=0x900000\n"
                                "SYMFRAME 2 kind=ra module=1 offset=0x2000\n"
                                "*** END SYMBOLIC FRAMES ***\n"));
}

TEST(CrashSymbolication_ModuleWithoutBuildIdIsReportedUnmapped)
{
    CrashModuleIdentity module = MakeModule(0x10000, 0x10000, 0x20000, "nobuildid.so", 0);
    module.buildIdSize = 0;
    const std::uintptr_t frames[] = {0x10010};

    char buffer[256];
    const size_t length =
        Spark::CrashHandlerDetail::FormatSymbolicCrashFrames(&module, 1, frames, 1, false, buffer, sizeof(buffer));
    const std::string text(buffer, length);
    EXPECT_TRUE(text.find("MODULE") == std::string::npos);
    EXPECT_TRUE(text.find("SYMFRAME 0 kind=ra module=- address=0x10010\n") != std::string::npos);
}

TEST(CrashSymbolication_TruncationKeepsWholeLinesAndDropsEndMarker)
{
    const CrashModuleIdentity module = MakeModule(0x400000, 0x400000, 0x480000, "SparkEngine", 0xb1);
    const std::uintptr_t frames[] = {0x401234, 0x401300, 0x401400};

    char buffer[96];
    const size_t length =
        Spark::CrashHandlerDetail::FormatSymbolicCrashFrames(&module, 1, frames, 3, true, buffer, sizeof(buffer));
    ASSERT_TRUE(length > 0 && length <= sizeof(buffer));
    const std::string text(buffer, length);
    EXPECT_EQ(text.back(), '\n');
    // A truncated section has no end marker, so the symbolizer refuses it.
    EXPECT_TRUE(text.find("*** END SYMBOLIC FRAMES ***") == std::string::npos);
    EXPECT_EQ(Spark::CrashHandlerDetail::FormatSymbolicCrashFrames(&module, 1, frames, 3, true, buffer, 8), size_t{0});
}
