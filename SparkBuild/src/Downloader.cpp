#include "Downloader.h"
#include "ArchiveExtraction.h"
#include "DownloadSecurity.h"
#include <fstream>
#include <filesystem>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <system_error>
#include <utility>
#include <vector>

#ifdef SPARK_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

namespace
{
    class ScopedWinHttpHandle
    {
      public:
        explicit ScopedWinHttpHandle(HINTERNET handle = nullptr) : m_handle(handle) {}
        ~ScopedWinHttpHandle()
        {
            if (m_handle)
                WinHttpCloseHandle(m_handle);
        }

        ScopedWinHttpHandle(const ScopedWinHttpHandle&) = delete;
        ScopedWinHttpHandle& operator=(const ScopedWinHttpHandle&) = delete;

        [[nodiscard]] HINTERNET Get() const { return m_handle; }
        [[nodiscard]] explicit operator bool() const { return m_handle != nullptr; }

      private:
        HINTERNET m_handle;
    };
} // namespace

#pragma comment(lib, "winhttp.lib")
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace SparkBuild
{
    namespace
    {
        std::atomic<uint64_t> g_tempDownloadSequence{0};

        uint64_t CurrentProcessId()
        {
#ifdef SPARK_PLATFORM_WINDOWS
            return static_cast<uint64_t>(GetCurrentProcessId());
#else
            return static_cast<uint64_t>(getpid());
#endif
        }

        // Removes a reserved download file or a staging directory tree. remove_all
        // never follows symlinks, so links planted by an archive are unlinked only.
        class ScopedTemporaryPath
        {
          public:
            explicit ScopedTemporaryPath(std::filesystem::path path) : m_path(std::move(path)) {}
            ~ScopedTemporaryPath()
            {
                std::error_code ignored;
                std::filesystem::remove_all(m_path, ignored);
            }

            ScopedTemporaryPath(const ScopedTemporaryPath&) = delete;
            ScopedTemporaryPath& operator=(const ScopedTemporaryPath&) = delete;

          private:
            std::filesystem::path m_path;
        };
    } // namespace

// ============================================================================
// Windows implementation
// ============================================================================
#ifdef SPARK_PLATFORM_WINDOWS

    static bool ParseUrl(const std::string& url, std::wstring& host, std::wstring& path, bool& isHttps)
    {
        URL_COMPONENTS uc = {};
        uc.dwStructSize = sizeof(uc);
        wchar_t hostBuf[256] = {};
        wchar_t pathBuf[2048] = {};
        uc.lpszHostName = hostBuf;
        uc.dwHostNameLength = 256;
        uc.lpszUrlPath = pathBuf;
        uc.dwUrlPathLength = 2048;

        int wlen = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, nullptr, 0);
        std::wstring wurl(wlen, 0);
        MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, wurl.data(), wlen);

        if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc))
            return false;
        host = hostBuf;
        path = pathBuf;
        isHttps = (uc.nScheme == INTERNET_SCHEME_HTTPS);
        return true;
    }

    bool Downloader::DownloadFile(const std::string& url, const std::string& outputPath,
                                  DownloadProgressCallback progress)
    {
        std::wstring host, path;
        bool isHttps = true;
        if (!ParseUrl(url, host, path, isHttps))
            return false;

        ScopedWinHttpHandle hSession(WinHttpOpen(L"SparkBuild/2.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (!hSession)
            return false;

        INTERNET_PORT port = isHttps ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
        ScopedWinHttpHandle hConnect(WinHttpConnect(hSession.Get(), host.c_str(), port, 0));
        if (!hConnect)
            return false;

        DWORD flags = isHttps ? WINHTTP_FLAG_SECURE : 0;
        ScopedWinHttpHandle hRequest(WinHttpOpenRequest(hConnect.Get(), L"GET", path.c_str(), nullptr,
                                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
        if (!hRequest)
            return false;

        // Make the secure redirect contract explicit instead of depending on
        // WinHTTP's default policy. Automatic handling also keeps each handle
        // single-owner, eliminating the old redirect failure double-close path.
        DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
        if (!WinHttpSetOption(hRequest.Get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy)))
            return false;
        DWORD maxRedirects = 5;
        if (!WinHttpSetOption(hRequest.Get(), WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, &maxRedirects,
                              sizeof(maxRedirects)))
            return false;

        if (!WinHttpSendRequest(hRequest.Get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
            return false;
        if (!WinHttpReceiveResponse(hRequest.Get(), nullptr))
            return false;

        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        if (!WinHttpQueryHeaders(hRequest.Get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX) ||
            statusCode != 200)
            return false;

        size_t totalBytes = 0;
        wchar_t contentLength[32] = {};
        DWORD contentLengthSize = sizeof(contentLength);
        if (WinHttpQueryHeaders(hRequest.Get(), WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                                contentLength, &contentLengthSize, WINHTTP_NO_HEADER_INDEX))
            totalBytes = static_cast<size_t>(_wtoi64(contentLength));

        std::filesystem::path outPath(outputPath);
        if (outPath.has_parent_path())
            std::filesystem::create_directories(outPath.parent_path());

        std::ofstream outFile(outputPath, std::ios::binary | std::ios::trunc);
        if (!outFile.is_open())
            return false;

        size_t bytesDownloaded = 0;
        while (true)
        {
            DWORD bytesAvail = 0;
            if (!WinHttpQueryDataAvailable(hRequest.Get(), &bytesAvail))
                return false;
            if (bytesAvail == 0)
                break;

            std::vector<char> buffer(bytesAvail);
            DWORD bytesRead = 0;
            if (!WinHttpReadData(hRequest.Get(), buffer.data(), bytesAvail, &bytesRead) || bytesRead == 0)
                return false;
            outFile.write(buffer.data(), static_cast<std::streamsize>(bytesRead));
            if (!outFile)
                return false;
            bytesDownloaded += bytesRead;
            if (progress)
                progress(bytesDownloaded, totalBytes);
        }
        return true;
    }

    // Extract with the in-box bsdtar (System32\tar.exe, Windows 10 1803+),
    // which reads ZIP natively, runs to completion before we look at the
    // staging tree, and reports every failure through its exit code. The Shell
    // Folder::CopyHere path it replaces could return while its copy was still
    // running and hid per-item errors behind FOF_NOERRORUI.
    static bool ExtractZipIntoStaging(const std::filesystem::path& zipPath, const std::filesystem::path& stagingDir)
    {
        wchar_t systemDirectory[MAX_PATH] = {};
        const UINT systemLength = GetSystemDirectoryW(systemDirectory, MAX_PATH);
        if (systemLength == 0 || systemLength >= MAX_PATH)
            return false;
        // The absolute System32 path keeps PATH and the working directory out of the lookup.
        const std::wstring tarExe = std::wstring(systemDirectory, systemLength) + L"\\tar.exe";

        const std::wstring archive = zipPath.wstring();
        const std::wstring staging = stagingDir.wstring();
        // Windows paths cannot contain '"'; a trailing backslash would escape the closing quote.
        const auto quotable = [](const std::wstring& path)
        { return !path.empty() && path.find(L'"') == std::wstring::npos && path.back() != L'\\'; };
        if (!quotable(archive) || !quotable(staging))
            return false;

        std::wstring commandLine = L"\"" + tarExe + L"\" -x -f \"" + archive + L"\" -C \"" + staging + L"\"";
        STARTUPINFOW startup = {};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process = {};
        if (!CreateProcessW(tarExe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                            nullptr, &startup, &process))
            return false;
        CloseHandle(process.hThread);

        DWORD exitCode = 1;
        const bool finished = WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0 &&
                              GetExitCodeProcess(process.hProcess, &exitCode) != 0;
        CloseHandle(process.hProcess);
        return finished && exitCode == 0;
    }

    std::string Downloader::GetTempDir()
    {
        char buf[MAX_PATH] = {};
        GetTempPathA(MAX_PATH, buf);
        std::string path(buf);
        if (!path.empty() && path.back() == '\\')
            path.pop_back();
        return path;
    }

// ============================================================================
// Unix implementation (Linux / macOS) - uses system curl and unzip
// ============================================================================
#else

    namespace
    {
        // Upper bound for captured tool output (a tar member listing).
        constexpr size_t kMaxCapturedOutputBytes = 64u * 1024 * 1024;

        // Run a tool without a shell. stdin is /dev/null so an extractor can
        // never block on an interactive prompt; stdout is captured on request.
        bool RunProcess(const std::string& executable, const std::vector<std::string>& args,
                        std::string* capturedOutput = nullptr)
        {
            std::vector<char*> argv;
            argv.reserve(args.size() + 2);
            argv.push_back(const_cast<char*>(executable.c_str()));
            for (const auto& arg : args)
                argv.push_back(const_cast<char*>(arg.c_str()));
            argv.push_back(nullptr);

            int outputPipe[2] = {-1, -1};
            if (capturedOutput && pipe(outputPipe) != 0)
                return false;

            pid_t pid = fork();
            if (pid < 0)
            {
                if (capturedOutput)
                {
                    close(outputPipe[0]);
                    close(outputPipe[1]);
                }
                return false;
            }

            if (pid == 0)
            {
                const int devNull = open("/dev/null", O_RDONLY);
                if (devNull >= 0)
                {
                    dup2(devNull, STDIN_FILENO);
                    close(devNull);
                }
                if (capturedOutput)
                {
                    dup2(outputPipe[1], STDOUT_FILENO);
                    close(outputPipe[0]);
                    close(outputPipe[1]);
                }
                execvp(executable.c_str(), argv.data());
                _exit(127);
            }

            bool outputComplete = true;
            if (capturedOutput)
            {
                close(outputPipe[1]);
                capturedOutput->clear();
                std::array<char, 4096> buffer{};
                while (true)
                {
                    const ssize_t bytesRead = read(outputPipe[0], buffer.data(), buffer.size());
                    if (bytesRead < 0 && errno == EINTR)
                        continue;
                    if (bytesRead <= 0)
                    {
                        outputComplete = outputComplete && bytesRead == 0;
                        break;
                    }
                    // Keep draining past the cap so the child cannot block on a full pipe.
                    if (capturedOutput->size() + static_cast<size_t>(bytesRead) > kMaxCapturedOutputBytes)
                        outputComplete = false;
                    else
                        capturedOutput->append(buffer.data(), static_cast<size_t>(bytesRead));
                }
                close(outputPipe[0]);
            }

            int status = 0;
            while (waitpid(pid, &status, 0) < 0)
            {
                if (errno != EINTR)
                    return false;
            }
            return outputComplete && WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }

        // unzip -n never overwrites; staging is fresh, so this only matters for
        // duplicate member names, which must not replace an earlier member.
        bool ExtractZipIntoStaging(const std::string& zipPath, const std::string& stagingDir)
        {
            return RunProcess("unzip", {"-q", "-n", zipPath, "-d", stagingDir});
        }

        bool ListTarMembers(const std::string& archivePath, std::vector<std::string>& names, std::string& error)
        {
            std::string listing;
            if (!RunProcess("tar", {"-tzf", archivePath}, &listing))
            {
                error = "tar could not list the archive";
                return false;
            }
            names.clear();
            size_t start = 0;
            while (start < listing.size())
            {
                size_t end = listing.find('\n', start);
                if (end == std::string::npos)
                    end = listing.size();
                names.emplace_back(listing, start, end - start);
                start = end + 1;
            }
            return true;
        }

        bool ExtractTarIntoStaging(const std::string& archivePath, const std::string& stagingDir)
        {
            return RunProcess("tar", {"--no-same-owner", "-xzf", archivePath, "-C", stagingDir});
        }
    } // namespace

    bool Downloader::DownloadFile(const std::string& url, const std::string& outputPath,
                                  DownloadProgressCallback /*progress*/)
    {
        // Create parent directories
        auto parent = std::filesystem::path(outputPath).parent_path();
        if (!parent.empty())
        {
            std::filesystem::create_directories(parent);
        }

        return RunProcess("curl",
                          {"-fSL", "--progress-bar", "--max-redirs", "5", "--proto", "=http,https", "--proto-redir",
                           DownloadSecurity::RedirectProtocolPolicy(url), "-o", outputPath, url});
    }

    std::string Downloader::GetTempDir()
    {
        const char* tmpdir = std::getenv("TMPDIR");
        if (tmpdir && tmpdir[0] != '\0')
            return tmpdir;
        return "/tmp";
    }

#endif

    // Common implementation
    namespace
    {
        const char* ArchiveFormatName(ArchiveFormat format)
        {
            switch (format)
            {
            case ArchiveFormat::Zip:
                return "ZIP";
            case ArchiveFormat::GzipTar:
                return "gzip-compressed tar";
            case ArchiveFormat::Unknown:
                break;
            }
            return "unrecognised";
        }

        bool RejectArchive(const std::string& archivePath, const std::string& reason)
        {
            std::cerr << "SparkBuild: refusing archive '" << archivePath << "': " << reason << '\n';
            return false;
        }
    } // namespace

    std::string Downloader::ReserveTempDownloadPath(const std::string& archiveSuffix)
    {
        if (archiveSuffix != ".zip" && archiveSuffix != ".tar.gz")
            return {};
        const std::filesystem::path tempDirectory(GetTempDir());
        if (tempDirectory.empty())
            return {};

        std::error_code directoryError;
        std::filesystem::create_directories(tempDirectory, directoryError);
        if (directoryError)
            return {};

        const uint64_t processId = CurrentProcessId();
        const uint64_t tick = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());

        for (size_t attempt = 0; attempt < 128; ++attempt)
        {
            const uint64_t sequence = g_tempDownloadSequence.fetch_add(1, std::memory_order_relaxed);
            const std::filesystem::path candidate =
                tempDirectory / ("sparkbuild_download_" + std::to_string(processId) + "_" + std::to_string(tick) + "_" +
                                 std::to_string(sequence) + archiveSuffix);

#ifdef SPARK_PLATFORM_WINDOWS
            HANDLE file = CreateFileA(candidate.string().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                                      nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
            if (file != INVALID_HANDLE_VALUE)
            {
                CloseHandle(file);
                return candidate.string();
            }
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
                return {};
#else
            int flags = O_CREAT | O_EXCL | O_WRONLY;
#ifdef O_CLOEXEC
            flags |= O_CLOEXEC;
#endif
            const int file = open(candidate.c_str(), flags, S_IRUSR | S_IWUSR);
            if (file >= 0)
            {
                close(file);
                return candidate.string();
            }
            if (errno != EEXIST)
                return {};
#endif
        }

        return {};
    }

    bool Downloader::ExtractVerifiedArchive(const std::string& archivePath, const std::string& destDir,
                                            const std::string& expectedSha256, ArchiveFormat expectedFormat)
    {
        std::string error;
        if (!DownloadSecurity::VerifySha256(archivePath, expectedSha256, error))
            return RejectArchive(archivePath, error);
        if (expectedFormat == ArchiveFormat::Unknown)
            return RejectArchive(archivePath, "no expected archive format was given");

        // The container is chosen by content; a name or URL only states the expectation.
        const ArchiveFormat detected = DetectArchiveFormat(archivePath);
        if (detected != expectedFormat)
        {
            return RejectArchive(archivePath, std::string("content is ") + ArchiveFormatName(detected) + ", expected " +
                                                  ArchiveFormatName(expectedFormat));
        }

        std::vector<std::string> members;
        if (detected == ArchiveFormat::Zip)
        {
            if (!ArchiveExtraction::ListZipMembers(archivePath, members, error) ||
                !ArchiveExtraction::ValidateMemberNames(members, ArchiveExtraction::MemberSyntax::Zip, error))
                return RejectArchive(archivePath, error);
        }
        else
        {
#ifdef SPARK_PLATFORM_WINDOWS
            return RejectArchive(archivePath, "gzip-compressed tar extraction is not supported on Windows");
#else
            if (!ListTarMembers(archivePath, members, error) ||
                !ArchiveExtraction::ValidateMemberNames(members, ArchiveExtraction::MemberSyntax::Tar, error))
                return RejectArchive(archivePath, error);
#endif
        }

        // A destination created only for this extraction is removed again if it fails.
        std::error_code statusError;
        const bool destinationExisted =
            std::filesystem::symlink_status(destDir, statusError).type() != std::filesystem::file_type::not_found;

        bool committed = false;
        std::filesystem::path staging;
        if (ArchiveExtraction::CreateStagingDirectory(destDir, staging, error))
        {
            const ScopedTemporaryPath stagingCleanup(staging);
#ifdef SPARK_PLATFORM_WINDOWS
            const bool extracted = ExtractZipIntoStaging(std::filesystem::path(archivePath), staging);
#else
            const bool extracted = detected == ArchiveFormat::Zip
                                       ? ExtractZipIntoStaging(archivePath, staging.string())
                                       : ExtractTarIntoStaging(archivePath, staging.string());
#endif
            // tar's own exit status covers completeness; the ZIP listing is
            // parsed in-process, so the staged set can be compared against it.
            if (!extracted)
                error = "the extractor reported a failure";
            else if ((detected != ArchiveFormat::Zip ||
                      ArchiveExtraction::VerifyStagedMembers(staging, members, error)) &&
                     ArchiveExtraction::ValidateStagedTree(staging, error))
                committed = ArchiveExtraction::CommitStagedTree(staging, destDir, error);
        }
        if (committed)
            return true;

        if (!destinationExisted)
        {
            std::error_code ignored;
            std::filesystem::remove(destDir, ignored); // only succeeds while it is still empty
        }
        return RejectArchive(archivePath, error);
    }

    bool Downloader::DownloadAndExtract(const std::string& url, const std::string& destDir,
                                        const std::string& expectedSha256, DownloadProgressCallback progress)
    {
        const ArchiveFormat expectedFormat = ArchiveFormatFromName(url);
        if (expectedFormat == ArchiveFormat::Unknown)
            return false;
        const std::string tempPath = ReserveTempDownloadPath(expectedFormat == ArchiveFormat::Zip ? ".zip" : ".tar.gz");
        if (tempPath.empty())
            return false;
        const ScopedTemporaryPath cleanup(tempPath);

        if (!DownloadFile(url, tempPath, progress))
        {
            return false;
        }

        return ExtractVerifiedArchive(tempPath, destDir, expectedSha256, expectedFormat);
    }

} // namespace SparkBuild
