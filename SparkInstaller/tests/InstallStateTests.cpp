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
} // namespace

int main()
{
    const int atomicReplacement = RunAtomicReplacementTest();
    const int malformedMarker = RunMalformedMarkerFailClosedTest();
    if (atomicReplacement == 0 && malformedMarker == 0)
        std::cout << "SparkInstaller install-state recovery tests passed\n";
    return atomicReplacement == 0 && malformedMarker == 0 ? 0 : 1;
}
