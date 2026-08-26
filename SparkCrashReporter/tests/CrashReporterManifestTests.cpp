#include "CrashReporterApp.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    class ScratchDirectory
    {
      public:
        ScratchDirectory()
        {
            const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
            path = fs::temp_directory_path() / ("spark-crash-reporter-tests-" + std::to_string(nonce));
            fs::create_directories(path);
        }

        ~ScratchDirectory()
        {
            std::error_code error;
            fs::remove_all(path, error);
        }

        fs::path path;
    };

    int failures = 0;

    void Check(bool condition, std::string_view message)
    {
        if (!condition)
        {
            std::cerr << "FAIL: " << message << '\n';
            ++failures;
        }
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
        const std::string json = R"json({
  "enginePID": "4242",
  "timestamp": "snowman \u2603 rocket \uD83D\uDE80",
  "dumpFile": "C:\\crashes\\dump.dmp",
  "logFile": "C:\\crashes\\crash \"alpha\".log",
  "screenshotFile": "",
  "zipFile": "C:\\crashes\\report.zip",
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
        Check(manifest.logFile == "C:\\crashes\\crash \"alpha\".log", "quoted Windows path parsed");
        Check(manifest.crashTitle == "Access violation\nframe 2", "escaped newline parsed");
        Check(manifest.timestamp == "snowman \xE2\x98\x83 rocket \xF0\x9F\x9A\x80", "Unicode escapes parsed");
        Check(!manifest.requireConsent && manifest.allowScreenshotRefusal && !manifest.promptUserDescription,
              "boolean settings parsed");
        Check(manifest.timeoutSeconds == 17, "integer setting parsed");
        Check(manifest.githubToken == "credential-must-not-be-logged" && manifest.smtpPass == "another-secret",
              "credential fields remain data rather than output");
    }

    void TestWriterRoundTrip(const fs::path& scratch)
    {
        const fs::path path = scratch / "round_trip.json";
        SparkCrashReporter::CrashManifest expected;
        expected.enginePID = "7";
        expected.timestamp = "2026-08-26T12:34:56Z";
        expected.dumpFile = "/tmp/dump.core";
        expected.logFile = "/tmp/log with spaces.log";
        expected.crashTitle = std::string("quote \" slash \\ controls ") + '\b' + '\f' + '\r' + '\t' + '\n' +
                              static_cast<char>(1);
        expected.githubToken = "writer-must-not-persist-this-token";
        expected.smtpPass = "writer-must-not-persist-this-password";
        expected.requireConsent = false;
        expected.allowScreenshotRefusal = false;
        expected.promptUserDescription = false;
        expected.timeoutSeconds = 29;

        Check(SparkCrashReporter::WriteManifest(path.string(), expected), "write manifest with JSON escapes");
        std::ifstream written(path, std::ios::binary);
        const std::string serialized((std::istreambuf_iterator<char>(written)), std::istreambuf_iterator<char>());
        Check(serialized.find(expected.githubToken) == std::string::npos &&
                  serialized.find(expected.smtpPass) == std::string::npos &&
                  serialized.find("githubToken") == std::string::npos && serialized.find("smtpPass") == std::string::npos,
              "writer omits reusable credentials");
        SparkCrashReporter::CrashManifest actual;
        Check(SparkCrashReporter::LoadManifest(path.string(), actual), "read writer output");
        Check(actual.enginePID == expected.enginePID && actual.logFile == expected.logFile &&
                  actual.crashTitle == expected.crashTitle,
              "string fields round-trip");
        Check(actual.requireConsent == expected.requireConsent &&
                  actual.allowScreenshotRefusal == expected.allowScreenshotRefusal &&
                  actual.promptUserDescription == expected.promptUserDescription &&
                  actual.timeoutSeconds == expected.timeoutSeconds,
              "scalar fields round-trip");
        Check(actual.githubToken.empty() && actual.smtpPass.empty(), "omitted credential fields decode empty");
    }

    void TestMalformedInputRejectedWithoutPartialMutation(const fs::path& scratch)
    {
        const std::vector<std::string> malformed = {
            R"json({"logFile":"unterminated})json",
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
        Check(WriteText(log, "crash details\n"), "write crash log fixture");

        SparkCrashReporter::CrashManifest manifest;
        manifest.logFile = log.string();
        manifest.crashTitle = "test crash";
        manifest.requireConsent = false;
        manifest.allowScreenshotRefusal = false;
        manifest.promptUserDescription = false;
        manifest.githubToken = "github-secret-sentinel";
        manifest.smtpPass = "smtp-secret-sentinel";

        std::ostringstream captured;
        std::streambuf* previous = std::cerr.rdbuf(captured.rdbuf());
        const int result = SparkCrashReporter::RunCrashReporter(manifest);
        std::cerr.rdbuf(previous);

        const std::string output = captured.str();
        Check(result == 0, "local report preparation succeeds");
        Check(output.find("saved locally") != std::string::npos &&
                  output.find("Automatic upload is not available") != std::string::npos,
              "no-upload path states actual outcome");
        Check(output.find("Uploading") == std::string::npos && output.find("Thank you") == std::string::npos,
              "no-upload path does not imply delivery");
        Check(output.find(manifest.githubToken) == std::string::npos && output.find(manifest.smtpPass) == std::string::npos,
              "console output does not expose manifest credentials");
    }
} // namespace

int main()
{
    ScratchDirectory scratch;
    TestEngineWriterSpacingAndEscapes(scratch.path);
    TestWriterRoundTrip(scratch.path);
    TestMalformedInputRejectedWithoutPartialMutation(scratch.path);
    TestNoUploadPathIsTruthfulAndDoesNotExposeCredentials(scratch.path);

    if (failures != 0)
    {
        std::cerr << failures << " CrashReporter manifest test(s) failed\n";
        return 1;
    }
    std::cout << "CrashReporter manifest compatibility tests passed\n";
    return 0;
}
