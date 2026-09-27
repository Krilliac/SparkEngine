#include "GitBootstrap.h"
#include "GitRunner.h"
#include "ProcessRunner.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Defined in PortableGitCacheTests.cpp; returns the number of failed checks.
int RunPortableGitCacheTests();

namespace
{
    std::vector<std::string> DecodeProcessRunnerCommand(std::string_view command)
    {
        std::vector<std::string> arguments;
        std::string current;
        bool inQuotes = false;
        for (size_t index = 0; index < command.size(); ++index)
        {
            const char character = command[index];
            if (character == '\\' && index + 1 < command.size() &&
                (command[index + 1] == '"' || command[index + 1] == '\\'))
            {
                current.push_back(command[++index]);
            }
            else if (character == '"')
            {
                inQuotes = !inQuotes;
            }
            else if (std::isspace(static_cast<unsigned char>(character)) && !inQuotes)
            {
                if (!current.empty())
                {
                    arguments.push_back(std::move(current));
                    current.clear();
                }
            }
            else
            {
                current.push_back(character);
            }
        }
        if (!current.empty())
            arguments.push_back(std::move(current));
        return arguments;
    }

    int Check(bool condition, const std::string& message)
    {
        if (condition)
            return 0;
        std::cerr << "FAIL: " << message << '\n';
        return 1;
    }

    void SetFakeGitStatus(std::string_view value)
    {
#ifdef _WIN32
        _putenv_s("SPARK_FAKE_GIT_STATUS", std::string(value).c_str());
#else
        setenv("SPARK_FAKE_GIT_STATUS", std::string(value).c_str(), 1);
#endif
    }

    bool WriteTextFile(const std::filesystem::path& path, std::string_view text)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
        out.close();
        return static_cast<bool>(out);
    }

    int RunRealGit(const std::filesystem::path& repository, const std::string& arguments)
    {
        SparkBuild::ProcessRunner runner;
        std::string output;
        return runner.RunSync("git " + arguments, repository.string(), output);
    }

    // Proves the installer-marker exemption against real `git status
    // --porcelain` output: a freshly installed checkout carries an untracked
    // .sparkengine-install.json, which must not block the next update.
    int RunRealGitMarkerCleanlinessTests(const std::filesystem::path& repository)
    {
        std::error_code error;
        std::filesystem::create_directories(repository, error);
        int failures = Check(!error, "could not create real-git test repository directory");
        if (failures != 0)
            return failures;

        failures += Check(RunRealGit(repository, "init --quiet") == 0, "real git init failed");
        // A global core.autocrlf/safecrlf can print line-ending warnings, which
        // ProcessRunner merges into status output; keep the fixture neutral.
        failures += Check(RunRealGit(repository, "config core.autocrlf false") == 0 &&
                              RunRealGit(repository, "config core.safecrlf false") == 0,
                          "real git config failed");
        failures += Check(WriteTextFile(repository / "tracked.txt", "tracked\n"), "could not write tracked file");
        failures += Check(RunRealGit(repository, "add tracked.txt") == 0, "real git add failed");
        failures += Check(RunRealGit(repository, "-c user.name=SparkInstallerTests -c user.email=tests@invalid "
                                                 "-c commit.gpgsign=false commit --quiet -m initial") == 0,
                          "real git commit failed");

        const SparkInstaller::GitRunner git("git");
        failures += Check(git.WorkingTreeClean(repository.string(), {}), "freshly committed real repository was dirty");

        failures += Check(WriteTextFile(repository / ".sparkengine-install.json", "{}\n"),
                          "could not write real-git install marker");
        failures += Check(WriteTextFile(repository / ".sparkengine-install.json.tmp", "{}\n"),
                          "could not write real-git install marker staging file");
        failures += Check(git.WorkingTreeClean(repository.string(), {}),
                          "real git status: installer marker files blocked an update of a clean install");

        failures += Check(WriteTextFile(repository / "user.txt", "user data\n"), "could not write real-git user file");
        failures += Check(!git.WorkingTreeClean(repository.string(), {}),
                          "real git status: untracked user file beside the marker was accepted as clean");
        std::filesystem::remove(repository / "user.txt", error);

        failures += Check(WriteTextFile(repository / "tracked.txt", "modified\n"), "could not modify tracked file");
        failures += Check(!git.WorkingTreeClean(repository.string(), {}),
                          "real git status: modified tracked file beside the marker was accepted as clean");
        return failures;
    }

    int RunFakeGit(int argc, char* argv[])
    {
        const std::string command = argv[1];
        if (command == "status")
        {
            if (const char* status = std::getenv("SPARK_FAKE_GIT_STATUS");
                status && std::string_view(status) == "error")
                return 47;
            const char* rawStatus = std::getenv("SPARK_FAKE_GIT_STATUS");
            const std::string_view status = rawStatus ? rawStatus : "";
            if (status == "dirty")
                std::cout << " M user-change.txt\n";
            if (status == "marker-only" || status == "marker-plus-user")
                std::cout << "?? .sparkengine-install.json\n?? .sparkengine-install.json.tmp\n";
            if (status == "marker-plus-user")
                std::cout << "?? user.txt\n";
            if (status == "marker-modified")
                std::cout << " M .sparkengine-install.json\n";
            if (status == "marker-staged")
                std::cout << "A  .sparkengine-install.json\n";
            return 0;
        }
        if (command == "clone")
        {
            bool foundOptionTerminator = false;
            for (int index = 2; index < argc; ++index)
                foundOptionTerminator = foundOptionTerminator || std::string_view(argv[index]) == "--";
            return foundOptionTerminator ? 0 : 41;
        }
        if (command == "checkout")
            return argc >= 3 ? 0 : 42;
        if (command == "show-ref" && argc >= 5)
        {
            return std::string_view(argv[4]).find("branch") != std::string_view::npos ? 0 : 1;
        }
        if (command == "pull")
            return 23;
        return 0;
    }
} // namespace

