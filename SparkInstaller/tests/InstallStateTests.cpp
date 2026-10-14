#include "InstallState.h"
#include "PathUtf8.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{
    std::filesystem::path MakeTestRoot()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        return std::filesystem::temp_directory_path() / ("SparkInstallerInstallStateTests_" + std::to_string(stamp));
    }

    int Check(bool condition, const std::string& message)
    {
        if (condition)
            return 0;
        std::cerr << "FAIL: " << message << '\n';
        return 1;
    }

    SparkInstaller::InstallState StateWithCommit(const std::string& commit)
    {
        SparkInstaller::InstallState state;
        state.ref = "Working";
        state.commit = commit;
        state.generator = "Ninja";
        state.buildType = "Release";
        state.installerVersion = "1.0.0";
        state.options["BUILD_TESTS"] = true;
        return state;
    }

    // A destination string is UTF-8 (TUI, GUI and command line all hand the
    // installer UTF-8). On Windows a narrow fs::path would read it in the ANSI
    // code page and write the markers into a mojibake sibling directory.
    std::filesystem::path NonAsciiLeaf()
    {
#ifdef _WIN32
        return std::filesystem::path(L"Zo\u00EB-install");
#else
        return std::filesystem::path("Zo\xC3\xAB-install");
#endif
    }

    int RunUtf8DestinationTest()
    {
        const auto root = MakeTestRoot();
        const auto wideDestination = root / NonAsciiLeaf();
        const std::string utf8Destination = SparkInstaller::PathToUtf8(root) + "/Zo\xC3\xAB-install";
        std::error_code error;
        std::filesystem::create_directories(wideDestination, error);
        int failures = Check(!error, "could not create the non-ASCII install-state test root");
        if (failures != 0)
            return failures;

        const auto state = StateWithCommit("utf8-commit");
        failures += Check(state.Save(utf8Destination), "install state save to a UTF-8 destination failed");
        failures += Check(std::filesystem::is_regular_file(wideDestination / SparkInstaller::InstallState::FileName()),
                          "install state for a UTF-8 destination was not written inside that destination");
        SparkInstaller::InstallState loaded;
        failures += Check(SparkInstaller::InstallState::Load(utf8Destination, loaded) &&
                              loaded.commit == "utf8-commit" && loaded.destination == utf8Destination,
                          "install state for a UTF-8 destination did not round-trip");

        failures += Check(SparkInstaller::InstallState::WritePendingMarker(utf8Destination, "Working", "utf8-commit"),
                          "pending marker write to a UTF-8 destination failed");
        failures +=
            Check(std::filesystem::is_regular_file(wideDestination / SparkInstaller::InstallState::PendingFileName()),
                  "pending marker for a UTF-8 destination was not written inside that destination");
        std::string ref;
        std::string commit;
        failures += Check(SparkInstaller::InstallState::ReadPendingMarker(utf8Destination, ref, commit) &&
                              ref == "Working" && commit == "utf8-commit",
                          "pending marker for a UTF-8 destination did not round-trip");

        std::filesystem::remove_all(root, error);
        return failures;
    }

    // The conversion helpers themselves: UTF-8 maps to the intended path and
    // back, and ill-formed UTF-8 is recognised instead of silently remapped.
    int RunPathUtf8HelperTest()
    {
        const std::string utf8 = "Zo\xC3\xAB-install";
        int failures = Check(SparkInstaller::PathFromUtf8(utf8) == NonAsciiLeaf(),
                             "PathFromUtf8 did not map UTF-8 to the intended path");
        failures += Check(SparkInstaller::PathToUtf8(NonAsciiLeaf()) == utf8, "PathToUtf8 did not produce UTF-8");
        failures += Check(SparkInstaller::IsValidUtf8(utf8) && SparkInstaller::IsValidUtf8(""),
                          "IsValidUtf8 rejected well-formed UTF-8");
        failures += Check(!SparkInstaller::IsValidUtf8("Zo\xEB") && !SparkInstaller::IsValidUtf8("\xC0\xAF") &&
                              !SparkInstaller::IsValidUtf8("\xED\xA0\x80") && !SparkInstaller::IsValidUtf8("\xC3"),
                          "IsValidUtf8 accepted ill-formed UTF-8");
        std::string narrow = "unchanged";
        failures += Check(SparkInstaller::ProcessArgFromUtf8("ascii", narrow) && narrow == "ascii",
                          "ProcessArgFromUtf8 changed an ASCII path");
#ifdef _WIN32
        narrow = "unchanged";
        failures += Check(!SparkInstaller::ProcessArgFromUtf8("Zo\xEB", narrow) && narrow == "unchanged",
                          "ProcessArgFromUtf8 accepted ill-formed UTF-8");
#endif
        return failures;
    }

    // A non-ASCII character with its UTF-8 bytes, for picking one the active
    // code page can (or cannot) represent. The probe calls the Win32 API
    // directly so it does not depend on the helper under test.
    struct NonAsciiCandidate
    {
        wchar_t wide;
        const char* utf8;
    };

    [[maybe_unused]] constexpr NonAsciiCandidate kNonAsciiCandidates[] = {
        {L'\u00EB', "\xC3\xAB"},     // Latin small e with diaeresis (cp1252 and most Western pages)
        {L'\u0416', "\xD0\x96"},     // Cyrillic Zhe (cp1251, cp932, cp936)
        {L'\u65E5', "\xE6\x97\xA5"}, // CJK "sun" (cp932, cp936, cp949, cp950)
        {L'\u05D0', "\xD7\x90"},     // Hebrew Alef (cp1255)
        {L'\u0E01', "\xE0\xB8\x81"}, // Thai Ko Kai (cp874)
    };

