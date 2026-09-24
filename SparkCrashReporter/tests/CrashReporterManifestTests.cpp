#include "CrashReporterApp.h"
#include "CrashAutoIssues.h"
#include "Utils/CrashHandlerSupport.h"

#include <algorithm>
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
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace
{
    namespace fs = std::filesystem;

    class ScratchDirectory
    {
      public:
        ScratchDirectory()
        {
            const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
            // macOS exposes its temp directory through /var -> /private/var.
            // The reporter deliberately rejects redirected config ancestors,
            // so run the fixture under the real directory, not that alias.
            path = fs::canonical(fs::temp_directory_path()) / ("spark-crash-reporter-tests-" + std::to_string(nonce));
            fs::create_directories(path);
        }

        ~ScratchDirectory()
        {
            std::error_code error;
            fs::remove_all(path, error);
        }

        fs::path path;
    };

    class ScopedCurrentPath
    {
      public:
        explicit ScopedCurrentPath(const fs::path& path) : m_previous(fs::current_path()) { fs::current_path(path); }

        ~ScopedCurrentPath()
        {
            std::error_code error;
            fs::current_path(m_previous, error);
        }

      private:
        fs::path m_previous;
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

    int failures = 0;
    int skips = 0;

    void Check(bool condition, std::string_view message)
    {
        if (!condition)
        {
            std::cerr << "FAIL: " << message << '\n';
            ++failures;
        }
    }

    void Skip(std::string_view message, const std::error_code& error)
    {
        std::cerr << "SKIP: " << message << " (fixture unavailable: " << error.message() << ")\n";
        ++skips;
    }

    bool WriteText(const fs::path& path, std::string_view text)
    {
        std::ofstream output(path, std::ios::binary);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        return output.good();
    }

    void TestEngineWriterSpacingAndEscapes(const fs::path& scratch)
    {
        const fs::path path = scratch / "engine_manifest.json";
        Check(WriteText(scratch / "dump.dmp", "dump"), "write engine dump fixture");
        Check(WriteText(scratch / "crash alpha.log", "log"), "write engine log fixture");
        Check(WriteText(scratch / "report.zip", "zip"), "write engine archive fixture");
        const std::string json = R"json({
  "enginePID": "4242",
  "timestamp": "snowman \u2603 rocket \uD83D\uDE80",
  "dumpFile": "dump.dmp",
  "logFile": "crash alpha.log",
  "screenshotFile": "",
  "zipFile": "report.zip",
  "crashTitle": "Access violation\nframe 2",
  "uploadURL": "",
  "proxyURL": "",
  "githubRepo": "Krilliac/SparkEngine",
  "githubToken": "credential-must-not-be-logged",
  "githubLabels": "crash,automated",
  "smtpUser": "robot@example.invalid",
  "smtpPass": "another-secret",
  "emailTo": "triage@example.invalid",
  "emailFrom": "robot@example.invalid",
  "requireConsent": false,
  "allowScreenshotRefusal": true,
  "promptUserDescription": false,
  "timeoutSeconds": 17,
  "futureData": {"array": [1, true, null, {"nested": "accepted"}]}
})json";
        Check(WriteText(path, json), "write engine-format manifest");

        SparkCrashReporter::CrashManifest manifest;
        Check(SparkCrashReporter::LoadManifest(path.string(), manifest),
              "load colon-space engine manifest with escaped values");
        Check(manifest.enginePID == "4242", "engine PID parsed");
        Check(fs::equivalent(fs::path(manifest.logFile), scratch / "crash alpha.log"),
              "artifact path resolves inside manifest directory");
        Check(manifest.crashTitle == "Access violation\nframe 2", "escaped newline parsed");
        Check(manifest.timestamp == "snowman \xE2\x98\x83 rocket \xF0\x9F\x9A\x80", "Unicode escapes parsed");
        Check(!manifest.requireConsent && manifest.allowScreenshotRefusal && !manifest.promptUserDescription,
              "boolean settings parsed");
        Check(manifest.timeoutSeconds == 5 && manifest.uploadURL.empty() && manifest.proxyURL.empty() &&
                  manifest.githubRepo.empty() && manifest.githubToken.empty() && manifest.githubLabels.empty() &&
                  manifest.smtpUser.empty() && manifest.smtpPass.empty() && manifest.emailTo.empty() &&
                  manifest.emailFrom.empty(),
              "legacy transport fields are accepted and securely discarded");
    }

    void TestWriterRoundTrip(const fs::path& scratch)
    {
        const fs::path path = scratch / "round_trip.json";
        SparkCrashReporter::CrashManifest expected;
        expected.enginePID = "7";
        expected.timestamp = "2026-08-26T12:34:56Z";
        expected.dumpFile = (scratch / "round_trip.dmp").string();
        expected.logFile = (scratch / "log with spaces.log").string();
        Check(WriteText(expected.dumpFile, "dump"), "write round-trip dump fixture");
        Check(WriteText(expected.logFile, "log"), "write round-trip log fixture");
        expected.crashTitle =
            std::string("quote \" slash \\ controls ") + '\b' + '\f' + '\r' + '\t' + '\n' + static_cast<char>(1);
        expected.githubToken = "writer-must-not-persist-this-token";
        expected.smtpPass = "writer-must-not-persist-this-password";
        expected.uploadURL = "https://user:secret@example.invalid/upload?signature=secret";
        expected.proxyURL = "https://proxy.invalid/upload?token=secret";
        expected.githubRepo = "owner/repository";
        expected.githubLabels = "crash,private";
        expected.smtpUser = "robot@example.invalid";
        expected.emailTo = "triage@example.invalid";
        expected.emailFrom = "robot@example.invalid";
        expected.requireConsent = false;
        expected.allowScreenshotRefusal = false;
        expected.promptUserDescription = false;
        expected.fullMemoryDump = true;
        expected.timeoutSeconds = 29;

        Check(SparkCrashReporter::WriteManifest(path.string(), expected), "write manifest with JSON escapes");
        std::ifstream written(path, std::ios::binary);
        const std::string serialized((std::istreambuf_iterator<char>(written)), std::istreambuf_iterator<char>());
        Check(serialized.find("uploadURL") == std::string::npos && serialized.find("proxyURL") == std::string::npos &&
                  serialized.find("githubRepo") == std::string::npos &&
                  serialized.find("githubToken") == std::string::npos &&
                  serialized.find("githubLabels") == std::string::npos &&
                  serialized.find("smtpUser") == std::string::npos &&
                  serialized.find("smtpPass") == std::string::npos && serialized.find("emailTo") == std::string::npos &&
                  serialized.find("emailFrom") == std::string::npos &&
                  serialized.find("timeoutSeconds") == std::string::npos &&
                  serialized.find("artifactRoot") == std::string::npos,
              "writer omits transport configuration and trusted local root state");
        SparkCrashReporter::CrashManifest actual;
        Check(SparkCrashReporter::LoadManifest(path.string(), actual), "read writer output");
        Check(actual.enginePID == expected.enginePID && fs::equivalent(actual.logFile, expected.logFile) &&
                  actual.crashTitle == expected.crashTitle && fs::equivalent(actual.artifactRoot, scratch),
              "string fields round-trip");
        Check(actual.requireConsent == expected.requireConsent &&
                  actual.allowScreenshotRefusal == expected.allowScreenshotRefusal &&
                  actual.promptUserDescription == expected.promptUserDescription &&
                  actual.fullMemoryDump == expected.fullMemoryDump && actual.timeoutSeconds == 5,
              "scalar fields round-trip");
        Check(actual.uploadURL.empty() && actual.proxyURL.empty() && actual.githubRepo.empty() &&
                  actual.githubToken.empty() && actual.githubLabels.empty() && actual.smtpUser.empty() &&
                  actual.smtpPass.empty() && actual.emailTo.empty() && actual.emailFrom.empty(),
              "omitted transport fields decode empty");
    }

    void TestMalformedInputRejectedWithoutPartialMutation(const fs::path& scratch)
    {
        const std::vector<std::string> malformed = {R"json({"logFile":"unterminated})json",
                                                    R"json({"logFile":"x")json",
                                                    R"json({"logFile":"x"} trailing)json",
                                                    R"json({"logFile":17})json",
                                                    R"json({"logFile":"first","logFile":"second"})json",
                                                    R"json({"logFile":"bad\qescape"})json",
                                                    std::string("{\"logFile\":\"raw\nnewline\"}"),
                                                    R"json({"logFile":"x","timeoutSeconds":999999999999999999999})json",
                                                    R"json({"logFile":"x","timestamp":"\uD800"})json",
                                                    R"json({"logFile":"x","future":{"truncated":true})json",
                                                    R"json({"enginePID":"1"})json"};

        for (size_t index = 0; index < malformed.size(); ++index)
        {
            const fs::path path = scratch / ("malformed_" + std::to_string(index) + ".json");
            Check(WriteText(path, malformed[index]), "write malformed manifest fixture");
            SparkCrashReporter::CrashManifest manifest;
            manifest.logFile = "unchanged-sentinel";
            Check(!SparkCrashReporter::LoadManifest(path.string(), manifest), "reject malformed manifest");
            Check(manifest.logFile == "unchanged-sentinel", "failed parse leaves output unchanged");
        }

        const fs::path oversized = scratch / "oversized.json";
        const std::string huge(1024 * 1024 + 1, 'x');
        Check(WriteText(oversized, huge), "write oversized manifest fixture");
        SparkCrashReporter::CrashManifest manifest;
        Check(!SparkCrashReporter::LoadManifest(oversized.string(), manifest), "reject manifest over size bound");
    }

    void TestNoUploadPathIsTruthfulAndDoesNotExposeCredentials(const fs::path& scratch)
    {
        const fs::path log = scratch / "crash.log";
        const fs::path staleArchive = scratch / "stale-preconsent.zip";
        Check(WriteText(log, "crash details\n"), "write crash log fixture");
        Check(WriteText(staleArchive, "seeded screenshot bytes"), "write stale archive fixture");

        SparkCrashReporter::CrashManifest manifest;
        manifest.logFile = log.string();
        manifest.zipFile = staleArchive.string();
        manifest.crashTitle = "test crash";
        manifest.requireConsent = false;
        manifest.allowScreenshotRefusal = false;
        manifest.promptUserDescription = false;
        manifest.githubToken = "github-secret-sentinel";
        manifest.smtpPass = "smtp-secret-sentinel";
        const fs::path manifestPath = scratch / "local_only.json";
        Check(SparkCrashReporter::WriteManifest(manifestPath.string(), manifest), "write local-only manifest");
        SparkCrashReporter::CrashManifest loaded;
        Check(SparkCrashReporter::LoadManifest(manifestPath.string(), loaded), "load local-only manifest securely");

        std::ostringstream captured;
        std::streambuf* previous = std::cerr.rdbuf(captured.rdbuf());
        const int result = SparkCrashReporter::RunCrashReporter(loaded);
        std::cerr.rdbuf(previous);

        const std::string output = captured.str();
        Check(result == 0, "local report preparation succeeds");
        Check(output.find("saved locally") != std::string::npos &&
                  output.find("No files were modified") != std::string::npos,
              "no-upload path states actual outcome");
        Check(output.find("Uploading") == std::string::npos && output.find("Thank you") == std::string::npos &&
                  output.find("sent successfully") == std::string::npos,
              "no-upload path does not imply delivery");
        Check(output.find(manifest.githubToken) == std::string::npos &&
                  output.find(manifest.smtpPass) == std::string::npos,
              "console output does not expose manifest credentials");
        std::ifstream stale(staleArchive, std::ios::binary);
        const std::string staleContents((std::istreambuf_iterator<char>(stale)), std::istreambuf_iterator<char>());
        Check(output.find("ignored by this read-only reporter") != std::string::npos &&
                  staleContents == "seeded screenshot bytes",
              "reporter ignores rather than mutates or reuses seeded pre-consent archive");
    }

    void TestConsentArchiveAllowlistAndReporterResolution(const fs::path& scratch)
    {
        Check(!Spark::CrashHandlerDetail::CanPackageScreenshotBeforeConsent(true, true, false),
              "interactive consent defers screenshot packaging");
        Check(Spark::CrashHandlerDetail::CanPackageScreenshotBeforeConsent(true, false, false),
              "explicit no-consent configuration can package screenshot");
        Check(Spark::CrashHandlerDetail::CanPackageScreenshotBeforeConsent(true, true, true),
              "headless auto-consent can package screenshot");

        const auto pending = Spark::CrashHandlerDetail::BuildCrashArchiveAllowlist(
            std::string("crash.dmp"), std::string("crash.log"), std::string("frame.png"), false);
        const auto approved = Spark::CrashHandlerDetail::BuildCrashArchiveAllowlist(
            std::string("crash.dmp"), std::string("crash.log"), std::string("frame.png"), true);
        const std::string seededStaleArchive = "stale-preconsent.zip";
        Check(pending == std::vector<std::string>{"crash.dmp", "crash.log"}, "pre-consent archive excludes screenshot");
        Check(approved == std::vector<std::string>{"crash.dmp", "crash.log", "frame.png"},
              "approved archive includes screenshot exactly once");
        Check(std::find(pending.begin(), pending.end(), seededStaleArchive) == pending.end() &&
                  std::find(pending.begin(), pending.end(), "frame.png") == pending.end(),
              "screenshot refusal policy never reuses a seeded pre-consent archive");

        const fs::path trusted = scratch / "trusted-executable";
        const fs::path hostile = scratch / "hostile-cwd";
        fs::create_directories(trusted);
        fs::create_directories(hostile);
#ifdef _WIN32
        const fs::path reporterName = L"SparkCrashReporter.exe";
#else
        const fs::path reporterName = "SparkCrashReporter";
#endif
        Check(WriteText(hostile / reporterName, "untrusted"), "write hostile reporter fixture");
        {
            ScopedCurrentPath restore(hostile);
            Check(Spark::CrashHandlerDetail::ResolveCrashReporterExecutable(trusted).empty(),
                  "reporter lookup ignores hostile working directory");
            Check(WriteText(trusted / reporterName, "trusted"), "write trusted reporter fixture");
            Check(Spark::CrashHandlerDetail::ResolveCrashReporterExecutable(trusted) ==
                      fs::canonical(trusted / reporterName),
                  "reporter resolves beside canonical executable directory");
        }
    }

    void TestFullMemoryDumpCredentialGate()
    {
        using Spark::CrashHandlerDetail::CanCaptureFullMemoryDump;

        Check(!CanCaptureFullMemoryDump(false, {}, {}, {}, {}), "full-memory capture remains opt-in");
        Check(CanCaptureFullMemoryDump(true, {}, {}, {}, {}), "explicit local-only full-memory capture is permitted");
        Check(!CanCaptureFullMemoryDump(true, "github-pat", {}, {}, {}),
              "GitHub credentials disable full-memory capture");
        Check(!CanCaptureFullMemoryDump(true, {}, "smtp-password", {}, {}),
              "SMTP credentials disable full-memory capture");
        Check(!CanCaptureFullMemoryDump(true, {}, {}, "https://crashes.example.test/upload", {}),
              "upload endpoints disable full-memory capture because their paths may be bearer capabilities");
        Check(!CanCaptureFullMemoryDump(true, {}, {}, {}, "https://relay.example.test/crash"),
              "proxy endpoints disable full-memory capture because their paths may be bearer capabilities");
    }

    void TestArtifactConfinementAndDisclosure(const fs::path& scratch)
    {
        const fs::path reportDirectory = scratch / "confined-report";
        fs::create_directories(reportDirectory);
        const fs::path insideLog = reportDirectory / "inside.log";
        const fs::path outsideArtifact = scratch / "outside.png";
        Check(WriteText(insideLog, "inside log"), "write confined log fixture");
        Check(WriteText(outsideArtifact, "must remain"), "write outside artifact fixture");

        SparkCrashReporter::CrashManifest traversal;
        traversal.logFile = "../outside.png";
        traversal.requireConsent = false;
        const fs::path traversalManifest = reportDirectory / "traversal.json";
        Check(SparkCrashReporter::WriteManifest(traversalManifest.string(), traversal),
              "write traversal manifest fixture");
        SparkCrashReporter::CrashManifest loaded;
        Check(!SparkCrashReporter::LoadManifest(traversalManifest.string(), loaded),
              "loader rejects artifact traversal outside manifest directory");

        SparkCrashReporter::CrashManifest untrusted;
        untrusted.artifactRoot = fs::canonical(reportDirectory).string();
        untrusted.logFile = insideLog.string();
        untrusted.screenshotFile = outsideArtifact.string();
        untrusted.requireConsent = false;
        untrusted.allowScreenshotRefusal = false;
        untrusted.promptUserDescription = false;
        std::ostringstream captured;
        std::streambuf* previous = std::cerr.rdbuf(captured.rdbuf());
        const int result = SparkCrashReporter::RunCrashReporter(untrusted);
        std::cerr.rdbuf(previous);
        Check(result != 0 && fs::exists(outsideArtifact),
              "runner rejects outside screenshot before read, deletion, or overwrite");

        SparkCrashReporter::CrashManifest disclosed;
        disclosed.dumpFile = "crash.dmp";
        disclosed.fullMemoryDump = true;
        disclosed.screenshotFile = "frame.png";
        disclosed.allowScreenshotRefusal = true;
        const std::string disclosure = SparkCrashReporter::BuildConsentMessage(disclosed);
        Check(disclosure.find("full-memory process dump") != std::string::npos &&
                  disclosure.find("application or user data held in memory") != std::string::npos &&
                  disclosure.find("personal or sensitive data") != std::string::npos,
              "consent accurately discloses full-memory and sensitive-data exposure");
        Check(disclosure.find("next dialog") != std::string::npos,
              "consent accurately describes separate screenshot choice");

        disclosed.fullMemoryDump = false;
        const std::string minimalDisclosure = SparkCrashReporter::BuildConsentMessage(disclosed);
        Check(minimalDisclosure.find("minimal process dump") != std::string::npos &&
                  minimalDisclosure.find("full-memory process dump") == std::string::npos,
              "default dump disclosure is minimal rather than full-memory");
    }

    void TestPrivateArtifactDirectoryCreation(const fs::path& scratch)
    {
        const fs::path preplanted = scratch / "preplanted-crash-root";
        fs::create_directories(preplanted);
        Check(!Spark::CrashHandlerDetail::TryCreatePrivateCrashArtifactDirectory(preplanted),
              "private crash root rejects a preplanted directory");

        const fs::path outside = scratch / "outside-root-target";
        fs::create_directories(outside);
        Check(WriteText(outside / "sentinel.txt", "must remain"), "write private-root outside sentinel");
        const fs::path linkedCandidate = scratch / "preplanted-root-link";
        std::error_code linkError;
        fs::create_directory_symlink(outside, linkedCandidate, linkError);
        if (!linkError)
        {
            Check(!Spark::CrashHandlerDetail::TryCreatePrivateCrashArtifactDirectory(linkedCandidate) &&
                      fs::exists(outside / "sentinel.txt"),
                  "private crash root rejects a preplanted directory symlink or junction");
        }
        else
        {
            Skip("private-root directory-link substitution coverage", linkError);
        }

        const fs::path created = Spark::CrashHandlerDetail::CreatePrivateCrashArtifactDirectory(scratch, 424242UL);
        Check(!created.empty() && created.parent_path() == fs::absolute(scratch).lexically_normal() &&
                  fs::is_directory(created),
              "private crash root is randomized and created under the requested base");
#ifndef _WIN32
        const fs::perms permissions = fs::status(created).permissions();
        Check((permissions & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none,
              "private crash root is owner-only on POSIX");
#endif

        const fs::path preexistingManifest = scratch / "preexisting-manifest.json";
        Check(WriteText(preexistingManifest, "sentinel"), "seed preexisting manifest");
        SparkCrashReporter::CrashManifest manifest;
        manifest.logFile = "unused.log";
        Check(!SparkCrashReporter::WriteManifest(preexistingManifest.string(), manifest),
              "manifest writer refuses to overwrite a preplanted file");
        std::ifstream input(preexistingManifest, std::ios::binary);
        const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        Check(contents == "sentinel", "failed exclusive manifest write leaves preplanted file unchanged");
    }

    void TestManifestAndArtifactSubstitutionRejection(const fs::path& scratch)
    {
        const fs::path trusted = scratch / "trusted-watch-root";
        const fs::path outside = scratch / "outside-watch-root";
        fs::create_directories(trusted);
        fs::create_directories(outside);
        Check(WriteText(outside / "outside.log", "outside sentinel"), "write outside manifest log");

        SparkCrashReporter::CrashManifest outsideManifest;
        outsideManifest.logFile = "outside.log";
        outsideManifest.requireConsent = false;
        outsideManifest.promptUserDescription = false;
        const fs::path outsideManifestPath = outside / "outside-manifest.json";
        Check(SparkCrashReporter::WriteManifest(outsideManifestPath.string(), outsideManifest),
              "write outside manifest fixture");

        const fs::path linkedManifest = trusted / "linked-manifest.json";
        std::error_code linkError;
        fs::create_symlink(outsideManifestPath, linkedManifest, linkError);
        if (!linkError)
        {
            SparkCrashReporter::CrashManifest output;
            output.logFile = "unchanged-sentinel";
            Check(!SparkCrashReporter::LoadManifest(linkedManifest.string(), output) &&
                      output.logFile == "unchanged-sentinel",
                  "manifest loader rejects an outside-target final symlink without mutating output");
        }
        else
        {
            Skip("final manifest symlink substitution coverage", linkError);
        }

        const fs::path linkedDirectory = scratch / "linked-watch-directory";
        linkError.clear();
        fs::create_directory_symlink(outside, linkedDirectory, linkError);
        if (!linkError)
        {
            SparkCrashReporter::CrashManifest output;
            output.logFile = "unchanged-sentinel";
            Check(!SparkCrashReporter::LoadManifest((linkedDirectory / "outside-manifest.json").string(), output) &&
                      output.logFile == "unchanged-sentinel",
                  "manifest loader rejects a substituted directory symlink or junction");
        }
        else
        {
            Skip("manifest-directory link substitution coverage", linkError);
        }

        const fs::path hardlinkRoot = scratch / "hardlink-root";
        fs::create_directories(hardlinkRoot);
        const fs::path outsideHardlinkTarget = scratch / "outside-hardlink-sentinel.log";
        Check(WriteText(outsideHardlinkTarget, "outside hardlink sentinel"), "write outside hardlink sentinel");
        std::error_code hardlinkError;
        fs::create_hard_link(outsideHardlinkTarget, hardlinkRoot / "crash.log", hardlinkError);
        if (!hardlinkError)
        {
            SparkCrashReporter::CrashManifest hardlinkManifest;
            hardlinkManifest.logFile = "crash.log";
            const fs::path hardlinkManifestPath = hardlinkRoot / "hardlink-manifest.json";
            Check(SparkCrashReporter::WriteManifest(hardlinkManifestPath.string(), hardlinkManifest),
                  "write hardlink manifest fixture");
            SparkCrashReporter::CrashManifest output;
            Check(!SparkCrashReporter::LoadManifest(hardlinkManifestPath.string(), output),
                  "artifact loader rejects non-single-link files");
        }
        else
        {
            Skip("hardlink artifact substitution coverage", hardlinkError);
        }
    }

    void TestIdentitySwapAndBoundedLogRead(const fs::path& scratch)
    {
        const fs::path swapRoot = scratch / "identity-swap-root";
        fs::create_directories(swapRoot);
        const fs::path log = swapRoot / "crash.log";
        Check(WriteText(log, "original log"), "write identity-swap log");
        SparkCrashReporter::CrashManifest manifest;
        manifest.logFile = "crash.log";
        manifest.requireConsent = false;
        manifest.promptUserDescription = false;
        const fs::path manifestPath = swapRoot / "manifest.json";
        Check(SparkCrashReporter::WriteManifest(manifestPath.string(), manifest), "write identity-swap manifest");
        SparkCrashReporter::CrashManifest loaded;
        Check(SparkCrashReporter::LoadManifest(manifestPath.string(), loaded), "load identity-swap manifest");
        fs::rename(log, swapRoot / "original.log");
        Check(WriteText(log, "replacement log"), "replace log after manifest validation");
        std::ostringstream captured;
        std::streambuf* previous = std::cerr.rdbuf(captured.rdbuf());
        const int swapResult = SparkCrashReporter::RunCrashReporter(loaded);
        std::cerr.rdbuf(previous);
        Check(swapResult != 0 && captured.str().find("file identity changed") != std::string::npos,
              "runner rejects a post-load artifact identity swap before reading it");

        const fs::path rootSwap = scratch / "root-swap";
        fs::create_directories(rootSwap);
        Check(WriteText(rootSwap / "crash.log", "root original"), "write root-swap log");
        SparkCrashReporter::CrashManifest rootManifest;
        rootManifest.logFile = "crash.log";
        rootManifest.requireConsent = false;
        rootManifest.promptUserDescription = false;
        const fs::path rootManifestPath = rootSwap / "manifest.json";
        Check(SparkCrashReporter::WriteManifest(rootManifestPath.string(), rootManifest), "write root-swap manifest");
        SparkCrashReporter::CrashManifest rootLoaded;
        Check(SparkCrashReporter::LoadManifest(rootManifestPath.string(), rootLoaded), "load root-swap manifest");
        fs::rename(rootSwap, scratch / "root-swap-original");
        fs::create_directories(rootSwap);
        Check(WriteText(rootSwap / "crash.log", "replacement root"), "write replacement root log");
        captured.str("");
        captured.clear();
        previous = std::cerr.rdbuf(captured.rdbuf());
        const int rootSwapResult = SparkCrashReporter::RunCrashReporter(rootLoaded);
        std::cerr.rdbuf(previous);
        Check(rootSwapResult != 0, "runner rejects a post-load artifact-root substitution");

        const fs::path largeRoot = scratch / "large-log-root";
        fs::create_directories(largeRoot);
        const fs::path largeLog = largeRoot / "large.log";
        const std::string oversizedLog(8 * 1024 * 1024 + 1, 'L');
        Check(WriteText(largeLog, oversizedLog), "write oversized crash log fixture");
        SparkCrashReporter::CrashManifest largeManifest;
        largeManifest.logFile = "large.log";
        largeManifest.requireConsent = false;
        largeManifest.promptUserDescription = false;
        const fs::path largeManifestPath = largeRoot / "manifest.json";
        Check(SparkCrashReporter::WriteManifest(largeManifestPath.string(), largeManifest),
              "write oversized-log manifest");
        SparkCrashReporter::CrashManifest largeLoaded;
        Check(SparkCrashReporter::LoadManifest(largeManifestPath.string(), largeLoaded),
              "load manifest without reading oversized crash log");
        captured.str("");
        captured.clear();
        previous = std::cerr.rdbuf(captured.rdbuf());
        const int largeResult = SparkCrashReporter::RunCrashReporter(largeLoaded);
        std::cerr.rdbuf(previous);
        Check(largeResult != 0 && captured.str().find("size limit") != std::string::npos,
              "crash log read is bounded and fails closed");
    }

    void TestSequentialNonfatalManifestLifecycle(const fs::path& scratch)
    {
        const fs::path root = scratch / "sequential-manifest-root";
        fs::create_directories(root);
        const fs::path firstManifestPath = root / "crash_manifest_0000000000000001.json";
        const fs::path secondManifestPath = root / "crash_manifest_0000000000000002.json";
        const fs::path invalidManifestPath = root / "crash_manifest_not-a-report.json";

        SparkCrashReporter::CrashManifest first;
        first.logFile = "first.log";
        first.crashTitle = "first nonfatal report";
        first.requireConsent = false;
        first.promptUserDescription = false;
        Check(WriteText(root / first.logFile, "first report"), "write first sequential report log");
        Check(SparkCrashReporter::WriteManifest(firstManifestPath.string(), first), "write first sequential manifest");

        SparkCrashReporter::CrashManifest second;
        second.logFile = "second.log";
        second.crashTitle = "second nonfatal report";
        second.requireConsent = false;
        second.promptUserDescription = false;
        Check(WriteText(root / second.logFile, "second report"), "write second sequential report log");
        Check(SparkCrashReporter::WriteManifest(secondManifestPath.string(), second),
              "write second immediate sequential manifest");
        Check(WriteText(invalidManifestPath, "must remain unclaimed"), "write invalid manifest-name sentinel");

        std::ostringstream captured;
        std::streambuf* previous = std::cerr.rdbuf(captured.rdbuf());
        const int result = SparkCrashReporter::WatchAndReport(root.string(), "invalid-pid");
        std::cerr.rdbuf(previous);

        const std::string output = captured.str();
        Check(result == 0, "invalid PID is rejected without throwing while queued reports are drained");
        Check(output.find(first.crashTitle) != std::string::npos &&
                  output.find("second nonfatal report") != std::string::npos,
              "watcher processes two immediately queued nonfatal manifests");
        Check(!fs::exists(firstManifestPath) && !fs::exists(secondManifestPath),
              "watcher atomically claims and removes each queued manifest");
        Check(fs::exists(invalidManifestPath), "watcher ignores filenames outside the strict manifest shape");
    }

    void TestMalformedReadyManifestFailsClosed(const fs::path& scratch)
    {
        const fs::path root = scratch / "malformed-ready-manifest-root";
        fs::create_directories(root);
        const fs::path malformedManifest = root / "crash_manifest_0000000000000001.json";
        Check(WriteText(malformedManifest, R"json({"logFile":"unterminated})json"),
              "write malformed ready manifest fixture");

        std::ostringstream captured;
        std::streambuf* previous = std::cerr.rdbuf(captured.rdbuf());
        const int result = SparkCrashReporter::WatchAndReport(root.string(), "invalid-pid");
        std::cerr.rdbuf(previous);

        Check(result == 2, "watcher reports a rejected ready manifest as a failure");
    }

    void TestReadOnlyReporterLaunchPolicyAndManifestNames()
    {
        Check(Spark::CrashHandlerDetail::ShouldLaunchReadOnlyReporter(false, false),
              "read-only reporter may launch only for interactive local review");
        Check(!Spark::CrashHandlerDetail::ShouldLaunchReadOnlyReporter(true, false),
              "configured engine upload retains ownership");
        Check(!Spark::CrashHandlerDetail::ShouldLaunchReadOnlyReporter(false, true),
              "headless mode never launches reporter UI");
        Check(Spark::CrashHandlerDetail::CrashManifestReadyName("0000000000000001") ==
                      "crash_manifest_0000000000000001.json" &&
                  Spark::CrashHandlerDetail::IsCrashManifestReadyName("crash_manifest_0000000000000001.json"),
              "manifest helper accepts the strict publication name");
        Check(!Spark::CrashHandlerDetail::IsCrashManifestReadyName("crash_manifest_1.json") &&
                  !Spark::CrashHandlerDetail::IsCrashManifestReadyName("crash_manifest_0000000000000001.tmp") &&
                  Spark::CrashHandlerDetail::CrashManifestReadyName("../00000000000001").empty(),
              "manifest helper rejects short, temporary, and path-like identifiers");
        Check(Spark::CrashHandlerDetail::HasCrashManifestQueueCapacity(31) &&
                  !Spark::CrashHandlerDetail::HasCrashManifestQueueCapacity(32),
              "manifest publication queue has a tested hard bound");
    }

    void TestUtf8CrashArtifactPathConversion()
    {
        const std::string utf8Name = "crash-caf\xC3\xA9";
        Check(Spark::CrashHandlerDetail::WideToUtf8(L"crash-caf\u00e9") == utf8Name,
              "wide crash paths convert into an exactly sized UTF-8 buffer");
        const fs::path converted = Spark::CrashHandlerDetail::PathFromUtf8(utf8Name);
        const std::u8string roundTrip = converted.u8string();
        Check(std::string(reinterpret_cast<const char*>(roundTrip.data()), roundTrip.size()) == utf8Name,
              "UTF-8 crash artifact paths round-trip independently of the Windows locale");
    }

    SparkCrashReporter::CrashManifest MakeAutoIssueManifest(const fs::path& scratch, std::string_view label)
    {
        const fs::path root = scratch / ("auto-issue-" + std::string(label));
        fs::create_directories(root);
        const fs::path log = root / ("GameEngineCrash_20260922_" + std::string(label) + ".log");
        Check(WriteText(log, "private-log-sentinel\n"), "write automatic issue log fixture");
        const fs::path manifestFile = root / "manifest.json";
        const std::string json = "{\"logFile\":\"" + log.filename().string() +
                                 "\",\"crashTitle\":\"private-title-sentinel\","
                                 "\"requireConsent\":false,\"allowScreenshotRefusal\":false,"
                                 "\"promptUserDescription\":false,\"githubRepo\":\"attacker/repo\","
                                 "\"githubToken\":\"private-token-sentinel\"}";
        Check(WriteText(manifestFile, json), "write forged transport manifest fixture");
        SparkCrashReporter::CrashManifest loaded;
        Check(SparkCrashReporter::LoadManifest(manifestFile.string(), loaded), "load auto issue manifest securely");
        return loaded;
    }

    void TestAutomaticIssuesAreOptInBoundedAndIdempotent(const fs::path& scratch, const fs::path& fakeGh)
    {
        const fs::path fakeDirectory = scratch / "fake-gh-bin";
        fs::create_directories(fakeDirectory);
#ifdef _WIN32
        const fs::path fakeExecutable = fakeDirectory / "gh.exe";
#else
        const fs::path fakeExecutable = fakeDirectory / "gh";
#endif
        std::error_code copyError;
        fs::copy_file(fakeGh, fakeExecutable, fs::copy_options::overwrite_existing, copyError);
        Check(!copyError && fs::is_regular_file(fakeExecutable), "install fake gh in isolated PATH");
        if (copyError || !fs::is_regular_file(fakeExecutable))
            return; // Never fall back to the real GitHub CLI.
#ifndef _WIN32
        fs::permissions(fakeExecutable, fs::perms::owner_exec, fs::perm_options::add, copyError);
#endif
        const fs::path capture = scratch / "fake-gh-arguments.txt";
        ScopedEnvironment path("PATH", fakeDirectory.string());
        ScopedEnvironment capturePath("SPARK_FAKE_GH_CAPTURE", capture.string());
        ScopedEnvironment mode("SPARK_FAKE_GH_MODE", "success");
        ScopedEnvironment inheritedHost("GH_HOST", "example.invalid");
#ifdef _WIN32
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        const HANDLE inheritableSentinel = CreateEventW(&security, TRUE, FALSE, nullptr);
        Check(inheritableSentinel != nullptr, "create unrelated inheritable handle for child isolation test");
        const ScopedEnvironment sentinel("SPARK_FAKE_GH_SENTINEL_HANDLE",
                                         std::to_string(reinterpret_cast<std::uintptr_t>(inheritableSentinel)));
#else
        int inheritableSentinel[2] = {-1, -1};
        Check(pipe(inheritableSentinel) == 0, "create unrelated inheritable pipe for child isolation test");
        if (inheritableSentinel[0] >= 0)
            fcntl(inheritableSentinel[0], F_SETFL, O_NONBLOCK);
        const ScopedEnvironment sentinel("SPARK_FAKE_GH_SENTINEL_FD", std::to_string(inheritableSentinel[1]));
#endif
        Check(SparkCrashReporter::SetAutoIssuesEnabled(false), "automatic issues start disabled");
        Check(!SparkCrashReporter::AutoIssuesEnabled(), "no manifest can enable GitHub Issues");

        const auto disabled = MakeAutoIssueManifest(scratch, "disabled");
        Check(SparkCrashReporter::RunCrashReporter(disabled) == 0, "forged manifest remains local without opt-in");
        Check(!fs::exists(capture), "disabled reporter never launches fake gh");

        Check(SparkCrashReporter::SetAutoIssuesEnabled(true), "user explicitly enables automatic issues");
        Check(SparkCrashReporter::AutoIssuesEnabled(), "explicit opt-in persists");
        const std::string publicId = SparkCrashReporter::GeneratePublicIncidentId();
        Check(publicId.size() == 32 && std::all_of(publicId.begin(), publicId.end(), [](char c)
                                                   { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }),
              "public correlation ID comes from bounded random hexadecimal output");
        const auto success = MakeAutoIssueManifest(scratch, "success");
        Check(SparkCrashReporter::RunCrashReporter(success) == 0, "authenticated fake gh confirms issue URL");
        std::ifstream captured(capture, std::ios::binary);
        const std::string sent((std::istreambuf_iterator<char>(captured)), std::istreambuf_iterator<char>());
        Check(sent.find("\ngithub.com/Krilliac/SparkEngine\n") != std::string::npos &&
                  sent.find("\nKrilliac/SparkEngine\n") == std::string::npos &&
                  sent.find("gh-host=example.invalid") != std::string::npos &&
                  sent.find("https://github.com") == std::string::npos,
              "explicit github.com repository overrides a contaminated GH_HOST");
        Check(sent.find("private-log-sentinel") == std::string::npos &&
                  sent.find("private-title-sentinel") == std::string::npos &&
                  sent.find("private-token-sentinel") == std::string::npos &&
                  sent.find("attacker/repo") == std::string::npos && sent.find(scratch.string()) == std::string::npos,
              "submitted arguments contain no untrusted crash content, credentials, or paths");
        Check(sent.find("--end-call--") != std::string::npos, "fake gh received exactly one issue command");
        std::ostringstream statusOutput;
        std::streambuf* previousStatus = std::cout.rdbuf(statusOutput.rdbuf());
        const int statusResult = SparkCrashReporter::ShowAutoIssueStatus(success.artifactRoot);
        std::cout.rdbuf(previousStatus);
        Check(statusResult == 0 &&
                  statusOutput.str().find("confirmed https://github.com/Krilliac/SparkEngine/issues/42") !=
                      std::string::npos,
              "detached watchdog result can be inspected from a bounded local receipt");
#ifdef _WIN32
        Check(inheritableSentinel && WaitForSingleObject(inheritableSentinel, 0) == WAIT_TIMEOUT,
              "GitHub CLI inherits only the explicitly permitted stdio handles");
#else
        char inheritedByte = 0;
        Check(inheritableSentinel[0] >= 0 && read(inheritableSentinel[0], &inheritedByte, 1) < 0 && errno == EAGAIN,
              "GitHub CLI inherits no unrelated POSIX descriptors");
#endif
        const auto firstCallEnd = sent.find("--end-call--");
        Check(SparkCrashReporter::RunCrashReporter(success) == 3, "same incident is never posted twice");
        std::ifstream repeatedCapture(capture, std::ios::binary);
        const std::string afterRepeat((std::istreambuf_iterator<char>(repeatedCapture)),
                                      std::istreambuf_iterator<char>());
        Check(afterRepeat.find("--end-call--", firstCallEnd + 1) == std::string::npos,
              "duplicate run does not call gh again");

        ScopedEnvironment failureMode("SPARK_FAKE_GH_MODE", "failure");
        const auto failure = MakeAutoIssueManifest(scratch, "failure");
        std::ostringstream failureOutput;
        std::streambuf* previousFailureError = std::cerr.rdbuf(failureOutput.rdbuf());
        const int failureResult = SparkCrashReporter::RunCrashReporter(failure);
        std::cerr.rdbuf(previousFailureError);
        Check(failureResult == 3, "gh authentication or network failure is reported");
        Check(failureOutput.str().find("GitHub CLI could not create an issue") != std::string::npos,
              "a failed gh process with no stdout is classified by its exit code");
        Check(fs::exists(failure.logFile), "failed delivery retains local crash log");

        ScopedEnvironment timeoutMode("SPARK_FAKE_GH_MODE", "timeout");
        const auto timeout = MakeAutoIssueManifest(scratch, "timeout");
        Check(SparkCrashReporter::RunCrashReporter(timeout) == 3, "gh timeout is unconfirmed, not success");
        Check(SparkCrashReporter::RunCrashReporter(timeout) == 3, "uncertain timeout is not retried automatically");
        std::ostringstream timeoutStatus;
        std::streambuf* oldTimeoutStatus = std::cout.rdbuf(timeoutStatus.rdbuf());
        const int timeoutStatusResult = SparkCrashReporter::ShowAutoIssueStatus(timeout.artifactRoot);
        std::cout.rdbuf(oldTimeoutStatus);
        Check(timeoutStatusResult == 0 && timeoutStatus.str().find("unconfirmed") != std::string::npos,
              "timeout persists an inspectable unconfirmed outcome");

        ScopedEnvironment unconfirmedMode("SPARK_FAKE_GH_MODE", "unconfirmed");
        const auto unconfirmed = MakeAutoIssueManifest(scratch, "unconfirmed");
        Check(SparkCrashReporter::RunCrashReporter(unconfirmed) == 3, "unexpected issue URL is rejected");

        const auto missingGh = MakeAutoIssueManifest(scratch, "missing-gh");
        {
            ScopedEnvironment missingGhPath("PATH", "");
            std::ostringstream missingGhOutput;
            std::streambuf* previousError = std::cerr.rdbuf(missingGhOutput.rdbuf());
            const int missingGhResult = SparkCrashReporter::RunCrashReporter(missingGh);
            std::cerr.rdbuf(previousError);
            Check(missingGhResult == 3, "missing gh fails without contacting GitHub");
            Check(missingGhOutput.str().find("https://github.com/Krilliac/SparkEngine/issues/new") != std::string::npos,
                  "missing gh gives the playtester a manual issue route");
            Check(missingGhOutput.str().find("Incident ID: ") != std::string::npos,
                  "missing gh displays a safe correlation ID for a manual report");
        }
        const fs::path missingGhReceipt = fs::path(missingGh.artifactRoot) /
                                          ("issue_attempt_" + SparkCrashReporter::CrashReceiptKey(missingGh) + ".txt");
        Check(!fs::exists(missingGhReceipt), "certain missing-gh preflight does not consume one-shot attempt");
        {
            ScopedEnvironment restoredGhMode("SPARK_FAKE_GH_MODE", "success");
            Check(SparkCrashReporter::RunCrashReporter(missingGh) == 0,
                  "installing gh later can submit the same previously unattempted crash");
        }

        const fs::path redirectedTarget = scratch / "redirected-config-target";
        fs::create_directories(redirectedTarget);
        const fs::path redirectedBase = scratch / "redirected-config-base";
        std::error_code redirectError;
        fs::create_directory_symlink(redirectedTarget, redirectedBase, redirectError);
        if (redirectError)
            Skip("config symlink/junction fixture", redirectError);
        else
        {
#ifdef _WIN32
            ScopedEnvironment redirectedConfig("LOCALAPPDATA", redirectedBase.string());
#else
            ScopedEnvironment redirectedConfig("XDG_CONFIG_HOME", redirectedBase.string());
#endif
            Check(!SparkCrashReporter::AutoIssuesEnabled(), "redirected config root cannot enable auto Issues");
            Check(!SparkCrashReporter::SetAutoIssuesEnabled(true), "redirected config ancestor is rejected");
            Check(!fs::exists(redirectedTarget / "SparkEngine" / "CrashReporter" / "auto-issues-v1.enabled"),
                  "rejected redirected config does not write through the link");
        }

        Check(SparkCrashReporter::SetAutoIssuesEnabled(false), "user revokes automatic issue opt-in");
        Check(!SparkCrashReporter::AutoIssuesEnabled(), "revocation takes effect for future reports");
#ifdef _WIN32
        if (inheritableSentinel)
            CloseHandle(inheritableSentinel);
#else
        if (inheritableSentinel[0] >= 0)
            close(inheritableSentinel[0]);
        if (inheritableSentinel[1] >= 0)
            close(inheritableSentinel[1]);
#endif
    }
} // namespace

