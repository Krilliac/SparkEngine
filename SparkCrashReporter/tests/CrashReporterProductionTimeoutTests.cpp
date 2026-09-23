#include "CrashAutoIssues.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace
{
    struct BodyTestCleanup
    {
        std::filesystem::path root;

        ~BodyTestCleanup()
        {
            namespace fs = std::filesystem;
            std::error_code ignored;
            fs::remove(root / "SparkEngine" / "CrashReporter" / "auto-issues-v1.enabled", ignored);
            fs::remove(root / "SparkEngine" / "CrashReporter", ignored);
            fs::remove(root / "SparkEngine", ignored);
            fs::remove(root / "fake-gh-capture.txt", ignored);
#ifdef _WIN32
            fs::remove(root / "gh.exe", ignored);
#else
            fs::remove(root / "gh", ignored);
#endif
            fs::remove(root / "artifacts", ignored);
            fs::remove(root, ignored);
        }
    };
} // namespace

int main(int argc, char* argv[])
{
    if (argc == 3 && std::string_view(argv[1]) == "--real-gh-version")
    {
        const std::string result = SparkCrashReporter::ProbeGhVersion(std::filesystem::absolute(argv[2]));
        std::cout << result << '\n';
        return result == "ok" ? 0 : 1;
    }
    if (argc != 2 || !std::filesystem::is_regular_file(argv[1]))
        return 2;
#ifdef _WIN32
    _putenv_s("SPARK_FAKE_GH_MODE", "delayed-success");
#else
    setenv("SPARK_FAKE_GH_MODE", "delayed-success", 1);
#endif
    SparkCrashReporter::PreparedAutoIssue prepared;
    prepared.ready = true;
    prepared.ghExecutable = std::filesystem::absolute(argv[1]);
    prepared.workingDirectory = std::filesystem::temp_directory_path();
    prepared.body = "Synthetic subprocess timing check; no network or crash data.";
    const auto result = SparkCrashReporter::SubmitPreparedAutoIssue(prepared);
    if (!result.delivered || result.issueUrl != "https://github.com/Krilliac/SparkEngine/issues/42")
    {
        std::cerr << "Production timeout regression: " << result.reason << '\n';
        return 1;
    }

    // Prepare the real public payload in an isolated opt-in root. The only
    // executable on PATH is a copy of the local fake GitHub CLI.
    namespace fs = std::filesystem;
    const fs::path testRoot =
        fs::absolute(argv[1]).parent_path() /
        ("auto-issue-body-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const BodyTestCleanup cleanup{testRoot};
    const fs::path artifactRoot = testRoot / "artifacts";
#ifdef _WIN32
    const fs::path fakeGh = testRoot / "gh.exe";
#else
    const fs::path fakeGh = testRoot / "gh";
#endif
    std::error_code error;
    fs::create_directories(artifactRoot, error);
    fs::copy_file(fs::absolute(argv[1]), fakeGh, error);
#ifndef _WIN32
    fs::permissions(fakeGh, fs::perms::owner_exec, fs::perm_options::add, error);
#endif
    if (error)
    {
        std::cerr << "Could not prepare isolated fake GitHub CLI: " << error.message() << '\n';
        return 2;
    }
    const fs::path capture = testRoot / "fake-gh-capture.txt";
#ifdef _WIN32
    _putenv_s("LOCALAPPDATA", testRoot.string().c_str());
    _putenv_s("PATH", testRoot.string().c_str());
    _putenv_s("SPARK_FAKE_GH_CAPTURE", capture.string().c_str());
#else
    setenv("XDG_CONFIG_HOME", testRoot.string().c_str(), 1);
    setenv("PATH", testRoot.string().c_str(), 1);
    setenv("SPARK_FAKE_GH_CAPTURE", capture.string().c_str(), 1);
#endif
    if (!SparkCrashReporter::SetAutoIssuesEnabled(true))
    {
        std::cerr << "Could not enable isolated automatic Issue test setting\n";
        return 2;
    }
    SparkCrashReporter::CrashManifest manifest;
    manifest.artifactRoot = artifactRoot.string();
    manifest.logFile = "private-path-sentinel";
    manifest.crashTitle = "SIGSEGV";
    constexpr std::string_view incident = "0123456789abcdef0123456789abcdef";
    const auto known = SparkCrashReporter::PrepareAutoIssue(manifest, std::string(incident));
    if (!known.ready || known.body.find("Crash class: SIGSEGV") == std::string::npos ||
        known.body.find("private-path-sentinel") != std::string::npos || known.body.size() > 512 ||
        !SparkCrashReporter::SubmitPreparedAutoIssue(known).delivered)
    {
        std::cerr << "Known engine crash class was not submitted safely through fake gh\n";
        return 1;
    }
    manifest.crashTitle = "private-title-sentinel";
    const auto unknown = SparkCrashReporter::PrepareAutoIssue(manifest, std::string(incident));
    if (!unknown.ready || unknown.body.find("Crash class: Unknown") == std::string::npos ||
        unknown.body.find("private-title-sentinel") != std::string::npos || unknown.body.size() > 512 ||
        !SparkCrashReporter::SubmitPreparedAutoIssue(unknown).delivered)
    {
        std::cerr << "Untrusted crash title reached the public Issue body or fake gh failed\n";
        return 1;
    }
    std::ifstream captured(capture, std::ios::binary);
    const std::string sent(std::istreambuf_iterator<char>{captured}, {});
    if (sent.find("Crash class: SIGSEGV") == std::string::npos ||
        sent.find("Crash class: Unknown") == std::string::npos ||
        sent.find("private-title-sentinel") != std::string::npos ||
        sent.find("private-path-sentinel") != std::string::npos)
    {
        std::cerr << "Fake gh did not receive only allowlisted crash classes\n";
        return 1;
    }
    return 0;
}