#ifdef _WIN32
    [[maybe_unused]] bool ActiveCodePageRepresents(wchar_t character)
    {
        if (::GetACP() == CP_UTF8)
        {
            return true;
        }
        char buffer[8] = {};
        BOOL usedDefault = FALSE;
        const int size = ::WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, &character, 1, buffer, sizeof(buffer),
                                               nullptr, &usedDefault);
        return size > 0 && !usedDefault;
    }
#endif

    // Returns nullptr when every candidate is (or none is) representable.
    [[maybe_unused]] const NonAsciiCandidate* FindCandidate(bool representable)
    {
        for (const NonAsciiCandidate& candidate : kNonAsciiCandidates)
        {
#ifdef _WIN32
            if (ActiveCodePageRepresents(candidate.wide) == representable)
            {
                return &candidate;
            }
#else
            // POSIX paths are bytes: everything is representable.
            if (representable)
            {
                return &candidate;
            }
#endif
        }
        return nullptr;
    }

    [[maybe_unused]] std::filesystem::path CandidateLeaf(const NonAsciiCandidate& candidate)
    {
#ifdef _WIN32
        return std::filesystem::path(std::wstring(L"Zo") + candidate.wide + L"-install");
#else
        return std::filesystem::path(std::string("Zo") + candidate.utf8 + "-install");
#endif
    }

    // "Refused, not mangled": a well-formed character the active code page
    // lacks must fail the conversion and leave the output untouched, because a
    // best-fit or '?' substitute would name a different directory.
    int RunUnrepresentableProcessArgTest()
    {
#ifdef _WIN32
        const NonAsciiCandidate* missing = FindCandidate(false);
        if (missing == nullptr)
        {
            std::cout << "SKIP: active code page " << ::GetACP()
                      << " represents every candidate; no unrepresentable-character case to test\n";
            return 0;
        }
        const std::string utf8 = std::string("C:/Zo") + missing->utf8 + "-install";
        std::string narrow = "unchanged";
        int failures = Check(SparkInstaller::IsValidUtf8(utf8), "unrepresentable candidate is not valid UTF-8");
        failures += Check(!SparkInstaller::ProcessArgFromUtf8(utf8, narrow) && narrow == "unchanged",
                          "ProcessArgFromUtf8 mangled a character the active code page cannot represent");
        return failures;
#else
        return 0;
#endif
    }

    int RunAtomicReplacementTest()
    {
        const auto root = MakeTestRoot();
        std::error_code error;
        std::filesystem::create_directories(root, error);
        int failures = Check(!error, "could not create install-state test root");
        if (failures != 0)
            return failures;

        const auto marker = root / SparkInstaller::InstallState::FileName();
        const auto temporaryMarker = marker.string() + ".tmp";
        const auto first = StateWithCommit("old-commit");
        failures += Check(first.Save(root.string()), "initial install state save failed");

        {
            std::ofstream stale(temporaryMarker, std::ios::binary | std::ios::trunc);
            stale << "partial state from an interrupted save";
        }

        const auto replacement = StateWithCommit("new-commit");
        failures += Check(replacement.Save(root.string()), "replacement install state save failed");

        SparkInstaller::InstallState loaded;
        failures += Check(SparkInstaller::InstallState::Load(root.string(), loaded),
                          "replacement install state could not be loaded");
        failures += Check(loaded.commit == "new-commit", "replacement did not become the active state");
        failures +=
            Check(!std::filesystem::exists(temporaryMarker), "interrupted-save temporary marker was left behind");

        std::filesystem::remove_all(root, error);
        return failures;
    }

    int RunInvalidStateCannotReplaceValidMarkerTest()
    {
        const auto root = MakeTestRoot();
        std::error_code error;
        std::filesystem::create_directories(root, error);
        int failures = Check(!error, "could not create invalid-state test root");
        if (failures != 0)
            return failures;

        const auto validState = StateWithCommit("old-commit");
        failures += Check(validState.Save(root.string()), "could not create the valid install marker");

        const auto invalidState = StateWithCommit("");
        failures += Check(!invalidState.Save(root.string()), "invalid install state was accepted for persistence");

        SparkInstaller::InstallState loaded;
        failures += Check(SparkInstaller::InstallState::Load(root.string(), loaded),
                          "rejecting invalid install state discarded the valid marker");
        failures += Check(loaded.commit == "old-commit", "invalid install state replaced the previously valid marker");

        const auto temporaryMarker = root / (SparkInstaller::InstallState::FileName() + ".tmp");
        failures += Check(!std::filesystem::exists(temporaryMarker),
                          "rejecting invalid install state left a temporary marker behind");

        std::filesystem::remove_all(root, error);
        return failures;
    }

    int RunNonRegularMarkerCannotBeReplacedTest()
    {
        const auto root = MakeTestRoot();
        std::error_code error;
        std::filesystem::create_directories(root, error);
        int failures = Check(!error, "could not create non-regular-marker test root");
        if (failures != 0)
        {
            return failures;
        }

        const auto marker = root / SparkInstaller::InstallState::FileName();
        const auto replacement = StateWithCommit("replacement-commit");
        std::filesystem::create_directory(marker, error);
        failures += Check(!error, "could not create marker directory fixture");
        failures += Check(!replacement.Save(root.string()), "marker directory was replaced");
        failures += Check(std::filesystem::is_directory(marker), "marker directory fixture was removed");

        std::filesystem::remove_all(root, error);
        std::filesystem::create_directories(root, error);
        const auto danglingTarget = root / "missing-target.json";
        std::filesystem::create_symlink(danglingTarget, marker, error);
        if (error)
        {
            // Unprivileged Windows hosts without Developer Mode cannot create
            // symlinks; the directory case above still covers non-regular markers.
            std::cout << "SKIP: dangling marker link case (" << error.message() << ")\n";
            std::filesystem::remove_all(root, error);
            return failures;
        }
        failures += Check(!replacement.Save(root.string()), "dangling marker link was replaced");
        failures += Check(std::filesystem::is_symlink(marker), "dangling marker link was removed");

        std::filesystem::remove_all(root, error);
        return failures;
    }

    int RunMalformedMarkerFailClosedTest()
    {
        const auto root = MakeTestRoot();
        std::error_code error;
        std::filesystem::create_directories(root, error);
        int failures = Check(!error, "could not create malformed-marker test root");
        if (failures != 0)
            return failures;

        std::ofstream marker(root / SparkInstaller::InstallState::FileName(), std::ios::binary | std::ios::trunc);
        marker << "{\n  \"schema\": 1,\n  \"commit\": \"partial";
        marker.close();

        SparkInstaller::InstallState loaded;
        failures +=
            Check(!SparkInstaller::InstallState::Load(root.string(), loaded), "malformed install state was accepted");
        failures += Check(!SparkInstaller::InstallState::Exists(root.string()),
                          "malformed install state was treated as an existing install");

        std::filesystem::remove_all(root, error);
        return failures;
    }

    int RunOversizedMarkerRejectedBeforeParsingTest()
    {
        const auto root = MakeTestRoot();
        std::error_code error;
        std::filesystem::create_directories(root, error);
        int failures = Check(!error, "could not create oversized-marker test root");
        if (failures != 0)
            return failures;

        std::ofstream marker(root / SparkInstaller::InstallState::FileName(), std::ios::binary | std::ios::trunc);
        marker << '{' << "\"schema\":1,\"ref\":\"stable-v1\",\"commit\":\"0123456789abcdef\","
               << "\"destination\":\"C:/SparkEngine\",\"generator\":\"Ninja\","
               << "\"build_type\":\"Release\",\"built_at\":\"2026-09-13T00:00:00Z\","
               << "\"installer_version\":\"1.0.0\",\"payload\":\"" << std::string(64 * 1024, 'x') << "\"}";
        marker.close();

        SparkInstaller::InstallState loaded;
        failures +=
            Check(!SparkInstaller::InstallState::Load(root.string(), loaded), "oversized install state was accepted");
        failures += Check(!SparkInstaller::InstallState::Exists(root.string()),
                          "oversized install state was treated as an existing install");

        std::filesystem::remove_all(root, error);
        return failures;
    }

    bool WriteMarkerText(const std::filesystem::path& path, const std::string& text)
    {
        std::ofstream marker(path, std::ios::binary | std::ios::trunc);
        marker << text;
        marker.close();
        return static_cast<bool>(marker);
    }

    // Save escapes '"' and '\\'; Load must unescape them. A Windows destination
    // or a ref holding a quote used to read back truncated or doubled.
    int RunEscapedValueRoundTripTest()
    {
        const auto root = MakeTestRoot();
        std::error_code error;
        std::filesystem::create_directories(root, error);
        int failures = Check(!error, "could not create escaped-value test root");
        if (failures != 0)
        {
            return failures;
        }

        auto state = StateWithCommit("0123456789abcdef");
        state.ref = "feat\"x";
        state.generator = "Visual Studio 17 \\ 2022";
        state.options["TAB\tKEY"] = false;
        failures += Check(state.Save(root.string()), "escaped install state save failed");

        SparkInstaller::InstallState loaded;
        failures += Check(SparkInstaller::InstallState::Load(root.string(), loaded),
                          "escaped install state could not be loaded");
        failures += Check(loaded.ref == state.ref, "ref with a quote did not round-trip: " + loaded.ref);
        failures += Check(loaded.generator == state.generator,
                          "generator with a backslash did not round-trip: " + loaded.generator);
        failures += Check(loaded.destination == root.string(), "destination did not round-trip: " + loaded.destination);
        failures += Check(loaded.options == state.options, "options did not round-trip");

        std::filesystem::remove_all(root, error);
        return failures;
    }

    std::string StateDocument(const std::string& schema, const std::string& extraMembers)
    {
        return "{\n  \"schema\": " + schema +
               ",\n  \"ref\": \"Working\",\n  \"commit\": \"0123456789abcdef\",\n"
               "  \"destination\": \"/opt/spark\",\n  \"generator\": \"Ninja\",\n  \"build_type\": \"Release\",\n"
               "  \"built_at\": \"2026-09-29T00:00:00Z\",\n  \"installer_version\": \"1.0.0\"," +
               extraMembers + "\n  \"options\": {\n    \"BUILD_TESTS\": true\n  }\n}\n";
    }

    int ExpectDocument(const std::string& name, const std::string& document, bool accepted)
    {
        const auto root = MakeTestRoot();
        std::error_code error;
        std::filesystem::create_directories(root, error);
        int failures = Check(!error, name + ": could not create test root");
        if (failures != 0)
        {
            return failures;
        }
        failures += Check(WriteMarkerText(root / SparkInstaller::InstallState::FileName(), document),
                          name + ": could not write the marker");
        SparkInstaller::InstallState loaded;
        loaded.commit = "sentinel";
        const bool result = SparkInstaller::InstallState::Load(root.string(), loaded);
        failures += Check(result == accepted, name + (accepted ? ": was rejected" : ": was accepted"));
        if (!accepted)
        {
            failures += Check(loaded.commit == "sentinel", name + ": a rejected load changed the output state");
        }
        std::filesystem::remove_all(root, error);
        return failures;
    }

    // std::atoi on an out-of-range value is undefined; LP64 glibc truncates
    // 4294967297 to 1, so the old reader accepted it as schema 1.
    int RunSchemaOverflowRejectedTest()
    {
        int failures = ExpectDocument("well-formed state", StateDocument("1", ""), true);
        failures += ExpectDocument("schema 4294967297", StateDocument("4294967297", ""), false);
        failures += ExpectDocument("schema 99999999999999999999", StateDocument("99999999999999999999", ""), false);
        failures += ExpectDocument("schema 2", StateDocument("2", ""), false);
        failures += ExpectDocument("schema 1.5", StateDocument("1.5", ""), false);
        // JSON has no leading zeros; from_chars alone reads "01" as 1 (SparkFuzzInstallState finding).
        failures += ExpectDocument("schema 01", StateDocument("01", ""), false);
        failures += ExpectDocument("schema 001", StateDocument("001", ""), false);
        failures += ExpectDocument("schema -01", StateDocument("-01", ""), false);
        return failures;
    }

    int RunStateSizeAndEncodingBoundTest()
    {
        std::string compact = "{\"schema\":1,\"ref\":\"@\",\"commit\":\"0123456789abcdef\",\"destination\":\"d\","
                              "\"generator\":\"Ninja\",\"build_type\":\"Release\",\"built_at\":\"t\","
                              "\"installer_version\":\"v\",\"options\":{}}";
        compact.replace(compact.find('@'), 1, std::string(64 * 1024 - compact.size() + 1, 'x'));
        int failures = Check(compact.size() == 64 * 1024, "compact state fixture did not reach the read bound");
        failures += ExpectDocument("state expands past write bound", compact, false);

        const auto root = MakeTestRoot();
        std::error_code error;
        std::filesystem::create_directories(root, error);
        failures += Check(!error, "could not create state-size test root");
        if (error)
        {
            return failures;
        }
        auto state = StateWithCommit("old-commit");
        failures += Check(state.Save(root.string()), "could not create state-size baseline marker");
        state.ref = std::string(64 * 1024, 'x');
        failures += Check(!state.Save(root.string()), "oversized state was written");
        state.ref = "bad\x01ref";
        failures += Check(!state.Save(root.string()), "state with a raw control byte was written");
        state.ref = "Working";
        state.options[std::string("bad\0option", 10)] = true;
        failures += Check(!state.Save(root.string()), "option key with a NUL was written");
        SparkInstaller::InstallState loaded;
        failures += Check(SparkInstaller::InstallState::Load(root.string(), loaded),
                          "rejected state replaced the previous valid marker");
        failures += Check(loaded.commit == "old-commit", "rejected state changed the previous valid marker");
        std::filesystem::remove_all(root, error);
        return failures;
    }

    int RunDuplicateKeyRejectedTest()
    {
        int failures =
            ExpectDocument("duplicate commit", StateDocument("1", "\n  \"commit\": \"fedcba9876543210\","), false);
        failures += ExpectDocument("unknown key", StateDocument("1", "\n  \"payload\": \"x\","), false);
        failures +=
            ExpectDocument("key inside a value",
                           "{\"schema\": 1, \"ref\": \"x \\\"commit\\\": \\\"y\\\"\", \"destination\": \"d\"}", false);
        std::string undefinedEscape = StateDocument("1", "");
        undefinedEscape.replace(undefinedEscape.find("Working"), 7, "a\\u0041");
        failures += ExpectDocument("undefined escape", undefinedEscape, false);
        std::string rawControl = StateDocument("1", "");
        rawControl.replace(rawControl.find("Working"), 7, "a\x01z");
        failures += ExpectDocument("raw control byte", rawControl, false);
        return failures;
    }

    // The pending marker moved under InstallState so its bounded, strict reader is
    // tested directly: a write reads back unchanged, and oversize, duplicate,
    // unknown or incomplete markers fail closed without touching the outputs.
    int RunPendingMarkerRoundTripAndBoundTest()
    {
        const auto root = MakeTestRoot();
        std::error_code error;
        std::filesystem::create_directories(root, error);
        int failures = Check(!error, "could not create pending-marker test root");
        if (failures != 0)
        {
            return failures;
        }

        using SparkInstaller::InstallState;
        failures += Check(InstallState::WritePendingMarker(root.string(), "stable-v1", "0123456789abcdef"),
                          "pending marker write failed");
        std::string ref;
        std::string commit;
        failures += Check(InstallState::ReadPendingMarker(root.string(), ref, commit), "pending marker read failed");
        failures += Check(ref == "stable-v1" && commit == "0123456789abcdef", "pending marker did not round-trip");
        failures += Check(!InstallState::WritePendingMarker(root.string(), "bad\nref", "0123"),
                          "a ref holding a newline was written");
        const std::string maxRef(4096 - 13 - 1, 'r');
        failures += Check(InstallState::WritePendingMarker(root.string(), maxRef, "c"),
                          "maximum-size pending marker was rejected");
        failures += Check(InstallState::ReadPendingMarker(root.string(), ref, commit) && ref == maxRef && commit == "c",
                          "maximum-size pending marker did not round-trip");
        failures += Check(!InstallState::WritePendingMarker(root.string(), maxRef + "r", "c"),
                          "oversized pending marker was written");

        const auto marker = root / InstallState::PendingFileName();
        const auto expectRejected = [&](const std::string& name, const std::string& text)
        {
            int caseFailures = Check(WriteMarkerText(marker, text), name + ": could not write the marker");
            std::string caseRef = "sentinel-ref";
            std::string caseCommit = "sentinel-commit";
            caseFailures +=
                Check(!InstallState::ReadPendingMarker(root.string(), caseRef, caseCommit), name + ": was accepted");
            caseFailures += Check(caseRef == "sentinel-ref" && caseCommit == "sentinel-commit",
                                  name + ": a rejected read changed the outputs");
            return caseFailures;
        };
        failures += expectRejected("oversized line",
                                   "ref=stable-v1\ncommit=0123456789abcdef\n" + std::string(4097, 'x') + "\n");
        failures += expectRejected("duplicate commit", "ref=stable-v1\ncommit=aaaa\ncommit=bbbb\n");
        failures += expectRejected("unknown line", "ref=stable-v1\ncommit=aaaa\nextra=1\n");
        failures += expectRejected("missing commit", "ref=stable-v1\n");
        failures += expectRejected("embedded NUL", std::string("ref=stable-v1\ncommit=aa") + '\0' + "aa\n");

        std::filesystem::remove_all(root, error);
        return failures;
    }
} // namespace

int main()
{
    const int results[] = {
        RunAtomicReplacementTest(),
        RunInvalidStateCannotReplaceValidMarkerTest(),
        RunNonRegularMarkerCannotBeReplacedTest(),
        RunMalformedMarkerFailClosedTest(),
        RunOversizedMarkerRejectedBeforeParsingTest(),
        RunEscapedValueRoundTripTest(),
        RunSchemaOverflowRejectedTest(),
        RunStateSizeAndEncodingBoundTest(),
        RunDuplicateKeyRejectedTest(),
        RunPendingMarkerRoundTripAndBoundTest(),
        RunUtf8DestinationTest(),
        RunPathUtf8HelperTest(),
        RunUnrepresentableProcessArgTest(),
    };
    for (const int failures : results)
    {
        if (failures != 0)
        {
            return 1;
        }
    }
    std::cout << "SparkInstaller install-state recovery tests passed\n";
    return 0;
}
