#include "InstallState.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

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
