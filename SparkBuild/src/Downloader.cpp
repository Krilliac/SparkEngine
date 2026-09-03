#include "Downloader.h"
#include "DownloadSecurity.h"
#include <fstream>
#include <filesystem>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <system_error>
#include <utility>
#include <vector>

#ifdef SPARK_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#include <ole2.h>
#include <oleauto.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shldisp.h>

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
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")
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

        class ScopedTemporaryDownload
        {
          public:
            explicit ScopedTemporaryDownload(std::string path) : m_path(std::move(path)) {}
            ~ScopedTemporaryDownload()
            {
                std::error_code ignored;
                std::filesystem::remove(m_path, ignored);
            }

            ScopedTemporaryDownload(const ScopedTemporaryDownload&) = delete;
            ScopedTemporaryDownload& operator=(const ScopedTemporaryDownload&) = delete;

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

    bool Downloader::ExtractZip(const std::string& zipPath, const std::string& destDir)
    {
        const HRESULT comInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(comInit) && comInit != RPC_E_CHANGED_MODE)
            return false;

        int zipWLen = MultiByteToWideChar(CP_UTF8, 0, zipPath.c_str(), -1, nullptr, 0);
        std::wstring zipW(zipWLen, 0);
        MultiByteToWideChar(CP_UTF8, 0, zipPath.c_str(), -1, zipW.data(), zipWLen);

        int destWLen = MultiByteToWideChar(CP_UTF8, 0, destDir.c_str(), -1, nullptr, 0);
        std::wstring destW(destWLen, 0);
        MultiByteToWideChar(CP_UTF8, 0, destDir.c_str(), -1, destW.data(), destWLen);

        std::filesystem::create_directories(destDir);

        IShellDispatch* pShell = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_Shell, nullptr, CLSCTX_INPROC_SERVER, IID_IShellDispatch, (void**)&pShell);
        if (FAILED(hr) || !pShell)
            return false;

        VARIANT vZip, vDest;
        VariantInit(&vZip);
        VariantInit(&vDest);
        vZip.vt = VT_BSTR;
        vZip.bstrVal = SysAllocString(zipW.c_str());
        vDest.vt = VT_BSTR;
        vDest.bstrVal = SysAllocString(destW.c_str());

        Folder* pZipFolder = nullptr;
        hr = pShell->NameSpace(vZip, &pZipFolder);
        if (FAILED(hr) || !pZipFolder)
        {
            VariantClear(&vZip);
            VariantClear(&vDest);
            pShell->Release();
            return false;
        }

        Folder* pDestFolder = nullptr;
        hr = pShell->NameSpace(vDest, &pDestFolder);
        if (FAILED(hr) || !pDestFolder)
        {
            pZipFolder->Release();
            VariantClear(&vZip);
            VariantClear(&vDest);
            pShell->Release();
            return false;
        }

        FolderItems* pItems = nullptr;
        pZipFolder->Items(&pItems);
        if (!pItems)
        {
            pDestFolder->Release();
            pZipFolder->Release();
            VariantClear(&vZip);
            VariantClear(&vDest);
            pShell->Release();
            return false;
        }

        VARIANT vItems;
        VariantInit(&vItems);
        vItems.vt = VT_DISPATCH;
        vItems.pdispVal = pItems;

        VARIANT vOptions;
        VariantInit(&vOptions);
        vOptions.vt = VT_I4;
        vOptions.lVal = 0x0614; // FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT

        hr = pDestFolder->CopyHere(vItems, vOptions);

        pItems->Release();
        pDestFolder->Release();
        pZipFolder->Release();
        VariantClear(&vZip);
        VariantClear(&vDest);
        pShell->Release();

        return SUCCEEDED(hr);
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
        bool RunProcess(const std::string& executable, const std::vector<std::string>& args)
        {
            std::vector<char*> argv;
            argv.reserve(args.size() + 2);
            argv.push_back(const_cast<char*>(executable.c_str()));
            for (const auto& arg : args)
                argv.push_back(const_cast<char*>(arg.c_str()));
            argv.push_back(nullptr);

            pid_t pid = fork();
            if (pid < 0)
                return false;

            if (pid == 0)
            {
                execvp(executable.c_str(), argv.data());
                _exit(127);
            }

            int status = 0;
            if (waitpid(pid, &status, 0) < 0)
                return false;
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
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

    bool Downloader::ExtractZip(const std::string& zipPath, const std::string& destDir)
    {
        std::filesystem::create_directories(destDir);

        // Try unzip first, then python's zipfile module as fallback.
        if (RunProcess("unzip", {"-o", "-q", zipPath, "-d", destDir}))
            return true;

        // Fallback: use tar if it's a .tar.gz
        if (zipPath.find(".tar.gz") != std::string::npos || zipPath.find(".tgz") != std::string::npos)
        {
            return RunProcess("tar", {"xzf", zipPath, "-C", destDir});
        }

        // Fallback: python3
        return RunProcess("python3", {"-c", "import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])",
                                      zipPath, destDir});
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
                                            const std::string& expectedSha256)
    {
        std::string verificationError;
        if (!DownloadSecurity::VerifySha256(archivePath, expectedSha256, verificationError))
            return false;
        return ExtractZip(archivePath, destDir);
    }

    bool Downloader::DownloadAndExtract(const std::string& url, const std::string& destDir,
                                        const std::string& expectedSha256, DownloadProgressCallback progress)
    {
        const std::string archiveSuffix =
            (url.find(".tar.gz") != std::string::npos || url.find(".tgz") != std::string::npos) ? ".tar.gz" : ".zip";
        const std::string tempPath = ReserveTempDownloadPath(archiveSuffix);
        if (tempPath.empty())
            return false;
        const ScopedTemporaryDownload cleanup(tempPath);

        if (!DownloadFile(url, tempPath, progress))
        {
            return false;
        }

        return ExtractVerifiedArchive(tempPath, destDir, expectedSha256);
    }

} // namespace SparkBuild
