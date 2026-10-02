/**
 * @file FuzzSparkBuildConfigProduction.cpp
 * @brief libc++-compiled production adapter for the sparkbuild.ini libFuzzer harness.
 *
 * SparkBuild reads sparkbuild.ini with ConfigManager::Load, which bounds the
 * file and hands its text to LoadFromStream. The loaded paths and preset then
 * flow into BuildCMakeConfigureCommand and BuildCMakeBuildCommand, whose
 * strings ProcessRunner runs through /bin/sh -c. The adapter loads a fixed
 * baseline, feeds the fuzz bytes to the shipped LoadFromStream, builds both
 * commands and aborts, so libFuzzer records a crash rather than a silent
 * pass, when:
 *  - a rejected document changed the previously loaded configuration (the
 *    load is documented as transactional),
 *  - a command builder refused (std::invalid_argument) although no value it
 *    quotes holds a character the POSIX quoting rule forbids,
 *  - an independent POSIX sh word split of a built command finds an unquoted
 *    shell metacharacter (; | & < > ( ) or a line break), an expansion or
 *    escape character ($ ` \) inside quotes, or an unterminated quote,
 *  - the executable, EnginePath, BuildPath or CMakePreset value is not exactly
 *    one whole argv word in its expected position, or the word count differs
 *    from what the configuration implies.
 *
 * The fuzz targets run on Linux, so this covers the /bin/sh quoting branch of
 * QuoteCommandArgument; the cmd.exe branch is pinned by the SparkBuildConfig_*
 * unit tests on the Windows build.
 */

#include "FuzzSparkBuildConfigProduction.h"

