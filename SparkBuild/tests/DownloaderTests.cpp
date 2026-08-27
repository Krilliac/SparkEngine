#include "Downloader.h"
#include "DownloadSecurity.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace
{
    uint64_t CurrentProcessId()
    {
#ifdef SPARK_PLATFORM_WINDOWS
        return static_cast<uint64_t>(GetCurrentProcessId());
#else
        return static_cast<uint64_t>(getpid());
#endif
    }

    std::set<std::filesystem::path> CurrentProcessTempDownloads()
    {
        std::set<std::filesystem::path> paths;
        const std::string prefix = "sparkbuild_download_" + std::to_string(CurrentProcessId()) + "_";
        std::error_code error;
        const std::filesystem::path tempDirectory(SparkBuild::Downloader::GetTempDir());
        for (std::filesystem::directory_iterator it(tempDirectory, error), end; !error && it != end;
             it.increment(error))
        {
            const std::string filename = it->path().filename().string();
            if (filename.compare(0, prefix.size(), prefix) == 0 && it->path().extension() == ".zip")
                paths.insert(it->path());
        }
        return paths;
    }

    class ReservedPathCleanup
    {
      public:
        ~ReservedPathCleanup()
        {
            for (const auto& path : paths)
            {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
            }
        }

        std::vector<std::filesystem::path> paths;
    };

    int RunUniqueReservationTest()
    {
        constexpr size_t reservationCount = 32;
        ReservedPathCleanup cleanup;
        cleanup.paths.resize(reservationCount);
        std::vector<std::thread> workers;
        workers.reserve(reservationCount);

        for (size_t index = 0; index < reservationCount; ++index)
        {
            workers.emplace_back([index, &cleanup]
                                 { cleanup.paths[index] = SparkBuild::Downloader::ReserveTempDownloadPath(); });
        }
        for (auto& worker : workers)
            worker.join();

        int failures = 0;
        auto check = [&failures](bool condition, const std::string& message)
        {
            if (!condition)
            {
                ++failures;
                std::cerr << "FAIL: " << message << '\n';
            }
        };

        std::set<std::filesystem::path> uniquePaths;
        for (size_t index = 0; index < cleanup.paths.size(); ++index)
        {
            const auto& path = cleanup.paths[index];
            check(!path.empty(), "reservation " + std::to_string(index) + " returned an empty path");
            if (path.empty())
                continue;

            uniquePaths.insert(path);
            std::error_code error;
            check(std::filesystem::is_regular_file(path, error) && !error,
                  "reserved path is not an existing regular file: " + path.string());
            check(path.extension() == ".zip", "reserved path does not retain the .zip extension: " + path.string());
        }
        check(uniquePaths.size() == reservationCount, "concurrent reservations returned duplicate paths");

        // Each reservation is a real, independent file rather than a merely
        // predicted name. Writing one must not affect any other reservation.
        for (size_t index = 0; index < cleanup.paths.size(); ++index)
        {
            if (cleanup.paths[index].empty())
                continue;
            std::ofstream file(cleanup.paths[index], std::ios::binary | std::ios::trunc);
            file << index;
            check(file.good(), "could not write reserved path " + cleanup.paths[index].string());
        }
        for (size_t index = 0; index < cleanup.paths.size(); ++index)
        {
            if (cleanup.paths[index].empty())
                continue;
            std::ifstream file(cleanup.paths[index], std::ios::binary);
            size_t storedIndex = reservationCount;
            file >> storedIndex;
            check(storedIndex == index, "reserved files were not independent");
        }

        return failures == 0 ? 0 : 1;
    }

    int RunFailedDownloadCleanupTest()
    {
        const auto before = CurrentProcessTempDownloads();
        const std::filesystem::path unusedDestination =
            std::filesystem::path(SparkBuild::Downloader::GetTempDir()) /
            ("sparkbuild_unused_destination_" + std::to_string(CurrentProcessId()));

        // An empty URL is rejected before network I/O on Windows and causes
        // curl to exit with an argument error on Unix. Both paths must remove
        // the atomically reserved archive before returning.
        const bool result = SparkBuild::Downloader::DownloadAndExtract(
            "", unusedDestination.string(), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        const auto after = CurrentProcessTempDownloads();

        std::error_code ignored;
        std::filesystem::remove_all(unusedDestination, ignored);
        if (result)
        {
            std::cerr << "FAIL: an empty download URL unexpectedly succeeded\n";
            return 1;
        }
        if (after != before)
        {
            std::cerr << "FAIL: failed download left a temporary archive behind\n";
            return 1;
        }
        return 0;
    }

    int RunSha256VerificationTest()
    {
        const std::filesystem::path archive = std::filesystem::path(SparkBuild::Downloader::GetTempDir()) /
                                              ("sparkbuild_sha256_" + std::to_string(CurrentProcessId()) + ".zip");
        const std::filesystem::path destination =
            std::filesystem::path(SparkBuild::Downloader::GetTempDir()) /
            ("sparkbuild_sha256_destination_" + std::to_string(CurrentProcessId()));
        std::error_code ignored;
        std::filesystem::remove(archive, ignored);
        std::filesystem::remove_all(destination, ignored);
        {
            std::ofstream output(archive, std::ios::binary | std::ios::trunc);
            output << "abc";
        }

        int failures = 0;
        auto check = [&failures](bool condition, const std::string& message)
        {
            if (!condition)
            {
                ++failures;
                std::cerr << "FAIL: " << message << '\n';
            }
        };
        std::string error;
        check(SparkBuild::DownloadSecurity::VerifySha256(
                  archive, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", error),
              "published SHA-256 known-answer vector was rejected: " + error);
        error.clear();
        check(!SparkBuild::DownloadSecurity::VerifySha256(
                  archive, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", error),
              "mismatched SHA-256 unexpectedly passed verification");
        check(error.find("mismatch") != std::string::npos, "checksum mismatch did not produce a useful diagnostic");
        error.clear();
        check(!SparkBuild::DownloadSecurity::VerifySha256(archive, "not-a-sha256", error),
              "malformed expected SHA-256 unexpectedly passed validation");
        check(!SparkBuild::Downloader::ExtractVerifiedArchive(
                  archive.string(), destination.string(),
                  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
              "mismatched archive unexpectedly reached extraction");
        check(!std::filesystem::exists(destination), "checksum mismatch created an extraction destination");
        check(SparkBuild::DownloadSecurity::RedirectProtocolPolicy("https://example.test/archive.zip") == "=https",
              "HTTPS source did not restrict redirect protocols to HTTPS");
        check(SparkBuild::DownloadSecurity::RedirectProtocolPolicy("HTTPS://example.test/archive.zip") == "=https",
              "HTTPS redirect policy was case-sensitive");
        check(SparkBuild::DownloadSecurity::RedirectProtocolPolicy("http://example.test/archive.zip") == "=http,https",
              "HTTP source redirect policy escaped the HTTP(S) protocol family");

        std::filesystem::remove(archive, ignored);
        std::filesystem::remove_all(destination, ignored);
        return failures == 0 ? 0 : 1;
    }
} // namespace

int main()
{
    const int reservationResult = RunUniqueReservationTest();
    const int cleanupResult = RunFailedDownloadCleanupTest();
    const int sha256Result = RunSha256VerificationTest();
    return reservationResult == 0 && cleanupResult == 0 && sha256Result == 0 ? 0 : 1;
}
