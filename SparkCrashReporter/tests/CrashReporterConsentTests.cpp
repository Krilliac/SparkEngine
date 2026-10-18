// OPS-100 regression: "Declined consent transmits nothing".
//
// The reporter asks for per-crash consent before its only network action (a
// public, metadata-only GitHub Issue for users who opted in locally). A
// detached watchdog, a service, or a CI job has no one to answer that prompt:
// stdin is closed, /dev/null, or broken. Such a non-answer must count as a
// refusal, and an answer that authorizes a public post must be an explicit yes.

#include "CrashReporterApp.h"
#include "CrashAutoIssues.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
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

    SparkCrashReporter::CrashManifest MakeConsentManifest(const fs::path& scratch, std::string_view label,
                                                          bool requireConsent = true,
                                                          std::string_view crashTitleJson = "SIGSEGV")
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
            // requireConsent true means the engine asked for a per-crash
            // prompt; false is what a crafted manifest or a project setting
            // that only meant "no screenshot prompt" would send.
            output << "{\"logFile\":\"" << log.filename().string() << "\",\"crashTitle\":\"" << crashTitleJson
                   << "\",\"requireConsent\":" << (requireConsent ? "true" : "false")
                   << ",\"allowScreenshotRefusal\":false,\"promptUserDescription\":false}";
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

        // The answer limit is 64 bytes, inclusive.
        Check(answer("y" + std::string(63, ' ') + "\n", false), "CrashReporter_Consent: a 64-byte answer is read");
        Check(!answer("y" + std::string(64, ' ') + "\n", false), "CrashReporter_Consent: a 65-byte answer declines");
    }

    /// Serves an endless run of 'y' with no newline and counts what the reader
    /// pulls. An unbounded std::getline would drain all of it into memory.
    class EndlessAnswerBuffer : public std::streambuf
    {
      public:
        explicit EndlessAnswerBuffer(std::size_t limit) : m_limit(limit) { setg(m_chunk, m_chunk, m_chunk); }
        std::size_t Served() const { return m_served; }

      protected:
        int_type underflow() override
        {
            if (m_served >= m_limit)
                return traits_type::eof();
            const std::size_t count = std::min(sizeof(m_chunk), m_limit - m_served);
            std::fill(m_chunk, m_chunk + count, 'y');
            m_served += count;
            setg(m_chunk, m_chunk, m_chunk + count);
            return traits_type::to_int_type(m_chunk[0]);
        }

      private:
        char m_chunk[256] = {};
        std::size_t m_limit = 0;
        std::size_t m_served = 0;
    };

    void TestConsentAnswerIsBoundedBeforeBuffering()
    {
        // 64 MiB without a newline stands in for a pipe or /dev/zero on stdin.
        constexpr std::size_t kStreamBytes = std::size_t{64} * 1024 * 1024;
        EndlessAnswerBuffer endless(kStreamBytes);
        std::istream input(&endless);
        Check(!SparkCrashReporter::ReadConsentAnswer(input, true),
              "CrashReporter_Consent: an overlong newline-free answer declines");
        Check(endless.Served() <= 4096,
              "CrashReporter_Consent: the answer reader stops pulling input once past the 64-byte limit");
    }

    void TestTerminalSafeRendering()
    {
        using SparkCrashReporter::TerminalSafe;
        Check(TerminalSafe("SIGSEGV") == "SIGSEGV", "CrashReporter_TerminalSafe: plain ASCII is unchanged");
        Check(TerminalSafe("snowman \xE2\x98\x83 rocket \xF0\x9F\x9A\x80") ==
                  "snowman \xE2\x98\x83 rocket \xF0\x9F\x9A\x80",
              "CrashReporter_TerminalSafe: well-formed printable UTF-8 is unchanged");
        Check(TerminalSafe("\x1b]52;c;cHduZWQ=\x07") == "\\x1b]52;c;cHduZWQ=\\x07",
              "CrashReporter_TerminalSafe: ESC and BEL of an OSC 52 clipboard write are escaped");
        Check(TerminalSafe(std::string("a\nb\rc\td\x7f") + '\0' + "e") == "a\\x0ab\\x0dc\\x09d\\x7f\\x00e",
              "CrashReporter_TerminalSafe: C0 controls, DEL and NUL are escaped");
        Check(TerminalSafe("\xC2\x9B"
                           "2J") == "\\u009b2J",
              "CrashReporter_TerminalSafe: a UTF-8 encoded C1 CSI is escaped");
        Check(TerminalSafe("\x9B"
                           "2J") == "\\x9b2J",
              "CrashReporter_TerminalSafe: a raw 8-bit CSI byte is escaped");
        Check(TerminalSafe("\xC0\x9B") == "\\xc0\\x9b",
              "CrashReporter_TerminalSafe: an overlong encoding is escaped byte by byte");
        Check(TerminalSafe("\xE2\x98") == "\\xe2\\x98", "CrashReporter_TerminalSafe: a truncated sequence is escaped");
    }

    /// Runs the reporter with stderr captured.
    std::string CaptureReporterStderr(const SparkCrashReporter::CrashManifest& manifest, int& result)
    {
        std::ostringstream captured;
        std::streambuf* previous = std::cerr.rdbuf(captured.rdbuf());
        {
            ScopedStdin stdinScript("", false);
            result = SparkCrashReporter::RunCrashReporter(manifest);
        }
        std::cerr.rdbuf(previous);
        return captured.str();
    }

    void TestManifestControlsNeverReachTheTerminal(const fs::path& scratch)
    {
        // No prompt and no opt-in: this path runs unattended on every platform.
        Check(!SparkCrashReporter::AutoIssuesEnabled(), "terminal-escape case runs without automatic issues");
        const auto manifest = MakeConsentManifest(scratch, "terminal-escape", false,
                                                  "\\u001b]52;c;cHduZWQ=\\u0007\\u001b[2J\\u009b1A spoof");
        Check(manifest.crashTitle.find('\x1b') != std::string::npos,
              "fixture: the parsed crash title really contains ESC");
        int result = -1;
        const std::string output = CaptureReporterStderr(manifest, result);
        Check(result == 0, "CrashReporter_TerminalSafe: the reporter still reviews the manifest");
        Check(output.find('\x1b') == std::string::npos && output.find('\x07') == std::string::npos &&
                  output.find("\xC2\x9B") == std::string::npos,
              "CrashReporter_TerminalSafe: no manifest control character reaches stderr");
        Check(output.find("The engine has crashed: \\x1b]52;c;cHduZWQ=\\x07\\x1b[2J\\u009b1A spoof") !=
                  std::string::npos,
              "CrashReporter_TerminalSafe: the crash title is shown with visible escapes");
    }

    /// Runs on every platform through the dialog seam: requireConsent=false in
    /// a manifest must never waive the per-crash publication question.
    void TestManifestCannotWaivePublicationQuestion(const fs::path& scratch, const fs::path& fakeGh)
    {
        const fs::path fakeDirectory = scratch / "fake-gh-seam-bin";
        fs::create_directories(fakeDirectory);
#ifdef _WIN32
        const fs::path fakeExecutable = fakeDirectory / "gh.exe";
#else
        const fs::path fakeExecutable = fakeDirectory / "gh";
#endif
        std::error_code copyError;
        fs::copy_file(fakeGh, fakeExecutable, fs::copy_options::overwrite_existing, copyError);
        Check(!copyError && fs::is_regular_file(fakeExecutable), "install fake gh for the dialog-seam matrix");
        if (copyError || !fs::is_regular_file(fakeExecutable))
            return; // Never fall back to the real GitHub CLI.
#ifndef _WIN32
        fs::permissions(fakeExecutable, fs::perms::owner_exec, fs::perm_options::add, copyError);
#endif
        const fs::path capture = scratch / "fake-gh-seam-arguments.txt";
        ScopedEnvironment path("PATH", fakeDirectory.string());
        ScopedEnvironment capturePath("SPARK_FAKE_GH_CAPTURE", capture.string());
        ScopedEnvironment mode("SPARK_FAKE_GH_MODE", "success");
        Check(SparkCrashReporter::SetAutoIssuesEnabled(true), "user opts in for the dialog-seam matrix");

        for (const bool answer : {false, true})
        {
            const std::string label = answer ? "seam-yes" : "seam-no";
            const auto manifest = MakeConsentManifest(scratch, label, false);
            Check(!manifest.requireConsent, "fixture: manifest disables the local review prompt (" + label + ")");
            size_t publicationQuestions = 0;
            size_t outcomes = 0;
            SparkCrashReporter::ReporterUi ui;
            ui.ask = [&](const std::string&, const char*, bool publication)
            {
                if (publication)
                    ++publicationQuestions;
                return answer;
            };
            ui.notifyIssueOutcome = [&](const std::string&, bool) { ++outcomes; };

            const size_t before = CountCalls(capture);
            const int result = SparkCrashReporter::RunCrashReporter(manifest, ui);
            Check(result == 0, "CrashReporter_Consent: dialog-seam run exits cleanly (" + label + ")");
            Check(publicationQuestions == 1,
                  "CrashReporter_Consent: requireConsent=false still asks the publication question (" + label + ")");
            Check(CountCalls(capture) == before + (answer ? 1u : 0u),
                  "CrashReporter_Consent: only an explicit yes transmits (" + label + ")");
            Check(AttemptReceiptExists(manifest) == answer,
                  "CrashReporter_Consent: only an explicit yes claims an issue attempt (" + label + ")");
            Check(outcomes == (answer ? 1u : 0u),
                  "CrashReporter_Consent: a posted Issue always shows its outcome (" + label + ")");
        }
        Check(SparkCrashReporter::SetAutoIssuesEnabled(false), "user revokes the dialog-seam opt-in");
    }

    /// The dialog layer maps a backend answer to the ReporterUi seam. Anything but a clear Yes declines.
    void TestDialogBackendFailsClosed()
    {
        using namespace SparkCrashReporter;
        const std::pair<const char*, DialogBackend> declining[] = {
            {"failed", [](const DialogRequest&) { return DialogAnswer::Failed; }},
            {"no", [](const DialogRequest&) { return DialogAnswer::No; }},
            {"throwing", [](const DialogRequest&) -> DialogAnswer { throw std::runtime_error("no desktop"); }},
            {"empty", DialogBackend{}},
        };
        for (const auto& [label, backend] : declining)
        {
            const ReporterUi ui = MakeDialogReporterUi(backend);
            const std::string name(label);
            Check(!ui.ask("Send?", "Crash Report", false),
                  "CrashReporter_Consent: " + name + " dialog declines review");
            Check(!ui.ask("Post?", "Crash Report", true),
                  "CrashReporter_Consent: " + name + " dialog declines publication");
            Check(!ui.ask("Shot?", "Screenshot Consent", false),
                  "CrashReporter_Consent: " + name + " dialog declines the screenshot");
            Check(static_cast<bool>(ui.notifyIssueOutcome), "CrashReporter_Consent: outcome notifier exists");
        }

        std::vector<DialogRequest> seen;
        const ReporterUi ui = MakeDialogReporterUi(
            [&seen](const DialogRequest& request)
            {
                seen.push_back(request);
                return DialogAnswer::Yes;
            });
        Check(ui.ask("Review text", "Crash Report", false), "CrashReporter_Consent: a Yes answer confirms");
        Check(ui.ask("Post text", "Crash Report", true), "CrashReporter_Consent: a Yes answer confirms publication");
        Check(ui.ask("Shot text", "Screenshot Consent", false), "CrashReporter_Consent: a Yes answer confirms a shot");
        Check(seen.size() == 3, "CrashReporter_Consent: one dialog per question");
        if (seen.size() == 3)
        {
            Check(seen[0].title == "Crash Report" && seen[0].instruction == "SparkEngine stopped unexpectedly" &&
                      seen[0].content == "Review text" && seen[0].icon == DialogIcon::Information && seen[0].askYesNo &&
                      !seen[0].defaultNo,
                  "CrashReporter_Consent: review dialog is an informational Yes/No with Yes default");
            Check(seen[1].icon == DialogIcon::Shield && seen[1].defaultNo && seen[1].askYesNo &&
                      seen[1].content == "Post text",
                  "CrashReporter_Consent: publication dialog uses the shield and defaults to No");
            Check(seen[2].title == "Screenshot Consent" && seen[2].instruction.empty() && seen[2].askYesNo,
                  "CrashReporter_Consent: screenshot dialog keeps its title and has no instruction");
        }

        seen.clear();
        std::ostringstream captured;
        std::streambuf* previous = std::cerr.rdbuf(captured.rdbuf());
        ui.notifyIssueOutcome("Created https://example.test/issues/1", true);
        ui.notifyIssueOutcome("Not confirmed", false);
        std::cerr.rdbuf(previous);
        Check(seen.size() == 2 && seen[0].title == "SparkEngine Issue created" &&
                  seen[0].icon == DialogIcon::Information && !seen[0].askYesNo &&
                  seen[0].content == "Created https://example.test/issues/1" &&
                  seen[1].title == "SparkEngine Issue not confirmed" && seen[1].icon == DialogIcon::Warning,
              "CrashReporter_Consent: outcome dialogs show the plain-text message with an OK button");
    }

    /// Full reporter run on a dialog backend that cannot show anything: nothing is published. With the review
    /// prompt waived and automatic Issues off, no dialog is requested at all.
    void TestFailingDialogBackendTransmitsNothing(const fs::path& scratch)
    {
#ifdef _WIN32
        const fs::path fakeExecutable = scratch / "fake-gh-seam-bin" / "gh.exe";
#else
        const fs::path fakeExecutable = scratch / "fake-gh-seam-bin" / "gh";
#endif
        Check(fs::is_regular_file(fakeExecutable), "failing-backend run reuses the isolated fake gh");
        if (!fs::is_regular_file(fakeExecutable))
            return; // Never fall back to the real GitHub CLI.
        const fs::path capture = scratch / "fake-gh-seam-arguments.txt";
        ScopedEnvironment path("PATH", fakeExecutable.parent_path().string());
        ScopedEnvironment capturePath("SPARK_FAKE_GH_CAPTURE", capture.string());
        ScopedEnvironment mode("SPARK_FAKE_GH_MODE", "success");

        size_t requests = 0;
        const SparkCrashReporter::ReporterUi failing = SparkCrashReporter::MakeDialogReporterUi(
            [&requests](const SparkCrashReporter::DialogRequest&)
            {
                ++requests;
                return SparkCrashReporter::DialogAnswer::Failed;
            });

        Check(SparkCrashReporter::SetAutoIssuesEnabled(true), "user opts in for the failing-backend run");
        {
            const auto manifest = MakeConsentManifest(scratch, "backend-failed", false);
            const size_t before = CountCalls(capture);
            const int result = SparkCrashReporter::RunCrashReporter(manifest, failing);
            Check(result == 0, "CrashReporter_Consent: a failing dialog backend still exits cleanly");
            Check(requests == 1, "CrashReporter_Consent: the publication question is asked once");
            Check(CountCalls(capture) == before, "CrashReporter_Consent: a failing dialog backend transmits nothing");
            Check(!AttemptReceiptExists(manifest),
                  "CrashReporter_Consent: a failing dialog backend claims no issue attempt");
        }
        Check(SparkCrashReporter::SetAutoIssuesEnabled(false), "user revokes the failing-backend opt-in");

        requests = 0;
        const auto waived = MakeConsentManifest(scratch, "backend-waived", false);
        Check(SparkCrashReporter::RunCrashReporter(waived, failing) == 0,
              "CrashReporter_Consent: a waived review prompt runs without a dialog");
        Check(requests == 0, "CrashReporter_Consent: requireConsent=false with automatic Issues off asks nothing");
    }