#include "Config.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;

    // A baseline with non-default values in every section, so a partially
    // applied rejected document would show up in the fingerprint.
    constexpr const char* kBaselineDocument = "[Paths]\nEnginePath=baseline engine\nBuildPath=baseline build\n"
                                              "[Build]\nGenerator=UnixMakefiles\nBuildType=Debug\nParallelJobs=3\n"
                                              "[Options]\nENABLE_RECAST=OFF\n";

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzSparkBuildConfig: SparkBuild configuration violated: %s\n", what);
        std::abort();
    }

    std::string Fingerprint(const SparkBuild::BuildConfig& config)
    {
        std::string text =
            config.enginePath + '\x1f' + config.buildPath + '\x1f' + config.cmakePath + '\x1f' + config.msvcToolset +
            '\x1f' + config.cmakePreset + '\x1f' + std::to_string(static_cast<int>(config.generator)) + '\x1f' +
            std::to_string(static_cast<int>(config.buildType)) + '\x1f' + std::to_string(config.parallelJobs);
        for (const SparkBuild::BuildOption& option : config.options)
            text += '\x1f' + option.cmakeVar + (option.currentValue ? "=1" : "=0");
        return text;
    }

    /// Characters the POSIX branch of QuoteCommandArgument must refuse.
    bool HasUnquotableCharacter(std::string_view value)
    {
        return value.find_first_of(std::string_view("\0\r\n\"$`\\", 7)) != std::string_view::npos;
    }

    /// Word-split @p command the way /bin/sh -c would for a command that uses
    /// only double quotes, aborting on anything the shell would interpret.
    std::vector<std::string> SplitShellWords(const std::string& command)
    {
        std::vector<std::string> words;
        std::string word;
        bool inWord = false;
        bool quoted = false;
        for (const char c : command)
        {
            if (c == '\0' || c == '\n' || c == '\r')
                InvariantFailure("a built command contains a NUL or line break");
            if (quoted)
            {
                if (c == '"')
                    quoted = false;
                else if (c == '$' || c == '`' || c == '\\')
                    InvariantFailure("a built command keeps an expansion or escape character inside quotes");
                else
                    word += c;
                continue;
            }
            if (c == ' ' || c == '\t')
            {
                if (inWord)
                    words.push_back(word);
                word.clear();
                inWord = false;
                continue;
            }
            if (std::string_view(";|&<>()$`\\'*?[]#~{}").find(c) != std::string_view::npos)
                InvariantFailure("a built command has an unquoted shell metacharacter");
            inWord = true;
            if (c == '"')
                quoted = true;
            else
                word += c;
        }
        if (quoted)
            InvariantFailure("a built command has an unterminated quote");
        if (inWord)
            words.push_back(word);
        return words;
    }

    void RequireWord(const std::vector<std::string>& words, std::size_t index, const std::string& expected,
                     const char* what)
    {
        if (index >= words.size() || words[index] != expected)
            InvariantFailure(what);
    }

    void CheckConfigureCommand(const SparkBuild::ConfigManager& manager)
    {
        const SparkBuild::BuildConfig& config = manager.config;
        std::string command;
        try
        {
            command = manager.BuildCMakeConfigureCommand();
        }
        catch (const std::invalid_argument&)
        {
            const bool justified = HasUnquotableCharacter(config.cmakePath) ||
                                   HasUnquotableCharacter(config.enginePath) ||
                                   (config.cmakePreset.empty() ? HasUnquotableCharacter(config.buildPath)
                                                               : HasUnquotableCharacter(config.cmakePreset));
            if (!justified)
                InvariantFailure("the configure command was refused without an unquotable value");
            return;
        }

        const std::vector<std::string> words = SplitShellWords(command);
        const std::string executable = config.cmakePath.empty() ? "cmake" : config.cmakePath;
        const std::string source = config.enginePath.empty() ? "." : config.enginePath;
        RequireWord(words, 0, executable, "CMakePath is not the whole first argv word");
        if (!config.cmakePreset.empty())
        {
            RequireWord(words, 1, "--preset", "the preset command lost --preset");
            RequireWord(words, 2, config.cmakePreset, "CMakePreset is not one whole argv word");
            RequireWord(words, 3, "-S", "the preset command lost -S");
            RequireWord(words, 4, source, "EnginePath is not one whole argv word");
            if (words.size() != 5)
                InvariantFailure("the preset configure command has extra argv words");
            return;
        }

        const bool singleConfig = config.generator == SparkBuild::Generator::Ninja ||
                                  config.generator == SparkBuild::Generator::UnixMakefiles;
        RequireWord(words, 1, "-S", "the configure command lost -S");
        RequireWord(words, 2, source, "EnginePath is not one whole argv word");
        RequireWord(words, 3, "-B", "the configure command lost -B");
        RequireWord(words, 4, config.buildPath.empty() ? "build" : config.buildPath,
                    "BuildPath is not one whole argv word");
        RequireWord(words, 5, "-G", "the configure command lost -G");
        RequireWord(words, 6, SparkBuild::GeneratorToString(config.generator), "the generator is not one argv word");
        if (words.size() != 7 + (singleConfig ? 1u : 0u) + config.options.size())
            InvariantFailure("the configure command has a different number of argv words than its options");
    }

    void CheckBuildCommand(const SparkBuild::ConfigManager& manager)
    {
        const SparkBuild::BuildConfig& config = manager.config;
        std::string command;
        try
        {
            command = manager.BuildCMakeBuildCommand();
        }
        catch (const std::invalid_argument&)
        {
            if (!HasUnquotableCharacter(config.cmakePath) && !HasUnquotableCharacter(config.buildPath))
                InvariantFailure("the build command was refused without an unquotable value");
            return;
        }

        const std::vector<std::string> words = SplitShellWords(command);
        RequireWord(words, 0, config.cmakePath.empty() ? "cmake" : config.cmakePath,
                    "CMakePath is not the whole first argv word");
        RequireWord(words, 1, "--build", "the build command lost --build");
        RequireWord(words, 2, config.buildPath.empty() ? "build" : config.buildPath,
                    "BuildPath is not one whole argv word");
        RequireWord(words, 3, "--config", "the build command lost --config");
        RequireWord(words, 4, SparkBuild::BuildTypeToString(config.buildType), "the build type is not one argv word");
        RequireWord(words, 5, "--parallel", "the build command lost --parallel");
        const std::size_t expected = config.parallelJobs > 0 ? 7 : 6;
        if (config.parallelJobs > 0)
            RequireWord(words, 6, std::to_string(config.parallelJobs), "ParallelJobs is not one argv word");
        if (words.size() != expected)
            InvariantFailure("the build command has extra argv words");
    }
} // namespace

// This C ABI is the only boundary between the libstdc++ libFuzzer executable
// and the libc++-compiled SparkBuild configuration code.
extern "C" int SparkFuzzLoadSparkBuildConfig(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes)
        return 0;
    if (data == nullptr && size != 0)
        return 0;

    SparkBuild::ConfigManager manager;
    std::istringstream baseline(kBaselineDocument);
    if (!manager.LoadFromStream(baseline))
        InvariantFailure("the baseline document is rejected");
    const std::string before = Fingerprint(manager.config);

    std::istringstream input(size == 0 ? std::string() : std::string(reinterpret_cast<const char*>(data), size));
    if (!manager.LoadFromStream(input))
    {
        if (Fingerprint(manager.config) != before)
            InvariantFailure("a rejected document modified the loaded configuration");
        return 0;
    }

    CheckConfigureCommand(manager);
    CheckBuildCommand(manager);
    return 0;
}