int main(int argc, char* argv[])
{
    if (argc > 1)
        return RunFakeGit(argc, argv);

    int failures = 0;
    const std::vector<std::string> expected = {
        "git executable with spaces & metacharacters/git", "argument with spaces", "quote\"inside",
        "semi;dollar$amp&pipe|caret^percent%bang!",        "backslash\\tail\\",    "single'quote",
    };

    std::string command;
    for (const auto& argument : expected)
    {
        if (!command.empty())
            command.push_back(' ');
        command += SparkInstaller::GitRunner::EncodeProcessRunnerArgument(argument);
    }
    const auto decoded = DecodeProcessRunnerCommand(command);
    failures +=
        Check(decoded == expected, "encoded git argv did not round-trip through ProcessRunner's parser contract");
    failures += Check(command.find("'\\''") == std::string::npos,
                      "shell-specific POSIX single-quote escaping is still present");
    failures +=
        Check(SparkInstaller::GitBootstrap::IsGitAvailableOnPath(), "shell-less `git --version` PATH discovery failed");

    const auto fakeGit = std::filesystem::absolute(argv[0]);
    const auto testRoot = std::filesystem::temp_directory_path() / "SparkInstallerGitContractTests";
    std::error_code filesystemError;
    std::filesystem::remove_all(testRoot, filesystemError);
    filesystemError.clear();
    std::filesystem::create_directories(testRoot / "checkout", filesystemError);
    failures += Check(!filesystemError, "could not create fake-git test directory");

    SparkInstaller::GitRunner git(fakeGit.string());
    failures += Check(git.Clone("https://github.com/Krilliac/SparkEngine.git", "Working",
                                (testRoot / "clone with spaces").string(), {}),
                      "valid clone arguments or the git option terminator were rejected");
    failures += Check(!git.Clone("--upload-pack=attacker", "Working", (testRoot / "clone").string(), {}),
                      "option-shaped repository URL was accepted");
    failures += Check(
        !git.Clone("https://github.com/Krilliac/SparkEngine.git", "Working", "--config=core.sshCommand=attacker", {}),
        "option-shaped clone destination was accepted");
    SetFakeGitStatus("clean");
    failures +=
        Check(git.WorkingTreeClean((testRoot / "checkout").string(), {}), "clean install was not recognized as clean");
    SetFakeGitStatus("dirty");
    failures +=
        Check(!git.WorkingTreeClean((testRoot / "checkout").string(), {}), "dirty install was accepted for update");
    SetFakeGitStatus("error");
    failures += Check(!git.WorkingTreeClean((testRoot / "checkout").string(), {}),
                      "failed install status query was accepted for update");
    SetFakeGitStatus("marker-only");
    failures += Check(git.WorkingTreeClean((testRoot / "checkout").string(), {}),
                      "untracked installer marker files made a clean install look dirty");
    SetFakeGitStatus("marker-plus-user");
    failures += Check(!git.WorkingTreeClean((testRoot / "checkout").string(), {}),
                      "untracked user file was hidden by the installer marker exemption");
    SetFakeGitStatus("marker-modified");
    failures += Check(!git.WorkingTreeClean((testRoot / "checkout").string(), {}),
                      "a modified tracked installer marker was accepted as clean");
    SetFakeGitStatus("marker-staged");
    failures += Check(!git.WorkingTreeClean((testRoot / "checkout").string(), {}),
                      "a staged installer marker was accepted as clean");
    SetFakeGitStatus("clean");
    failures += RunRealGitMarkerCleanlinessTests(testRoot / "real-git");
    failures += Check(git.Clone("./local-repository", "Working", (testRoot / "local clone").string(), {}),
                      "explicit local repository path was rejected");
    failures += Check(!git.CheckoutRef("release-branch", (testRoot / "checkout").string(), {}),
                      "branch update reported success after fake git pull failed");
    failures += Check(git.CheckoutRef("v1.2.3", (testRoot / "checkout").string(), {}),
                      "detached tag checkout incorrectly required git pull");

    std::filesystem::remove_all(testRoot, filesystemError);

    failures += RunPortableGitCacheTests();

    if (failures == 0)
        std::cout << "SparkInstaller git process contract tests passed\n";
    return failures == 0 ? 0 : 1;
}