#ifdef _WIN32
    /// The shipped exe must carry the comctl32 v6 / PerMonitorV2 manifest and the brand icon; without the manifest
    /// TaskDialogIndirect is unavailable and every dialog silently degrades to MessageBoxW.
    void TestExecutableResources(const fs::path& executable)
    {
        const HMODULE module = LoadLibraryExW(executable.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
        Check(module != nullptr, "CrashManifest_ExeResources: SparkCrashReporter.exe loads as a data file");
        if (module == nullptr)
            return;
        std::string manifest;
        constexpr WORD kManifestType = 24; // RT_MANIFEST, spelled numerically so UNICODE does not matter
        if (const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(kManifestType)))
        {
            if (const HGLOBAL loaded = LoadResource(module, resource))
            {
                const auto* bytes = static_cast<const char*>(LockResource(loaded));
                if (bytes != nullptr)
                    manifest.assign(bytes, SizeofResource(module, resource));
            }
        }
        Check(!manifest.empty(), "CrashManifest_ExeResources: the exe embeds an application manifest");
        Check(manifest.find("Microsoft.Windows.Common-Controls") != std::string::npos &&
                  manifest.find("version=\"6.0.0.0\"") != std::string::npos,
              "CrashManifest_ExeResources: the manifest depends on comctl32 v6");
        Check(manifest.find("PerMonitorV2") != std::string::npos,
              "CrashManifest_ExeResources: the manifest declares PerMonitorV2 DPI awareness");
        Check(FindResourceW(module, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(14)) != nullptr,
              "CrashManifest_ExeResources: the exe embeds the application icon");
        FreeLibrary(module);
    }
