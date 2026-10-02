/**
 * @file FuzzEngineSettingsProduction.cpp
 * @brief libc++-compiled production adapter for the engine settings libFuzzer harness.
 *
 * Every engine start runs EngineSettings::Load on settings.ini, merges the gitignored
 * settings.local.ini override over it, drops the retired crash-upload credentials and
 * reads every reflected settings struct from the result. The fuzz bytes up to the first
 * NUL become settings.ini; the bytes after it, when there is a NUL, become
 * settings.local.ini. Both are real files in a private directory, so Load runs exactly as
 * it does at startup. A violated contract aborts so libFuzzer records a crash:
 *  - Load is transactional: a rejected file leaves every visible setting unchanged and
 *    does not touch the process assert policy,
 *  - an accepted load hands the loaded [Debug] values to the assert policy,
 *  - no retired [CrashReporting] credential key survives an accepted load,
 *  - an accepted load saved with SaveAs and loaded again shows identical settings, so a
 *    settings file the engine accepts round-trips through its own writer.
 *
 * Load ends by handing [Debug] to Assert::SetSuppressionEnabled and
 * Assert::SetDebugBreakOnSuppressed, whose production bodies (Utils/Assert.cpp) sit on the
 * crash handler and console. As SparkFuzzReflectedScene does for Assert::Fail, the adapter
 * defines those two sinks; here they record what Load applied so it can be checked.
 */

#include "FuzzEngineSettingsProduction.h"

#include "Core/EngineSettings.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>

namespace
{
    struct AppliedAssertPolicy
    {
        unsigned calls = 0;
        bool suppression = false;
        bool breakOnSuppressed = false;
    };
    AppliedAssertPolicy g_applied;
} // namespace

namespace Assert
{
    void SetSuppressionEnabled(bool enabled)
    {
        ++g_applied.calls;
        g_applied.suppression = enabled;
    }

    void SetDebugBreakOnSuppressed(bool enabled)
    {
        g_applied.breakOnSuppressed = enabled;
    }
} // namespace Assert

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzEngineSettings: EngineSettings::Load violated: %s\n", what);
        std::abort();
    }

    struct Paths
    {
        std::filesystem::path settings;
        std::filesystem::path local;
        std::filesystem::path saved;
        std::filesystem::path snapshot;
    };

    const Paths& FixturePaths()
    {
        static const Paths paths = []
        {
            std::string pattern = (std::filesystem::temp_directory_path() / "spark-fuzz-settings-XXXXXX").string();
            if (::mkdtemp(pattern.data()) == nullptr)
                InvariantFailure("could not create the fixture directory");
            const std::filesystem::path base(pattern);
            return Paths{base / "settings.ini", base / "settings.local.ini", base / "saved.ini", base / "snapshot.ini"};
        }();
        return paths;
    }

    void WriteFile(const std::filesystem::path& path, std::string_view bytes)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out)
            InvariantFailure("could not write a fixture file");
    }

    /// Every visible setting, as the settings writer renders it (one WriteToConfig pass).
    std::string Capture(const EngineSettings& settings, const std::filesystem::path& snapshot)
    {
        if (!settings.SaveAs(snapshot.string()))
            InvariantFailure("the settings could not be written");
        std::ifstream in(snapshot, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    bool IsRetiredCrashKey(const std::string& key)
    {
        static constexpr std::string_view kRetired[] = {"uploadurl",    "proxyurl",   "githubrepo",     "githubtoken",
                                                        "githublabels", "attachdump", "timeoutseconds", "smtpuser",
                                                        "smtppass",     "emailto",    "emailfrom"};
        std::string lowered = key;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return std::find(std::begin(kRetired), std::end(kRetired), lowered) != std::end(kRetired);
    }
} // namespace

extern "C" int SparkFuzzLoadEngineSettings(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0))
        return 0;
    const std::string_view input =
        size == 0 ? std::string_view() : std::string_view(reinterpret_cast<const char*>(data), size);
    const Paths& paths = FixturePaths();

    const std::size_t split = input.find('\0');
    WriteFile(paths.settings, input.substr(0, split));
    std::error_code ignored;
    std::filesystem::remove(paths.local, ignored);
    if (split != std::string_view::npos)
        WriteFile(paths.local, input.substr(split + 1));

    EngineSettings& settings = EngineSettings::GetInstance();
    const std::string before = Capture(settings, paths.snapshot);
    const unsigned appliedBefore = g_applied.calls;
    if (!settings.Load(paths.settings.string()))
    {
        if (Capture(settings, paths.snapshot) != before)
            InvariantFailure("a rejected settings file changed the loaded settings");
        if (g_applied.calls != appliedBefore)
            InvariantFailure("a rejected settings file changed the assert policy");
        return 0;
    }
    if (g_applied.calls == appliedBefore || g_applied.suppression != settings.Debug().suppressFatalAsserts ||
        g_applied.breakOnSuppressed != settings.Debug().breakOnSuppressedAsserts)
        InvariantFailure("an accepted load did not apply its [Debug] assert policy");

    const std::string loaded = Capture(settings, paths.snapshot);
    for (const std::string& key : settings.GetKeys("CrashReporting"))
    {
        if (IsRetiredCrashKey(key))
            InvariantFailure("a retired [CrashReporting] credential key survived the load");
    }

    if (!settings.SaveAs(paths.saved.string()))
        InvariantFailure("accepted settings could not be saved");
    if (!settings.Load(paths.saved.string()))
        InvariantFailure("the settings writer produced a file Load rejects");
    if (Capture(settings, paths.snapshot) != loaded)
        InvariantFailure("Load -> SaveAs -> Load changed the settings");
    return 0;
}
