#include "CrashAutoIssues.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string_view>

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
    return 0;
}