#endif

#ifndef _WIN32
    // POSIX reads consent from stdin. The Windows dialogs fail closed when no
    // interactive desktop exists and are covered through the dialog backend seam.
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

        // A manifest (or engine setting) with requireConsent=false must not
        // waive the per-crash confirmation of a public post.
        for (const Case& item : declined)
        {
            const std::string label = "no-review-" + std::string(item.label);
            const auto manifest = MakeConsentManifest(scratch, label, false);
            Check(!manifest.requireConsent, "fixture: manifest disables the local review prompt (" + label + ")");
            const size_t before = CountCalls(capture);
            int result = -1;
            {
                ScopedStdin stdinScript(item.input, item.broken);
                result = SparkCrashReporter::RunCrashReporter(manifest);
            }
            Check(result == 0, "CrashReporter_Consent: requireConsent=false still asks before posting (" + label + ")");
            Check(CountCalls(capture) == before,
                  "CrashReporter_Consent: requireConsent=false without an explicit yes transmits nothing (" + label +
                      ")");
            Check(!AttemptReceiptExists(manifest),
                  "CrashReporter_Consent: requireConsent=false without an explicit yes claims no attempt (" + label +
                      ")");
        }
        {
            const auto manifest = MakeConsentManifest(scratch, "no-review-yes", false);
            const size_t before = CountCalls(capture);
            int result = -1;
            {
                ScopedStdin stdinScript("y\n");
                result = SparkCrashReporter::RunCrashReporter(manifest);
            }
            Check(result == 0 && CountCalls(capture) == before + 1,
                  "CrashReporter_Consent: requireConsent=false plus an explicit yes transmits exactly once");
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
#ifdef _WIN32
    if (argc == 3 && std::string_view(argv[1]) == "--exe-resources")
    {
        TestExecutableResources(argv[2]);
        if (failures != 0 || checks == 0)
        {
            std::cerr << failures << " of " << checks << " executable resource check(s) failed\n";
            return 1;
        }
        std::cout << "CrashReporter executable resource checks passed (" << checks << " checks)\n";
        return 0;
    }
#endif
    ScratchDirectory scratch;
#ifdef _WIN32
    ScopedEnvironment configRoot("LOCALAPPDATA", (scratch.path / "private-config").string());
#else
    ScopedEnvironment configRoot("XDG_CONFIG_HOME", (scratch.path / "private-config").string());
#endif
    TestConsentAnswerParsing();
    TestConsentAnswerIsBoundedBeforeBuffering();
    TestTerminalSafeRendering();
    TestManifestControlsNeverReachTheTerminal(scratch.path);
    TestDialogBackendFailsClosed();
    if (argc == 2)
    {
        TestManifestCannotWaivePublicationQuestion(scratch.path, argv[1]);
        TestFailingDialogBackendTransmitsNothing(scratch.path);
#ifndef _WIN32
        TestUnansweredPromptTransmitsNothing(scratch.path, argv[1]);
#endif
    }
    else
    {
        Check(false, "fake gh executable path must be supplied");
    }

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
