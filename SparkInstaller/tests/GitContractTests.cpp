#include "GitBootstrap.h"
#include "GitRunner.h"

#include <cctype>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

    int RunFakeGit(int argc, char* argv[])
    {
        const std::string command = argv[1];
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
    failures += Check(git.Clone("./local-repository", "Working", (testRoot / "local clone").string(), {}),
                      "explicit local repository path was rejected");
    failures += Check(!git.CheckoutRef("release-branch", (testRoot / "checkout").string(), {}),
                      "branch update reported success after fake git pull failed");
    failures += Check(git.CheckoutRef("v1.2.3", (testRoot / "checkout").string(), {}),
                      "detached tag checkout incorrectly required git pull");

    std::filesystem::remove_all(testRoot, filesystemError);

    if (failures == 0)
        std::cout << "SparkInstaller git process contract tests passed\n";
    return failures == 0 ? 0 : 1;
}