int main(int argc, char* argv[])
{
    ScratchDirectory scratch;
#ifdef _WIN32
    ScopedEnvironment configRoot("LOCALAPPDATA", (scratch.path / "private-config").string());
#else
    ScopedEnvironment configRoot("XDG_CONFIG_HOME", (scratch.path / "private-config").string());
#endif
    TestEngineWriterSpacingAndEscapes(scratch.path);
    TestWriterRoundTrip(scratch.path);
    TestMalformedInputRejectedWithoutPartialMutation(scratch.path);
    TestNoUploadPathIsTruthfulAndDoesNotExposeCredentials(scratch.path);
    TestConsentArchiveAllowlistAndReporterResolution(scratch.path);
    TestFullMemoryDumpCredentialGate();
    TestArtifactConfinementAndDisclosure(scratch.path);
    TestPrivateArtifactDirectoryCreation(scratch.path);
    TestManifestAndArtifactSubstitutionRejection(scratch.path);
    TestIdentitySwapAndBoundedLogRead(scratch.path);
    TestSequentialNonfatalManifestLifecycle(scratch.path);
    TestMalformedReadyManifestFailsClosed(scratch.path);
    TestReadOnlyReporterLaunchPolicyAndManifestNames();
    TestUtf8CrashArtifactPathConversion();
    if (argc == 2)
        TestAutomaticIssuesAreOptInBoundedAndIdempotent(scratch.path, argv[1]);
    else
        Check(false, "fake gh executable path must be supplied");

    if (failures != 0)
    {
        std::cerr << failures << " CrashReporter manifest test(s) failed\n";
        return 1;
    }
    std::cout << "CrashReporter manifest compatibility tests passed";
    if (skips != 0)
        std::cout << " with " << skips << " explicitly reported fixture skip(s)";
    std::cout << '\n';
    return 0;
}
