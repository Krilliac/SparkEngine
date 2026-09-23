// OPS-100 regression: "Declined consent transmits nothing".
//
// The reporter asks for per-crash consent before its only network action (a
// public, metadata-only GitHub Issue for users who opted in locally). A
// detached watchdog, a service, or a CI job has no one to answer that prompt:
// stdin is closed, /dev/null, or broken. Such a non-answer must count as a
// refusal, and an answer that authorizes a public post must be an explicit yes.

#include "CrashReporterApp.h"
#include "CrashAutoIssues.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace
{
    namespace fs = std::filesystem;

    int failures = 0;
    int checks = 0;

    void Check(bool condition, std::string_view message)
    {
        ++checks;
        if (!condition)
        {
            std::cerr << "FAIL: " << message << '\n';
            ++failures;
        }
    }

    class ScratchDirectory
    {
      public:
        ScratchDirectory()
        {
            const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
            // Canonical so the reporter's redirected-ancestor check accepts it.
            path = fs::canonical(fs::temp_directory_path()) / ("spark-crash-consent-tests-" + std::to_string(nonce));
            fs::create_directories(path);
        }
        ~ScratchDirectory()
        {
            std::error_code error;
            fs::remove_all(path, error);
        }
        fs::path path;
    };

    class ScopedEnvironment
    {
      public:
        ScopedEnvironment(const char* name, const std::string& value) : m_name(name)
        {
            if (const char* old = std::getenv(name))
                m_previous = old;
            Set(value);
        }
        ~ScopedEnvironment()
        {
            if (m_previous)
                Set(*m_previous);
            else
            {
#ifdef _WIN32
                _putenv_s(m_name.c_str(), "");
#else
                unsetenv(m_name.c_str());
#endif
            }
        }

      private:
        void Set(const std::string& value)
        {
#ifdef _WIN32
            _putenv_s(m_name.c_str(), value.c_str());
#else
            setenv(m_name.c_str(), value.c_str(), 1);
#endif
        }
        std::string m_name;
        std::optional<std::string> m_previous;
    };

    /// Replaces std::cin for one reporter run so the consent prompt reads a
    /// scripted answer (or a closed/broken stream) instead of the terminal.
    class ScopedStdin
    {
      public:
        explicit ScopedStdin(std::string input, bool broken = false) : m_input(std::move(input))
        {
            m_previous = std::cin.rdbuf(m_input.rdbuf());
            std::cin.clear();
            if (broken)
                std::cin.setstate(std::ios::badbit);
        }
        ~ScopedStdin()
        {
            std::cin.rdbuf(m_previous);
            std::cin.clear();
        }
        ScopedStdin(const ScopedStdin&) = delete;
        ScopedStdin& operator=(const ScopedStdin&) = delete;

      private:
        std::istringstream m_input;
        std::streambuf* m_previous = nullptr;
    };

    std::string ReadAll(const fs::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    }

    size_t CountCalls(const fs::path& capture)
    {
        const std::string text = ReadAll(capture);
        size_t count = 0;
        for (size_t at = text.find("--end-call--"); at != std::string::npos; at = text.find("--end-call--", at + 1))
            ++count;
        return count;
    }

    SparkCrashReporter::CrashManifest MakeConsentManifest(const fs::path& scratch, std::string_view label)
    {
        const fs::path root = scratch / ("consent-" + std::string(label));
        fs::create_directories(root);
#ifndef _WIN32
        chmod(root.c_str(), S_IRWXU);
#endif
        const fs::path log = root / ("GameEngineCrash_20260923_" + std::string(label) + ".log");
        {
            std::ofstream output(log, std::ios::binary);
            output << "private-log-sentinel\n";
        }
        const fs::path manifestFile = root / "manifest.json";
        {
            std::ofstream output(manifestFile, std::ios::binary);
            // requireConsent is true: the engine asked for a per-crash prompt.
            output << "{\"logFile\":\"" << log.filename().string()
                   << "\",\"crashTitle\":\"SIGSEGV\",\"requireConsent\":true,"
                      "\"allowScreenshotRefusal\":false,\"promptUserDescription\":false}";
        }
        SparkCrashReporter::CrashManifest loaded;
        Check(SparkCrashReporter::LoadManifest(manifestFile.string(), loaded), "load consent manifest fixture");
        return loaded;
    }

    bool AttemptReceiptExists(const SparkCrashReporter::CrashManifest& manifest)
    {
        return fs::exists(fs::path(manifest.artifactRoot) /
                          ("issue_attempt_" + SparkCrashReporter::CrashReceiptKey(manifest) + ".txt"));
    }

    void TestConsentAnswerParsing()
    {
        using SparkCrashReporter::ReadConsentAnswer;
        const auto answer = [](std::string text, bool emptyMeansYes, bool broken = false)
        {
            std::istringstream input(std::move(text));
            if (broken)
                input.setstate(std::ios::badbit);
            return ReadConsentAnswer(input, emptyMeansYes);
        };

        // No answer at all is never consent, whatever the prompt's default.
        Check(!answer("", true), "CrashReporter_Consent: closed stdin (EOF) is a refusal even for a [Y/n] prompt");
        Check(!answer("", false), "CrashReporter_Consent: closed stdin (EOF) is a refusal");
        Check(!answer("y\n", true, true), "CrashReporter_Consent: a broken stdin stream is a refusal");

        // Interactive Enter keeps the prompt's documented default.
        Check(answer("\n", true), "CrashReporter_Consent: Enter accepts a [Y/n] local-review prompt");
        Check(!answer("\n", false), "CrashReporter_Consent: Enter declines a [y/N] publication prompt");
        Check(!answer("   \r\n", false), "CrashReporter_Consent: whitespace-only answer declines [y/N]");

        // Only an explicit yes counts; words that merely start with 'y' do not.
        Check(answer("y\n", false), "CrashReporter_Consent: 'y' accepts");
        Check(answer("YES\r\n", false), "CrashReporter_Consent: 'YES' with CRLF accepts");
        Check(answer("  yes  \n", false), "CrashReporter_Consent: padded 'yes' accepts");
        Check(answer("y", false), "CrashReporter_Consent: 'y' without trailing newline accepts");
        Check(!answer("yikes\n", false), "CrashReporter_Consent: 'yikes' is not consent");
        Check(!answer("yes please don't\n", false), "CrashReporter_Consent: trailing words are not consent");
        Check(!answer("n\n", true), "CrashReporter_Consent: 'n' declines");
        Check(!answer("no\n", true), "CrashReporter_Consent: 'no' declines");
        Check(!answer("\ny\n", false), "CrashReporter_Consent: only the first line is the answer");
    }

#ifndef _WIN32
    // POSIX reads consent from stdin. Windows uses MessageBoxA, which returns
    // 0 (not IDYES) when no interactive desktop exists, so it already fails
    // closed and cannot be driven from an unattended test without a desktop.
    void TestUnansweredPromptTransmitsNothing(const fs::path& scratch, const fs::path& fakeGh)
    {
        const fs::path fakeDirectory = scratch / "fake-gh-bin";
        fs::create_directories(fakeDirectory);
        const fs::path fakeExecutable = fakeDirectory / "gh";
        std::error_code copyError;
        fs::copy_file(fakeGh, fakeExecutable, fs::copy_options::overwrite_existing, copyError);
        Check(!copyError && fs::is_regular_file(fakeExecutable), "install fake gh in isolated PATH");
        if (copyError || !fs::is_regular_file(fakeExecutable))
            return; // Never fall back to the real GitHub CLI.
        fs::permissions(fakeExecutable, fs::perms::owner_exec, fs::perm_options::add, copyError);

        const fs::path capture = scratch / "fake-gh-arguments.txt";
        ScopedEnvironment path("PATH", fakeDirectory.string());
        ScopedEnvironment capturePath("SPARK_FAKE_GH_CAPTURE", capture.string());
        ScopedEnvironment mode("SPARK_FAKE_GH_MODE", "success");

        // The user has opted in locally; only the per-crash answer differs.
        Check(SparkCrashReporter::SetAutoIssuesEnabled(true), "user opts in to automatic issues");
        Check(SparkCrashReporter::AutoIssuesEnabled(), "opt-in is active for the consent matrix");

        struct Case
        {
            std::string_view label;
            std::string input;
            bool broken;
        };
        const Case declined[] = {
            {"eof", "", false},          // detached watchdog / service: stdin is /dev/null
            {"broken", "y\n", true},     // stdin stream already failed
            {"enter", "\n", false},      // bare Enter on a publication prompt
            {"yikes", "yikes\n", false}, // not an explicit yes
            {"no", "n\n", false},        // explicit refusal
        };
        for (const Case& item : declined)
        {
            const auto manifest = MakeConsentManifest(scratch, item.label);
            const size_t before = CountCalls(capture);
            int result = -1;
            {
                ScopedStdin stdinScript(item.input, item.broken);
                result = SparkCrashReporter::RunCrashReporter(manifest);
            }
            const std::string label(item.label);
            Check(result == 0, "CrashReporter_Consent: declined/unanswered prompt exits cleanly (" + label + ")");
            Check(CountCalls(capture) == before,
                  "CrashReporter_Consent: declined/unanswered consent transmits nothing (" + label + ")");
            Check(!AttemptReceiptExists(manifest),
                  "CrashReporter_Consent: declined/unanswered consent claims no issue attempt (" + label + ")");
            Check(fs::exists(manifest.logFile),
                  "CrashReporter_Consent: declined consent keeps the local crash log (" + label + ")");
        }

        // Positive control: an explicit yes does reach the (fake) transport, so
        // the zero-call assertions above are able to observe a transmission.
        const Case accepted[] = {{"yes", "y\n", false}, {"yes-crlf", "YES\r\n", false}};
        for (const Case& item : accepted)
        {
            const auto manifest = MakeConsentManifest(scratch, item.label);
            const size_t before = CountCalls(capture);
            int result = -1;
            {
                ScopedStdin stdinScript(item.input, item.broken);
                result = SparkCrashReporter::RunCrashReporter(manifest);
            }
            const std::string label(item.label);
            Check(result == 0, "CrashReporter_Consent: explicit yes confirms the fake issue (" + label + ")");
            Check(CountCalls(capture) == before + 1,
                  "CrashReporter_Consent: explicit yes transmits exactly once (" + label + ")");
        }
        const std::string sent = ReadAll(capture);
        Check(sent.find("private-log-sentinel") == std::string::npos,
              "CrashReporter_Consent: accepted issue still carries no log content");

        Check(SparkCrashReporter::SetAutoIssuesEnabled(false), "user revokes automatic issue opt-in");
    }
#endif
} // namespace

int main(int argc, char* argv[])
{
    ScratchDirectory scratch;
#ifdef _WIN32
    ScopedEnvironment configRoot("LOCALAPPDATA", (scratch.path / "private-config").string());
#else
    ScopedEnvironment configRoot("XDG_CONFIG_HOME", (scratch.path / "private-config").string());
#endif
    TestConsentAnswerParsing();
#ifndef _WIN32
    if (argc == 2)
        TestUnansweredPromptTransmitsNothing(scratch.path, argv[1]);
    else
        Check(false, "fake gh executable path must be supplied");
#else
    (void)argc;
    (void)argv;
#endif

    if (checks == 0)
    {
        std::cerr << "CrashReporter consent tests ran no checks\n";
        return 1;
    }
    if (failures != 0)
    {
        std::cerr << failures << " of " << checks << " CrashReporter consent check(s) failed\n";
        return 1;
    }
    std::cout << "CrashReporter consent tests passed (" << checks << " checks)\n";
    return 0;
}
