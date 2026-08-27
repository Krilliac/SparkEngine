#include "Utils/CrashHandler.h"
#include "../Core/Platform.h"
#include "../Core/RuntimePackage.h"
#include "Utils/CrashHandlerSupport.h"
#include "Utils/CrashReportUploader.h"
#include "Utils/Assert.h"
#include "Utils/Process.h"
#include "Utils/SparkError.h"
#include "Utils/ConsoleProcessManager.h"
#include "Validate.h"

// Only include CURL when libcurl is available (detected by CMake)
#ifdef SPARK_HAS_CURL
#include <curl/curl.h>
#endif

#include <miniz.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <mutex>
#include <iostream>
#include <limits>
#include <ctime>
#include <string>
#include <cstring>

#ifdef SPARK_PLATFORM_WINDOWS
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <winternl.h>
#include <dbghelp.h>
#include <dxgi.h>
#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <versionhelpers.h>
#include <tlhelp32.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dxgi.lib")
#elif defined(SPARK_PLATFORM_LINUX) || defined(SPARK_PLATFORM_MACOS)
#include <dirent.h>
#include <signal.h>
#include <unistd.h>
#include <execinfo.h>
#include <cxxabi.h>
#include <sys/utsname.h>
#include <sys/resource.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <fstream>
#include <climits>
#include <cstdlib>
#ifdef SPARK_PLATFORM_LINUX
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#endif
#ifdef SPARK_PLATFORM_MACOS
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <pthread.h>
#endif
#endif

static CrashConfig g_cfg;
static std::mutex g_lock;
static bool g_triggerCrashOnAssert = false;
static std::atomic<std::uint64_t> g_reportSequence{0};
#if defined(SPARK_PLATFORM_LINUX) || defined(SPARK_PLATFORM_MACOS)
static volatile sig_atomic_t g_inSignalHandler = 0;
#endif

static void AssignCrashConfig(const CrashConfig& cfg)
{
    g_cfg = cfg;
    if (g_cfg.captureFullMemoryDump && !Spark::CrashHandlerDetail::CanCaptureFullMemoryDump(
                                           true, g_cfg.githubToken, g_cfg.smtpPass, g_cfg.uploadURL, g_cfg.proxyURL))
    {
        g_cfg.captureFullMemoryDump = false;
        SPARK_LOG_WARN(Spark::LogCategory::Core,
                       "CrashHandler: full-memory dump disabled because automatic transport configuration is present");
    }
}


// ============================================================================
// Cross-platform helpers
// ============================================================================

static std::string WideToUtf8(const std::wstring& w)
{
    return Spark::CrashHandlerDetail::WideToUtf8(w);
}

static std::string MakeCrashReportId()
{
    const std::uint64_t sequence = g_reportSequence.fetch_add(1, std::memory_order_relaxed) + 1;
    char buffer[17]{};
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(sequence));
    return buffer;
}

static std::string MakeTimeStampUtf8()
{
    time_t now = time(nullptr);
    struct tm t;
#ifdef SPARK_PLATFORM_WINDOWS
    localtime_s(&t, &now);
#else
    localtime_r(&now, &t);
#endif
    char buf[64];
    snprintf(buf, sizeof(buf), "_%04d%02d%02d_%02d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
             t.tm_min, t.tm_sec);
    return buf;
}

static std::filesystem::path g_artifactRootPath;
#ifdef SPARK_PLATFORM_WINDOWS
static HANDLE g_artifactRootHandle = INVALID_HANDLE_VALUE;
#else
static int g_artifactRootHandle = -1;
#endif

static void ResetPinnedArtifactRoot()
{
#ifdef SPARK_PLATFORM_WINDOWS
    if (g_artifactRootHandle != INVALID_HANDLE_VALUE)
        CloseHandle(g_artifactRootHandle);
    g_artifactRootHandle = INVALID_HANDLE_VALUE;
#else
    if (g_artifactRootHandle >= 0)
        close(g_artifactRootHandle);
    g_artifactRootHandle = -1;
#endif
    g_artifactRootPath.clear();
}

static bool PinArtifactRoot(const std::filesystem::path& root)
{
    ResetPinnedArtifactRoot();
    const std::filesystem::path absoluteRoot = root.lexically_normal();
    if (absoluteRoot.empty() || !absoluteRoot.is_absolute())
        return false;

#ifdef SPARK_PLATFORM_WINDOWS
    HANDLE handle =
        CreateFileW(absoluteRoot.c_str(), FILE_READ_ATTRIBUTES | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    BY_HANDLE_FILE_INFORMATION info{};
    if (handle == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
        return false;
    }
#else
    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const int handle = open(absoluteRoot.c_str(), flags);
    struct stat info{};
    if (handle < 0 || fstat(handle, &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != geteuid() ||
        (info.st_mode & (S_IRWXG | S_IRWXO)) != 0)
    {
        if (handle >= 0)
            close(handle);
        return false;
    }
#endif

    g_artifactRootHandle = handle;
    g_artifactRootPath = absoluteRoot;
    return true;
}

static bool ArtifactNameInPinnedRoot(const std::string& path, std::filesystem::path& name)
{
#ifdef SPARK_PLATFORM_WINDOWS
    const std::filesystem::path candidate = std::filesystem::u8path(path.begin(), path.end()).lexically_normal();
    if (g_artifactRootHandle == INVALID_HANDLE_VALUE)
        return false;
#else
    const std::filesystem::path candidate = std::filesystem::path(path).lexically_normal();
    if (g_artifactRootHandle < 0)
        return false;
#endif
    if (!candidate.is_absolute() || candidate.parent_path() != g_artifactRootPath)
        return false;
    name = candidate.filename();
    return !name.empty() && !name.has_parent_path() && name != "." && name != "..";
}

#ifdef SPARK_PLATFORM_WINDOWS
static HANDLE OpenArtifactRelativeToPinnedRoot(const std::filesystem::path& name, ACCESS_MASK desiredAccess,
                                               ULONG createDisposition)
{
    if (g_artifactRootHandle == INVALID_HANDLE_VALUE || name.empty() || name.has_parent_path())
        return INVALID_HANDLE_VALUE;

    using NtCreateFileFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER,
                                            ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
    static const auto ntCreateFile = reinterpret_cast<NtCreateFileFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile")));
    if (!ntCreateFile)
        return INVALID_HANDLE_VALUE;

    const std::wstring nativeName = name.native();
    if (nativeName.empty() ||
        nativeName.size() > (static_cast<size_t>((std::numeric_limits<USHORT>::max)()) / sizeof(wchar_t)))
        return INVALID_HANDLE_VALUE;

    UNICODE_STRING objectName{};
    objectName.Buffer = const_cast<PWSTR>(nativeName.c_str());
    objectName.Length = static_cast<USHORT>(nativeName.size() * sizeof(wchar_t));
    objectName.MaximumLength = objectName.Length;
    OBJECT_ATTRIBUTES attributes{};
    InitializeObjectAttributes(&attributes, &objectName, OBJ_CASE_INSENSITIVE, g_artifactRootHandle, nullptr);
    IO_STATUS_BLOCK ioStatus{};
    HANDLE handle = INVALID_HANDLE_VALUE;
    const NTSTATUS status =
        ntCreateFile(&handle, desiredAccess | SYNCHRONIZE, &attributes, &ioStatus, nullptr, FILE_ATTRIBUTE_NORMAL,
                     FILE_SHARE_READ, createDisposition,
                     FILE_NON_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT, nullptr, 0);
    return status >= 0 ? handle : INVALID_HANDLE_VALUE;
}
#endif

struct PinnedFile
{
    std::FILE* stream = nullptr;
    std::uint64_t size = 0;
    std::uint64_t device = 0;
    std::uint64_t file = 0;
    bool identityValid = false;

    PinnedFile() = default;
    PinnedFile(const PinnedFile&) = delete;
    PinnedFile& operator=(const PinnedFile&) = delete;
    PinnedFile(PinnedFile&& other) noexcept
        : stream(other.stream), size(other.size), device(other.device), file(other.file),
          identityValid(other.identityValid)
    {
        other.stream = nullptr;
        other.size = 0;
        other.identityValid = false;
    }
    PinnedFile& operator=(PinnedFile&& other) noexcept
    {
        if (this == &other)
            return *this;
        if (stream)
            std::fclose(stream);
        stream = other.stream;
        size = other.size;
        device = other.device;
        file = other.file;
        identityValid = other.identityValid;
        other.stream = nullptr;
        other.size = 0;
        other.identityValid = false;
        return *this;
    }
    ~PinnedFile()
    {
        if (stream)
            std::fclose(stream);
    }
};

static PinnedFile OpenPinnedInputFile(const std::string& path)
{
    PinnedFile result;
    std::filesystem::path name;
    if (!ArtifactNameInPinnedRoot(path, name))
        return result;
#ifdef SPARK_PLATFORM_WINDOWS
    HANDLE handle = OpenArtifactRelativeToPinnedRoot(name, GENERIC_READ, FILE_OPEN);
    if (handle == INVALID_HANDLE_VALUE)
        return result;

    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 || info.nNumberOfLinks != 1)
    {
        CloseHandle(handle);
        return result;
    }
    result.size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    result.device = info.dwVolumeSerialNumber;
    result.file = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    result.identityValid = true;
    const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDONLY | _O_BINARY);
    if (descriptor < 0)
    {
        CloseHandle(handle);
        return {};
    }
    result.stream = _fdopen(descriptor, "rb");
    if (!result.stream)
    {
        _close(descriptor);
        return {};
    }
#else
    int flags = O_RDONLY;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const int descriptor = openat(g_artifactRootHandle, name.c_str(), flags);
    if (descriptor < 0)
        return result;

    struct stat info{};
    if (fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1)
    {
        close(descriptor);
        return result;
    }
    result.size = static_cast<std::uint64_t>(info.st_size);
    result.device = static_cast<std::uint64_t>(info.st_dev);
    result.file = static_cast<std::uint64_t>(info.st_ino);
    result.identityValid = true;
    result.stream = fdopen(descriptor, "rb");
    if (!result.stream)
    {
        close(descriptor);
        return {};
    }
#endif
    return result;
}

static PinnedFile CreateExclusiveOutputFile(const std::string& path)
{
    PinnedFile result;
    std::filesystem::path name;
    if (!ArtifactNameInPinnedRoot(path, name))
        return result;
#ifdef SPARK_PLATFORM_WINDOWS
    HANDLE handle = OpenArtifactRelativeToPinnedRoot(name, GENERIC_READ | GENERIC_WRITE, FILE_CREATE);
    if (handle == INVALID_HANDLE_VALUE)
        return result;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info) || info.nNumberOfLinks != 1 ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
    {
        CloseHandle(handle);
        return result;
    }
    result.device = info.dwVolumeSerialNumber;
    result.file = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    result.identityValid = true;
    const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDWR | _O_BINARY);
    if (descriptor < 0)
    {
        CloseHandle(handle);
        return {};
    }
    result.stream = _fdopen(descriptor, "w+b");
    if (!result.stream)
    {
        _close(descriptor);
        return {};
    }
#else
    int flags = O_RDWR | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const int descriptor = openat(g_artifactRootHandle, name.c_str(), flags, S_IRUSR | S_IWUSR);
    if (descriptor < 0)
        return result;
    struct stat info{};
    if (fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1)
    {
        close(descriptor);
        return result;
    }
    result.device = static_cast<std::uint64_t>(info.st_dev);
    result.file = static_cast<std::uint64_t>(info.st_ino);
    result.identityValid = true;
    result.stream = fdopen(descriptor, "w+b");
    if (!result.stream)
    {
        close(descriptor);
        return {};
    }
#endif
    return result;
}

static bool ZipFilesUtf8(const std::string& zip, const std::vector<std::string>& files,
                         const std::vector<PinnedFile*>& expectedInputs, PinnedFile* pinnedArchive = nullptr)
{
    if (files.empty() || files.size() != expectedInputs.size())
        return false;
    PinnedFile output = CreateExclusiveOutputFile(zip);
    if (!output.stream)
        return false;
    mz_zip_archive za{};
    if (!mz_zip_writer_init_cfile(&za, output.stream, 0))
        return false;

    bool success = true;
    for (size_t index = 0; index < files.size(); ++index)
    {
        const std::string& f = files[index];
        PinnedFile input = OpenPinnedInputFile(f);
        const PinnedFile* expected = expectedInputs[index];
        if (!input.stream || !expected || !expected->identityValid || input.device != expected->device ||
            input.file != expected->file)
        {
            success = false;
            break;
        }
        const std::string entry =
            Spark::CrashHandlerDetail::PathToUtf8(Spark::CrashHandlerDetail::PathFromUtf8(f).filename());
        if (!mz_zip_writer_add_cfile(&za, entry.c_str(), input.stream, input.size, nullptr, nullptr, 0,
                                     MZ_BEST_COMPRESSION, nullptr, 0, nullptr, 0))
        {
            success = false;
            break;
        }
    }

    if (success)
        success = mz_zip_writer_finalize_archive(&za) != 0;
    mz_zip_writer_end(&za);

    if (!success)
    {
        std::fclose(output.stream);
        output.stream = nullptr;
    }
    else if (pinnedArchive)
    {
        if (std::fflush(output.stream) != 0)
            return false;
        *pinnedArchive = std::move(output);
    }
    return success;
}

static std::string StableUploadPath(PinnedFile& pinnedFile, const std::string& originalPath)
{
    if (!pinnedFile.stream || !pinnedFile.identityValid)
        return {};
#ifdef SPARK_PLATFORM_WINDOWS
    const intptr_t osHandle = _get_osfhandle(_fileno(pinnedFile.stream));
    BY_HANDLE_FILE_INFORMATION info{};
    if (osHandle == -1 || !GetFileInformationByHandle(reinterpret_cast<HANDLE>(osHandle), &info) ||
        info.nNumberOfLinks != 1 || info.dwVolumeSerialNumber != pinnedFile.device ||
        ((static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow) != pinnedFile.file)
    {
        return {};
    }
#else
    struct stat info{};
    if (fstat(fileno(pinnedFile.stream), &info) != 0 || info.st_nlink != 1 ||
        static_cast<std::uint64_t>(info.st_dev) != pinnedFile.device ||
        static_cast<std::uint64_t>(info.st_ino) != pinnedFile.file)
    {
        return {};
    }
#endif
    if (std::fseek(pinnedFile.stream, 0, SEEK_SET) != 0)
        return {};
#ifdef SPARK_PLATFORM_WINDOWS
    return originalPath;
#elif defined(SPARK_PLATFORM_LINUX)
    return "/proc/self/fd/" + std::to_string(fileno(pinnedFile.stream));
#else
    return "/dev/fd/" + std::to_string(fileno(pinnedFile.stream));
#endif
}

static bool WriteExclusiveFileUtf8(const std::string& path, const std::string& content)
{
#ifdef SPARK_PLATFORM_WINDOWS
    PinnedFile output = CreateExclusiveOutputFile(path);
    if (!output.stream)
        return false;
    const size_t written = std::fwrite(content.data(), 1, content.size(), output.stream);
    return written == content.size() && std::fflush(output.stream) == 0 && std::ferror(output.stream) == 0;
#else
    std::filesystem::path name;
    if (!ArtifactNameInPinnedRoot(path, name))
        return false;

    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const int descriptor = openat(g_artifactRootHandle, name.c_str(), flags, S_IRUSR | S_IWUSR);
    if (descriptor < 0)
        return false;

    size_t offset = 0;
    while (offset < content.size())
    {
        const ssize_t count = write(descriptor, content.data() + offset, content.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        offset += static_cast<size_t>(count);
    }
    const int closeResult = close(descriptor);
    const bool success = offset == content.size() && closeResult == 0;
    if (!success)
        unlinkat(g_artifactRootHandle, name.c_str(), 0);
    return success;
#endif
}

// ============================================================================
// Crash manifest for out-of-process reporter
// ============================================================================

static std::string g_manifestDir; // Private artifact root created during InstallCrashHandler
static bool g_reporterLaunched = false;

static std::filesystem::path GetCrashArtifactPrefix()
{
#ifdef SPARK_PLATFORM_WINDOWS
    std::filesystem::path configuredPrefix(g_cfg.dumpPrefix);
#else
    std::filesystem::path configuredPrefix(WideToUtf8(g_cfg.dumpPrefix));
#endif
    if (g_manifestDir.empty())
        return {};

    std::filesystem::path filePrefix = configuredPrefix.filename();
    if (filePrefix.empty())
        filePrefix = "GameEngineCrash";
    return Spark::CrashHandlerDetail::PathFromUtf8(g_manifestDir) / filePrefix;
}

static unsigned long GetEnginePID()
{
#ifdef SPARK_PLATFORM_WINDOWS
    return static_cast<unsigned long>(GetCurrentProcessId());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

static bool InitializeCrashArtifactDirectory()
{
    std::error_code error;
    const std::filesystem::path tempDirectory = std::filesystem::temp_directory_path(error);
    if (error)
        return false;

    const std::filesystem::path artifactDirectory =
        Spark::CrashHandlerDetail::CreatePrivateCrashArtifactDirectory(tempDirectory, GetEnginePID());
    if (artifactDirectory.empty())
        return false;

    if (!PinArtifactRoot(artifactDirectory))
        return false;

#ifdef SPARK_PLATFORM_WINDOWS
    g_manifestDir = WideToUtf8(artifactDirectory.wstring());
#else
    g_manifestDir = artifactDirectory.string();
#endif
    return !g_manifestDir.empty();
}

static size_t CountPendingCrashManifests()
{
    size_t count = 0;
#ifdef SPARK_PLATFORM_WINDOWS
    WIN32_FIND_DATAW entry{};
    const std::filesystem::path pattern = g_artifactRootPath / L"crash_manifest_*.json";
    HANDLE search = FindFirstFileW(pattern.c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE)
        return 0;
    do
    {
        const std::string name = Spark::CrashHandlerDetail::PathToUtf8(std::filesystem::path(entry.cFileName));
        if (Spark::CrashHandlerDetail::IsCrashManifestReadyName(name))
            ++count;
    } while (count < Spark::CrashHandlerDetail::kMaxPendingCrashManifests && FindNextFileW(search, &entry));
    FindClose(search);
#else
    const int duplicate = dup(g_artifactRootHandle);
    if (duplicate < 0)
        return Spark::CrashHandlerDetail::kMaxPendingCrashManifests;
    DIR* directory = fdopendir(duplicate);
    if (!directory)
    {
        close(duplicate);
        return Spark::CrashHandlerDetail::kMaxPendingCrashManifests;
    }
    while (const dirent* entry = readdir(directory))
    {
        if (Spark::CrashHandlerDetail::IsCrashManifestReadyName(entry->d_name))
            ++count;
        if (count >= Spark::CrashHandlerDetail::kMaxPendingCrashManifests)
            break;
    }
    closedir(directory);
#endif
    return count;
}

static bool PublishCrashManifest(std::string_view reportId, const std::string& json)
{
    const std::string readyName = Spark::CrashHandlerDetail::CrashManifestReadyName(reportId);
    if (readyName.empty() || !Spark::CrashHandlerDetail::HasCrashManifestQueueCapacity(CountPendingCrashManifests()))
        return false;

    const std::string temporaryName = readyName + ".tmp";
    const std::string temporaryPath = Spark::CrashHandlerDetail::PathToUtf8(
        g_artifactRootPath / Spark::CrashHandlerDetail::PathFromUtf8(temporaryName));
    if (!WriteExclusiveFileUtf8(temporaryPath, json))
        return false;

#ifdef SPARK_PLATFORM_WINDOWS
    HANDLE temporary = OpenArtifactRelativeToPinnedRoot(Spark::CrashHandlerDetail::PathFromUtf8(temporaryName),
                                                        DELETE | FILE_READ_ATTRIBUTES, FILE_OPEN);
    if (temporary == INVALID_HANDLE_VALUE)
        return false;

    const std::filesystem::path readyPath = Spark::CrashHandlerDetail::PathFromUtf8(readyName);
    const std::wstring readyNative = readyPath.native();
    const size_t renameBytes = sizeof(FILE_RENAME_INFO) + readyNative.size() * sizeof(wchar_t);
    std::vector<std::byte> renameStorage(renameBytes);
    auto* renameInfo = reinterpret_cast<FILE_RENAME_INFO*>(renameStorage.data());
    renameInfo->ReplaceIfExists = FALSE;
    renameInfo->RootDirectory = g_artifactRootHandle;
    renameInfo->FileNameLength = static_cast<DWORD>(readyNative.size() * sizeof(wchar_t));
    std::memcpy(renameInfo->FileName, readyNative.data(), renameInfo->FileNameLength);
    const bool published = SetFileInformationByHandle(temporary, FileRenameInfo, renameInfo,
                                                      static_cast<DWORD>(renameStorage.size())) != FALSE;
    if (!published)
    {
        FILE_DISPOSITION_INFO disposition{};
        disposition.DeleteFile = TRUE;
        SetFileInformationByHandle(temporary, FileDispositionInfo, &disposition, sizeof(disposition));
    }
    CloseHandle(temporary);
    return published;
#else
    const bool published =
        renameat(g_artifactRootHandle, temporaryName.c_str(), g_artifactRootHandle, readyName.c_str()) == 0;
    if (!published)
        unlinkat(g_artifactRootHandle, temporaryName.c_str(), 0);
    return published;
#endif
}

static std::string MakeManifestJson(const std::string& dumpFile, const std::string& logFile,
                                    const std::string& screenshotFile, const std::string& zipFile,
                                    const std::string& crashTitle)
{
    auto jsonEsc = [](const std::string& s) -> std::string
    {
        std::string out;
        constexpr char hex[] = "0123456789abcdef";
        for (unsigned char c : s)
        {
            if (c == '"')
                out += "\\\"";
            else if (c == '\\')
                out += "\\\\";
            else if (c == '\b')
                out += "\\b";
            else if (c == '\f')
                out += "\\f";
            else if (c == '\n')
                out += "\\n";
            else if (c == '\r')
                out += "\\r";
            else if (c == '\t')
                out += "\\t";
            else if (c < 0x20)
            {
                out += "\\u00";
                out.push_back(hex[c >> 4]);
                out.push_back(hex[c & 0x0F]);
            }
            else
                out += static_cast<char>(c);
        }
        return out;
    };

    std::ostringstream j;
    j << "{\n";
    j << "  \"enginePID\": \"" << GetEnginePID() << "\",\n";
    j << "  \"timestamp\": \"" << jsonEsc(MakeTimeStampUtf8()) << "\",\n";
    j << "  \"dumpFile\": \"" << jsonEsc(dumpFile) << "\",\n";
    j << "  \"logFile\": \"" << jsonEsc(logFile) << "\",\n";
    j << "  \"screenshotFile\": \"" << jsonEsc(screenshotFile) << "\",\n";
    j << "  \"zipFile\": \"" << jsonEsc(zipFile) << "\",\n";
    j << "  \"crashTitle\": \"" << jsonEsc(crashTitle) << "\",\n";
    j << "  \"requireConsent\": " << (g_cfg.requireConsent ? "true" : "false") << ",\n";
    j << "  \"allowScreenshotRefusal\": " << (g_cfg.allowScreenshotRefusal ? "true" : "false") << ",\n";
    j << "  \"promptUserDescription\": " << (g_cfg.promptUserDescription ? "true" : "false") << ",\n";
    j << "  \"fullMemoryDump\": " << (g_cfg.captureFullMemoryDump ? "true" : "false") << "\n";
    j << "}\n";
    return j.str();
}

static bool WriteCrashManifest(std::string_view reportId, const std::string& dumpFile, const std::string& logFile,
                               const std::string& screenshotFile, const std::string& zipFile,
                               const std::string& crashTitle)
{
    if (g_manifestDir.empty())
        return false;

    std::string json = MakeManifestJson(dumpFile, logFile, screenshotFile, zipFile, crashTitle);
    return PublishCrashManifest(reportId, json);
}

// Try to launch SparkCrashReporter in watchdog mode
static bool LaunchCrashReporter()
{
    if (g_manifestDir.empty())
        return false;

    // Resolve only from the running executable's canonical directory. The
    // launcher working directory can be a project/package root and is not a
    // trustworthy executable search location.
    const std::filesystem::path executableDirectory = Spark::RuntimePackage::GetExecutableDirectory();
    const std::filesystem::path reporterPath =
        Spark::CrashHandlerDetail::ResolveCrashReporterExecutable(executableDirectory);
    if (reporterPath.empty())
    {
        SPARK_LOG_DEBUG(Spark::LogCategory::Core,
                        "CrashHandler: SparkCrashReporter not found, using in-process reporting");
        return false;
    }

    // Launch reporter in watchdog mode (detached — survives engine crash)
    const std::string reporterPathUtf8 =
#ifdef SPARK_PLATFORM_WINDOWS
        WideToUtf8(reporterPath.wstring());
#else
        reporterPath.string();
#endif
    auto result = Spark::Process::Builder(reporterPathUtf8)
                      .Arg("--watch")
                      .Arg(g_manifestDir)
                      .Arg(std::to_string(GetEnginePID()))
                      .Detached()
                      .Launch();

    if (result)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Core, "CrashHandler: Launched SparkCrashReporter (watchdog mode)");
        return true;
    }
    else
    {
        SPARK_LOG_WARN(Spark::LogCategory::Core, "CrashHandler: Failed to launch SparkCrashReporter: %s",
                       result.error().c_str());
        return false;
    }
}

// ============================================================================
// WINDOWS IMPLEMENTATION
// ============================================================================

#ifdef SPARK_PLATFORM_WINDOWS

// Engine must implement these
extern IDXGISwapChain* GetMainSwapChain();
extern ID3D11Device* GetD3DDevice();
extern ID3D11DeviceContext* GetD3DContext();

// Forward declarations
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep);
static void HandleCrashInternal(EXCEPTION_POINTERS* ep, const char* assertMsg);
static bool WriteMiniDump(const std::wstring& path, EXCEPTION_POINTERS* ep);
static std::wstring MakeTimeStamp();
static std::wstring SymStackTrace(EXCEPTION_POINTERS* ep);
static std::wstring SystemInfo();
static std::wstring ThreadStacks(DWORD skipThreadId);
static std::wstring ThreadStacksBounded(bool& outTimedOut);
static bool SaveScreenshot(const std::wstring& file);
static bool ZipFiles(const std::wstring& zip, const std::vector<std::wstring>& files,
                     const std::vector<PinnedFile*>& expectedInputs, PinnedFile* pinnedArchive = nullptr);

// ---------------------------------------------------------------------------
// Teardown detection + bounded all-thread stack capture
//
// Live-debug history (cdb): ThreadStacks() used to suspend every thread in the
// process from the CRASHING thread — including, eventually, threads holding
// the loader/heap/dbghelp locks that StackWalk64 itself needs, and during
// static-destructor crashes threads that the OS was already tearing down. The
// handler then parked forever in NtSuspendThread and the process zombied
// instead of dying. The rules now are:
//   1. never capture thread stacks at all during process teardown,
//   2. run the capture on a helper thread with a hard watchdog timeout,
//   3. the helper never suspends itself NOR the crashing thread that is
//      blocked waiting on it (suspending the waiter would freeze the watchdog
//      and re-create the zombie),
//   4. on timeout: keep the process dump + log already produced, skip everything
//      that could touch a suspended thread, and TerminateProcess.
// ---------------------------------------------------------------------------

static std::atomic<bool> g_processTeardown{false}; ///< set once exit/static-dtor teardown begins

using RtlDllShutdownInProgressFn = BOOLEAN(NTAPI*)();
static RtlDllShutdownInProgressFn g_rtlDllShutdownInProgress = nullptr; // resolved in InstallCrashHandler

static bool IsProcessTeardown()
{
    if (g_processTeardown.load(std::memory_order_relaxed))
        return true;
    // Authoritative OS-side answer (TRUE once ExitProcess/static-dtor shutdown
    // has begun) — catches teardown paths that bypass our atexit hook.
    return g_rtlDllShutdownInProgress && g_rtlDllShutdownInProgress();
}

namespace
{
    constexpr DWORD kThreadStacksTimeoutMs = 5000;

    /// Shared with the (possibly abandoned) helper thread — static storage so a
    /// wedged helper writing late can never touch a dead stack frame.
    struct ThreadStacksCapture
    {
        std::wstring result;
        DWORD crashThreadId = 0; ///< the thread waiting on the helper; never suspend it
        HANDLE done = nullptr;
    };
} // namespace

static DWORD WINAPI ThreadStacksThreadProc(LPVOID param)
{
    auto* cap = static_cast<ThreadStacksCapture*>(param);
    cap->result = ThreadStacks(cap->crashThreadId);
    SetEvent(cap->done);
    return 0;
}

/// All-thread stack capture with the crash-safety bounds described above.
/// On timeout returns a placeholder note and sets outTimedOut — the caller
/// must finish the report WITHOUT touching other threads and terminate.
static std::wstring ThreadStacksBounded(bool& outTimedOut)
{
    outTimedOut = false;

    if (IsProcessTeardown())
        return L"*** THREAD STACKS ***\nSkipped: process teardown in progress (thread suspension is unsafe here)\n";

    static ThreadStacksCapture s_cap; // static: must outlive an abandoned helper
    s_cap.result.clear();
    s_cap.crashThreadId = GetCurrentThreadId();
    s_cap.done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!s_cap.done)
        return L"*** THREAD STACKS ***\nSkipped: watchdog event creation failed\n";

    HANDLE helper = CreateThread(nullptr, 0, ThreadStacksThreadProc, &s_cap, 0, nullptr);
    if (!helper)
    {
        CloseHandle(s_cap.done);
        s_cap.done = nullptr;
        return L"*** THREAD STACKS ***\nSkipped: watchdog helper thread creation failed\n";
    }

    if (WaitForSingleObject(s_cap.done, kThreadStacksTimeoutMs) == WAIT_OBJECT_0)
    {
        CloseHandle(helper);
        CloseHandle(s_cap.done);
        s_cap.done = nullptr;
        return std::move(s_cap.result);
    }

    // Wedged (historically inside NtSuspendThread / StackWalk64 against a
    // suspended lock-holder). Do NOT wait longer, resume anything, or read
    // s_cap.result — the helper may still be mutating it. Handles are leaked
    // deliberately; the process is about to be terminated by the caller.
    outTimedOut = true;
    return L"*** THREAD STACKS ***\nSkipped: capture timed out after 5 s (thread-suspension wedge) — "
           L"report written without thread stacks\n";
}

void InstallCrashHandler(const CrashConfig& cfg)
{
    SPARK_LOG_INFO(Spark::LogCategory::Core, "Installing crash handler (Windows)");
    AssignCrashConfig(cfg);
    g_triggerCrashOnAssert = cfg.triggerCrashOnAssert;
    g_reporterLaunched = false;
    g_manifestDir.clear();
    ResetPinnedArtifactRoot();
    if (!InitializeCrashArtifactDirectory())
    {
        SPARK_LOG_ERROR(
            Spark::LogCategory::Core,
            "CrashHandler: failed to create private crash-artifact directory; filesystem artifacts disabled");
    }

#ifdef SPARK_HAS_CURL
    curl_global_init(CURL_GLOBAL_DEFAULT);
#endif

    // Teardown detection (see block comment above): resolve the ntdll probe up
    // front so the crash path never calls GetProcAddress, and set our own flag
    // as soon as normal exit processing starts.
    g_rtlDllShutdownInProgress = reinterpret_cast<RtlDllShutdownInProgressFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlDllShutdownInProgress")));
    std::atexit([] { g_processTeardown.store(true, std::memory_order_relaxed); });

    SetUnhandledExceptionFilter(CrashFilter);

    // The external reporter is intentionally read-only. Keep consent and upload
    // ownership in-process whenever reporting is enabled, and never show UI headlessly.
    if (Spark::CrashHandlerDetail::ShouldLaunchReadOnlyReporter(g_cfg.enableCrashReporting, g_cfg.headlessMode))
        g_reporterLaunched = LaunchCrashReporter();

    SPARK_LOG_INFO(Spark::LogCategory::Core, "Crash handler installed successfully");
}

void TriggerCrashHandler(const char* assertMsg)
{
    if (!g_triggerCrashOnAssert)
    {
        std::string logMsg = "Assert triggered but crash handling disabled: ";
        if (assertMsg)
            logMsg += assertMsg;
        try
        {
            Spark::ConsoleProcessManager::GetInstance().LogCrash(logMsg);
        }
        catch (const std::exception& e)
        {
            OutputDebugStringA("[SPARK ENGINE] Exception: ");
            OutputDebugStringA(e.what());
            OutputDebugStringA("\n");
        }
        catch (...)
        {
            OutputDebugStringA("[SPARK ENGINE] Unknown exception in crash handler\n");
        }
        return;
    }

    EXCEPTION_RECORD rec{};
    rec.ExceptionCode = STATUS_FATAL_APP_EXIT;
    rec.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
#if defined(_MSC_VER)
    rec.ExceptionAddress = _ReturnAddress();
#elif defined(__GNUC__)
    rec.ExceptionAddress = __builtin_return_address(0);
#endif

    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    RtlCaptureContext(&ctx);

    EXCEPTION_POINTERS ep{&rec, &ctx};
    HandleCrashInternal(&ep, assertMsg);
}

void SetAssertCrashBehavior(bool shouldCrash)
{
    std::lock_guard<std::mutex> lock(g_lock);
    g_triggerCrashOnAssert = shouldCrash;
    try
    {
        std::string logMsg = "Assert crash behavior changed to: ";
        logMsg += (shouldCrash ? "ENABLED" : "DISABLED");
        Spark::ConsoleProcessManager::GetInstance().LogCrash(logMsg);
    }
    catch (const std::exception& e)
    {
        OutputDebugStringA("[SPARK ENGINE] Exception: ");
        OutputDebugStringA(e.what());
        OutputDebugStringA("\n");
    }
    catch (...)
    {
        OutputDebugStringA("[SPARK ENGINE] Unknown exception in crash handler\n");
    }
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep)
{
    HandleCrashInternal(ep, nullptr);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void HandleCrashInternal(EXCEPTION_POINTERS* ep, const char* assertMsg)
{
    std::lock_guard<std::mutex> guard(g_lock);
    if (!ep)
        return;

    const std::wstring prefix = GetCrashArtifactPrefix().wstring();
    if (prefix.empty())
    {
        OutputDebugStringA("[SPARK ENGINE] Private crash-artifact directory unavailable; report not written.\n");
        return;
    }
    const std::wstring stamp = MakeTimeStamp();
    const std::string reportId = MakeCrashReportId();
    const std::wstring reportSuffix = L"_" + std::wstring(reportId.begin(), reportId.end());
    std::wstring dump = prefix + reportSuffix + L".dmp";
    std::wstring logFile = prefix + reportSuffix + L".log";
    std::wstring shot = prefix + reportSuffix + L".png";
    std::wstring zipFile = prefix + reportSuffix + L".zip";

    const bool dumpReady = WriteMiniDump(dump, ep);

    std::wstringstream log;
    log << L"================================================================\n";
    log << L"           SPARK ENGINE CRASH REPORT\n";
    log << L"================================================================\n\n";
    log << L"Timestamp  : " << stamp << L"\n";
    log << L"Process ID : " << GetCurrentProcessId() << L"\n";
    log << L"Thread ID  : 0x" << std::hex << GetCurrentThreadId() << std::dec << L"\n\n";

    if (assertMsg)
    {
        int len = MultiByteToWideChar(CP_UTF8, 0, assertMsg, -1, nullptr, 0);
        std::wstring wmsg(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, assertMsg, -1, &wmsg[0], len);
        log << L"*** ASSERTION FAILURE ***\n" << wmsg << L"\n\n";
    }
    else
    {
        log << L"*** CRASH DETECTED ***\n\n";
    }

    if (ep->ExceptionRecord)
    {
        DWORD code = ep->ExceptionRecord->ExceptionCode;
        const char* codeName = SparkError::ExceptionCodeToString(code);
        int codeNameLen = MultiByteToWideChar(CP_UTF8, 0, codeName, -1, nullptr, 0);
        std::wstring wCodeName(codeNameLen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, codeName, -1, &wCodeName[0], codeNameLen);

        log << L"Exception Code    : 0x" << std::hex << code << std::dec << L"\n";
        log << L"Exception Name    : " << wCodeName << L"\n";
        log << L"Exception Address : 0x" << std::hex
            << reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress) << std::dec << L"\n";
        log << L"Exception Flags   : " << ep->ExceptionRecord->ExceptionFlags << L"\n";
        log << L"Number Parameters : " << ep->ExceptionRecord->NumberParameters << L"\n";

        if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
        {
            ULONG_PTR accessType = ep->ExceptionRecord->ExceptionInformation[0];
            ULONG_PTR targetAddr = ep->ExceptionRecord->ExceptionInformation[1];
            const wchar_t* accessStr = (accessType == 0)   ? L"READ"
                                       : (accessType == 1) ? L"WRITE"
                                       : (accessType == 8) ? L"DEP_VIOLATION"
                                                           : L"UNKNOWN";
            log << L"Access Type       : " << accessStr << L"\n";
            log << L"Target Address    : 0x" << std::hex << targetAddr << std::dec << L"\n";
        }
        log << L"\n";
    }

    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc)))
    {
        log << L"*** PROCESS MEMORY ***\n";
        log << L"Working Set       : " << (pmc.WorkingSetSize >> 20) << L" MiB\n";
        log << L"Peak Working Set  : " << (pmc.PeakWorkingSetSize >> 20) << L" MiB\n";
        log << L"Private Bytes     : " << (pmc.PrivateUsage >> 20) << L" MiB\n";
        log << L"Page Faults       : " << pmc.PageFaultCount << L"\n\n";
    }

    if (ep->ExceptionRecord && ep->ExceptionRecord->ExceptionAddress)
    {
        HMODULE hMod = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)ep->ExceptionRecord->ExceptionAddress, &hMod))
        {
            wchar_t modPath[MAX_PATH] = {};
            GetModuleFileNameW(hMod, modPath, MAX_PATH);
            log << L"Faulting Module   : " << modPath << L"\n\n";
        }
    }

    log << SymStackTrace(ep);
    if (g_cfg.captureSystemInfo)
        log << SystemInfo();
    bool threadStacksTimedOut = false;
    if (g_cfg.captureAllThreads)
        log << ThreadStacksBounded(threadStacksTimedOut);

    // Skipped after a thread-stacks timeout: the console-process IPC can block
    // on the same suspended threads the wedged helper left behind.
    if (!threadStacksTimedOut)
    {
        try
        {
            std::string crashSummary = assertMsg ? "ASSERTION FAILURE" : "CRASH DETECTED";
            crashSummary += "\nDump file: " + WideToUtf8(dump);
            crashSummary += "\nLog file: " + WideToUtf8(logFile);
            Spark::ConsoleProcessManager::GetInstance().LogCrash(crashSummary);
        }
        catch (const std::exception& e)
        {
            OutputDebugStringA("[SPARK ENGINE] Exception: ");
            OutputDebugStringA(e.what());
            OutputDebugStringA("\n");
        }
        catch (...)
        {
            OutputDebugStringA("[SPARK ENGINE] Unknown exception in crash handler\n");
        }
    }

    const bool logReady = WriteExclusiveFileUtf8(WideToUtf8(logFile), WideToUtf8(log.str()));

    if (threadStacksTimedOut)
    {
        // The wedged helper may have left arbitrary threads suspended: the
        // screenshot (render thread), zip/upload (heap, sockets) and dialog
        // pumps below could all deadlock the same way. The process dump and log
        // are already on disk — hand the manifest to the out-of-process
        // reporter and die hard instead of zombieing (the historical failure
        // mode this watchdog exists for).
        if (g_reporterLaunched && logReady)
        {
            WriteCrashManifest(reportId, dumpReady ? WideToUtf8(dump) : std::string{}, WideToUtf8(logFile), "", "",
                               assertMsg ? "Assertion Failure (thread-stack capture timed out)"
                                         : "Crash Detected (thread-stack capture timed out)");
        }
        TerminateProcess(GetCurrentProcess(), ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode
                                                                  : static_cast<DWORD>(STATUS_FATAL_APP_EXIT));
    }

    const bool screenshotWritten = g_cfg.captureScreenshot && SaveScreenshot(shot);

    PinnedFile dumpProbe = dumpReady ? OpenPinnedInputFile(WideToUtf8(dump)) : PinnedFile{};
    PinnedFile logProbe = logReady ? OpenPinnedInputFile(WideToUtf8(logFile)) : PinnedFile{};
    PinnedFile screenshotProbe = screenshotWritten ? OpenPinnedInputFile(WideToUtf8(shot)) : PinnedFile{};
    const bool screenshotAvailable = screenshotProbe.stream != nullptr;

    // Reporter launch success is the ownership switch. The engine emits only
    // raw artifacts plus a manifest and never performs its own consent,
    // archive, or upload path in this mode.
    if (g_reporterLaunched)
    {
        if (logProbe.stream)
        {
            const std::string crashTitle = assertMsg ? "Assertion Failure" : "Crash Detected";
            WriteCrashManifest(reportId, dumpProbe.stream ? WideToUtf8(dump) : std::string{}, WideToUtf8(logFile),
                               screenshotAvailable ? WideToUtf8(shot) : std::string{}, "", crashTitle);
        }
        return;
    }

    // ---- Reporter-unavailable fallback: this process alone owns consent and upload. ----
    bool ok = true;
    if (g_cfg.enableCrashReporting)
    {
        bool userConsented = true;
        bool includeScreenshot = true;

        if (g_cfg.requireConsent && !g_cfg.headlessMode)
        {
            std::wstring consentMsg = L"SparkEngine has crashed. Would you like to send a crash report "
                                      L"to help improve the engine?\n\nThe report can include ";
            if (dumpReady && g_cfg.captureFullMemoryDump)
            {
                consentMsg += L"a full-memory process dump (which can contain application or user data held in "
                              L"memory), ";
            }
            else if (dumpReady)
            {
                consentMsg += L"a minimal process dump (which can still contain limited memory and file paths), ";
            }
            consentMsg += L"stack traces and system/process information. These diagnostics may contain personal "
                          L"or sensitive data.";
            if (screenshotAvailable && g_cfg.allowScreenshotRefusal)
            {
                consentMsg += L"\n\nA screenshot was captured locally. You can choose whether to include it "
                              L"in the next dialog.";
            }
            else if (screenshotAvailable)
            {
                consentMsg += L"\n\nThe report also includes a screenshot of the last rendered frame.";
            }

            int result = MessageBoxW(nullptr, consentMsg.c_str(), L"Crash Report", MB_YESNO | MB_ICONERROR);
            userConsented = (result == IDYES);

            // Screenshot opt-out dialog
            if (userConsented && g_cfg.allowScreenshotRefusal && screenshotAvailable)
            {
                int ssResult = MessageBoxW(nullptr,
                                           L"Include a screenshot of the last rendered frame with the "
                                           L"crash report?",
                                           L"Screenshot Consent", MB_YESNO | MB_ICONERROR);
                includeScreenshot = (ssResult == IDYES);
            }
        }

        if (userConsented)
        {
            bool archiveReady = !g_cfg.zipBeforeUpload;
            PinnedFile archivePin;
            if (g_cfg.zipBeforeUpload)
            {
                const std::vector<std::wstring> approvedFiles = Spark::CrashHandlerDetail::BuildCrashArchiveAllowlist(
                    dumpProbe.stream ? dump : std::wstring{}, logProbe.stream ? logFile : std::wstring{}, shot,
                    includeScreenshot && screenshotAvailable);
                std::vector<PinnedFile*> approvedPins;
                if (dumpProbe.stream)
                    approvedPins.push_back(&dumpProbe);
                if (logProbe.stream)
                    approvedPins.push_back(&logProbe);
                if (includeScreenshot && screenshotAvailable)
                    approvedPins.push_back(&screenshotProbe);
                archiveReady = ZipFiles(zipFile, approvedFiles, approvedPins, &archivePin);
            }

            // User description input (Windows: simple InputBox via a small console prompt)
            // On Windows we can't easily show a text input without a full GUI framework,
            // so we use a MessageBox prompt with a follow-up note in the crash log.
            // The description is appended to the log content before upload.
            std::string userDesc;
            if (g_cfg.promptUserDescription && !g_cfg.headlessMode)
            {
                // Note: A proper implementation would use a text input dialog.
                // For now, we add a placeholder that the out-of-process reporter
                // (CrashReporter.exe) will replace with a real text input GUI.
                int descResult = MessageBoxW(nullptr,
                                             L"The crash report will be sent. If you'd like to describe "
                                             L"what you were doing when the crash occurred, please note "
                                             L"it and include it in a GitHub issue.\n\n"
                                             L"(A future update will add a text input here.)",
                                             L"Additional Information", MB_OK | MB_ICONERROR);
                (void)descResult;
            }

            std::string logUtf8 = WideToUtf8(log.str());
            if (!userDesc.empty())
                logUtf8 = "=== User Description ===\n" + userDesc + "\n\n" + logUtf8;

            // Copy config and attach user description
            CrashConfig uploadCfg = g_cfg;
            uploadCfg.userDescription = userDesc;
            const std::string archivePath = WideToUtf8(zipFile);
            const std::string approvedArchive =
                g_cfg.zipBeforeUpload && archiveReady ? StableUploadPath(archivePin, archivePath) : std::string{};
            ok = UploadCrashReport(uploadCfg, logUtf8, approvedArchive);
        }
    }

    if (!g_cfg.headlessMode)
    {
        std::wstring msg = assertMsg ? L"Assertion captured.\n" : L"Crash captured.\n";
        msg += L"Files:\n" + dump + L"\n" + logFile;
        MessageBoxW(nullptr, msg.c_str(), assertMsg ? L"Assertion Handler" : L"Crash Handler", MB_OK | MB_ICONERROR);
    }
}

static bool WriteMiniDump(const std::wstring& file, EXCEPTION_POINTERS* ep)
{
    HANDLE h =
        CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), ep, TRUE};
    MINIDUMP_TYPE dumpType = MiniDumpNormal;
    if (g_cfg.captureFullMemoryDump)
    {
        dumpType =
            static_cast<MINIDUMP_TYPE>(MiniDumpWithFullMemory | MiniDumpWithHandleData | MiniDumpWithUnloadedModules);
    }
    const bool written =
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h, dumpType, &info, nullptr, nullptr) != FALSE;
    CloseHandle(h);
    return written;
}

static std::wstring MakeTimeStamp()
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t buf[32];
    swprintf_s(buf, L"_%04d%02d%02d_%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return buf;
}

static std::wstring SymStackTrace(EXCEPTION_POINTERS* ep)
{
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    std::wstringstream out;
    out << L"*** STACK TRACE ***\n";

    CONTEXT& ctx = *ep->ContextRecord;
    STACKFRAME64 frame{};
#ifdef _WIN64
    DWORD machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrStack.Offset = ctx.Rsp;
#else
    DWORD machine = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset = ctx.Eip;
    frame.AddrFrame.Offset = ctx.Ebp;
    frame.AddrStack.Offset = ctx.Esp;
#endif
    frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;

    BYTE symBuffer[sizeof(SYMBOL_INFO) + 256] = {};
    SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(symBuffer);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;

    for (int i = 0; i < 32; ++i)
    {
        if (!StackWalk64(machine, GetCurrentProcess(), GetCurrentThread(), &frame, &ctx, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
            !frame.AddrPC.Offset)
            break;

        DWORD64 disp = 0;
        if (SymFromAddr(GetCurrentProcess(), frame.AddrPC.Offset, &disp, sym))
        {
            out << L" " << sym->Name << L" +0x" << std::hex << disp << std::dec << L"\n";
        }
        else
        {
            out << L" 0x" << std::hex << frame.AddrPC.Offset << std::dec << L"\n";
        }
    }
    SymCleanup(GetCurrentProcess());
    return out.str();
}

static std::wstring SystemInfo()
{
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    MEMORYSTATUSEX ms{sizeof(ms)};
    GlobalMemoryStatusEx(&ms);

    std::wstring gpu = L"Unknown GPU";
    IDXGIFactory* fac = nullptr;
    if (SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&fac)))
    {
        IDXGIAdapter* adp = nullptr;
        if (fac->EnumAdapters(0, &adp) != DXGI_ERROR_NOT_FOUND)
        {
            DXGI_ADAPTER_DESC desc;
            adp->GetDesc(&desc);
            gpu = desc.Description;
            adp->Release();
        }
        fac->Release();
    }

    std::wstringstream s;
    s << L"*** SYSTEM INFO ***\n";
    if (IsWindows10OrGreater())
        s << L"OS Version: Windows 10 or greater\n";
    else if (IsWindows8Point1OrGreater())
        s << L"OS Version: Windows 8.1 or greater\n";
    else
        s << L"OS Version: Windows (version unknown)\n";

    s << L"CPU Cores : " << si.dwNumberOfProcessors << L"\n"
      << L"RAM Total : " << (ms.ullTotalPhys >> 20) << L" MiB\n"
      << L"RAM Avail : " << (ms.ullAvailPhys >> 20) << L" MiB\n"
      << L"GPU : " << gpu << L"\n\n";
    return s.str();
}

static std::wstring ThreadStacks(DWORD skipThreadId)
{
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return L"*** THREAD STACKS ***\nFailed to create snapshot\n";

    std::wstringstream out;
    out << L"*** THREAD STACKS ***\n";

    BYTE symBuffer[sizeof(SYMBOL_INFO) + 256] = {};

    // Never suspend ourselves (instant self-deadlock in NtSuspendThread — the
    // original zombie wedge, this function used to run on the crashing thread
    // and suspend it) nor the crashing thread parked in ThreadStacksBounded's
    // watchdog wait (a suspended waiter can't time out, re-creating the hang).
    // Its stack is already in the report via SymStackTrace(ep).
    const DWORD selfThreadId = GetCurrentThreadId();

    THREADENTRY32 te{sizeof(te)};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
    {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == selfThreadId || te.th32ThreadID == skipThreadId)
            continue;
        HANDLE th = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
        if (!th)
            continue;
        SuspendThread(th);

        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(th, &ctx))
        {
            STACKFRAME64 sf{};
#ifdef _WIN64
            DWORD mach = IMAGE_FILE_MACHINE_AMD64;
            sf.AddrPC.Offset = ctx.Rip;
            sf.AddrFrame.Offset = ctx.Rbp;
            sf.AddrStack.Offset = ctx.Rsp;
#else
            DWORD mach = IMAGE_FILE_MACHINE_I386;
            sf.AddrPC.Offset = ctx.Eip;
            sf.AddrFrame.Offset = ctx.Ebp;
            sf.AddrStack.Offset = ctx.Esp;
#endif
            sf.AddrPC.Mode = sf.AddrFrame.Mode = sf.AddrStack.Mode = AddrModeFlat;
            out << L"\nThread 0x" << std::hex << te.th32ThreadID << std::dec << L"\n";

            SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(symBuffer);
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 255;

            for (int i = 0; i < 32; ++i)
            {
                if (!StackWalk64(mach, GetCurrentProcess(), th, &sf, &ctx, nullptr, SymFunctionTableAccess64,
                                 SymGetModuleBase64, nullptr) ||
                    !sf.AddrPC.Offset)
                    break;
                DWORD64 disp = 0;
                if (SymFromAddr(GetCurrentProcess(), sf.AddrPC.Offset, &disp, sym))
                    out << L" " << sym->Name << L" +0x" << std::hex << disp << std::dec << L"\n";
                else
                    out << L" 0x" << std::hex << sf.AddrPC.Offset << std::dec << L"\n";
            }
        }
        ResumeThread(th);
        CloseHandle(th);
    }
    CloseHandle(snap);
    SymCleanup(GetCurrentProcess());
    return out.str();
}

static bool SaveScreenshot(const std::wstring& file)
{
    auto sc = GetMainSwapChain();
    if (!sc)
        return false;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), &back)))
        return false;

    D3D11_TEXTURE2D_DESC d;
    back->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.BindFlags = d.MiscFlags = 0;

    auto dev = GetD3DDevice();
    auto ctx = GetD3DContext();
    if (!dev || !ctx)
        return false;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> cpu;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &cpu)))
        return false;
    ctx->CopyResource(cpu.Get(), back.Get());

    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(cpu.Get(), 0, D3D11_MAP_READ, 0, &m)))
        return false;

    HRESULT hrCom = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IWICImagingFactory* wic = nullptr;
    IStream* stm = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frm = nullptr;
    bool encoded = false;
    bool screenshotWritten = false;

    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
        SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stm)) &&
        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
        SUCCEEDED(enc->Initialize(stm, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&frm, nullptr)) &&
        SUCCEEDED(frm->Initialize(nullptr)) && SUCCEEDED(frm->SetSize(d.Width, d.Height)))
    {
        WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
        const std::uint64_t pixelBytes = static_cast<std::uint64_t>(m.RowPitch) * d.Height;
        encoded = pixelBytes <= std::numeric_limits<UINT>::max() && SUCCEEDED(frm->SetPixelFormat(&pf)) &&
                  SUCCEEDED(frm->WritePixels(d.Height, m.RowPitch, static_cast<UINT>(pixelBytes),
                                             reinterpret_cast<BYTE*>(m.pData))) &&
                  SUCCEEDED(frm->Commit()) && SUCCEEDED(enc->Commit());
    }

    if (encoded)
    {
        HGLOBAL memory = nullptr;
        STATSTG stats{};
        if (SUCCEEDED(GetHGlobalFromStream(stm, &memory)) && memory && SUCCEEDED(stm->Stat(&stats, STATFLAG_NONAME)) &&
            stats.cbSize.QuadPart > 0 &&
            stats.cbSize.QuadPart <= static_cast<ULONGLONG>(std::numeric_limits<size_t>::max()))
        {
            const size_t encodedBytes = static_cast<size_t>(stats.cbSize.QuadPart);
            const void* data = GlobalLock(memory);
            if (data)
            {
                PinnedFile output = CreateExclusiveOutputFile(WideToUtf8(file));
                if (output.stream)
                {
                    const size_t written = std::fwrite(data, 1, encodedBytes, output.stream);
                    screenshotWritten =
                        written == encodedBytes && std::fflush(output.stream) == 0 && std::ferror(output.stream) == 0;
                    if (!screenshotWritten)
                        OutputDebugStringA("[SPARK ENGINE] Failed to write screenshot safely.\n");
                }
                GlobalUnlock(memory);
            }
        }
    }
    if (frm)
        frm->Release();
    if (enc)
        enc->Release();
    if (stm)
        stm->Release();
    if (wic)
        wic->Release();
    if (SUCCEEDED(hrCom))
        CoUninitialize();
    ctx->Unmap(cpu.Get(), 0);
    return screenshotWritten;
}

static bool ZipFiles(const std::wstring& zip, const std::vector<std::wstring>& files,
                     const std::vector<PinnedFile*>& expectedInputs, PinnedFile* pinnedArchive)
{
    std::string zipUtf = WideToUtf8(zip);
    std::vector<std::string> utf8Files;
    for (const auto& f : files)
        utf8Files.push_back(WideToUtf8(f));
    return ZipFilesUtf8(zipUtf, utf8Files, expectedInputs, pinnedArchive);
}


// ============================================================================
// LINUX / MACOS IMPLEMENTATION (POSIX)
// ============================================================================

#elif defined(SPARK_PLATFORM_LINUX) || defined(SPARK_PLATFORM_MACOS)

static std::string CaptureStackTraceString()
{
    std::ostringstream out;
    out << "*** STACK TRACE ***\n";

    void* buffer[64];
    int nFrames = backtrace(buffer, 64);
    char** symbols = backtrace_symbols(buffer, nFrames);

    if (symbols)
    {
        for (int i = 0; i < nFrames; ++i)
        {
            // Try to demangle C++ names
            std::string sym(symbols[i]);
            // Extract mangled name between '(' and '+'
            size_t begin = sym.find('(');
            size_t end = sym.find('+', begin);
            if (begin != std::string::npos && end != std::string::npos)
            {
                std::string mangled = sym.substr(begin + 1, end - begin - 1);
                int status = 0;
                char* demangled = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &status);
                if (status == 0 && demangled)
                {
                    out << "  " << demangled << " " << sym.substr(end) << "\n";
                    free(demangled);
                }
                else
                {
                    out << "  " << sym << "\n";
                }
            }
            else
            {
                out << "  " << sym << "\n";
            }
        }
        free(symbols);
    }
    return out.str();
}

static std::string LinuxSystemInfo()
{
    std::ostringstream s;
    s << "*** SYSTEM INFO ***\n";

    struct utsname un;
    if (uname(&un) == 0)
    {
        s << "OS: " << un.sysname << " " << un.release << " " << un.machine << "\n";
    }

#ifdef SPARK_PLATFORM_LINUX
    struct sysinfo si;
    if (sysinfo(&si) == 0)
    {
        long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
        s << "CPU Cores : " << nprocs << "\n";
        s << "RAM Total : " << (si.totalram * si.mem_unit >> 20) << " MiB\n";
        s << "RAM Avail : " << (si.freeram * si.mem_unit >> 20) << " MiB\n";
        s << "Uptime    : " << si.uptime << " seconds\n";
    }
#elif defined(SPARK_PLATFORM_MACOS)
    {
        long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
        s << "CPU Cores : " << nprocs << "\n";
        int64_t memSize = 0;
        size_t len = sizeof(memSize);
        if (sysctlbyname("hw.memsize", &memSize, &len, nullptr, 0) == 0)
            s << "RAM Total : " << (memSize >> 20) << " MiB\n";
        mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
        vm_statistics64_data_t vmstat;
        if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vmstat), &count) ==
            KERN_SUCCESS)
        {
            int64_t pageSize = sysconf(_SC_PAGESIZE);
            int64_t freePages = vmstat.free_count + vmstat.inactive_count;
            s << "RAM Avail : " << (freePages * pageSize >> 20) << " MiB\n";
        }
    }
#endif

    // Try to read GPU info from /proc/driver/nvidia or lspci (Linux only)
    std::ifstream gpuFile("/proc/driver/nvidia/gpus/0/information");
    if (gpuFile.is_open())
    {
        std::string line;
        while (std::getline(gpuFile, line))
        {
            if (line.contains("Model:"))
            {
                s << "GPU: " << line << "\n";
                break;
            }
        }
    }
    else
    {
        s << "GPU: (use lspci for GPU info)\n";
    }

    // Process memory
    std::ifstream statusFile("/proc/self/status");
    if (statusFile.is_open())
    {
        std::string line;
        s << "\n*** PROCESS MEMORY ***\n";
        while (std::getline(statusFile, line))
        {
            if (line.find("VmRSS:") == 0 || line.find("VmPeak:") == 0 || line.find("VmSize:") == 0 ||
                line.find("Threads:") == 0)
            {
                s << line << "\n";
            }
        }
    }

    s << "\n";
    return s.str();
}

static std::string LinuxThreadStacks()
{
    std::ostringstream out;
    out << "*** THREAD STACKS ***\n";

    // Read /proc/self/task to enumerate threads
    std::string taskDir = "/proc/self/task";
    if (std::filesystem::exists(taskDir))
    {
        for (const auto& entry : std::filesystem::directory_iterator(taskDir))
        {
            std::string tid = entry.path().filename().string();
            out << "\nThread " << tid << ":\n";

            // Read thread status
            std::ifstream commFile(entry.path().string() + "/comm");
            if (commFile.is_open())
            {
                std::string name;
                std::getline(commFile, name);
                out << "  Name: " << name << "\n";
            }
        }
    }

    return out.str();
}

// Async-signal-safe helper: write a C string to stderr.
static void WriteStderr(const char* s)
{
    if (s)
    {
        size_t len = 0;
        while (s[len])
            ++len;
        (void)write(STDERR_FILENO, s, len);
    }
}

static void HandleLinuxCrash(int sig, siginfo_t* info, void* context)
{
    // Prevent re-entrant crashes (e.g. crash inside the handler itself).
    // sig_atomic_t is the only type guaranteed safe in signal handlers.
    if (g_inSignalHandler)
    {
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    g_inSignalHandler = 1;

    // --- Async-signal-safe section ---
    // Write a minimal crash notice to stderr using only write(), which is
    // async-signal-safe.  This guarantees visible output even if the heap
    // or iostream locks are corrupted.
    const char* sigName = "UNKNOWN";
    switch (sig)
    {
    case SIGSEGV:
        sigName = "SIGSEGV (Segmentation fault)";
        break;
    case SIGFPE:
        sigName = "SIGFPE (Floating point exception)";
        break;
    case SIGABRT:
        sigName = "SIGABRT (Abort)";
        break;
    case SIGBUS:
        sigName = "SIGBUS (Bus error)";
        break;
    case SIGILL:
        sigName = "SIGILL (Illegal instruction)";
        break;
    case SIGTRAP:
        sigName = "SIGTRAP (Trace/breakpoint trap)";
        break;
    }
    WriteStderr("\n[SPARK ENGINE] CRASH: ");
    WriteStderr(sigName);
    WriteStderr("\n");

    // --- Best-effort section ---
    // The operations below use the heap and are technically not
    // async-signal-safe.  However, they are wrapped in a try/catch and
    // the process is about to terminate anyway, so this is acceptable
    // best-effort behaviour (matching industry-standard crash reporters).
    // We intentionally do NOT acquire g_lock here because the crashing
    // thread may already hold it, which would deadlock.
    try
    {
        const std::string reportId = MakeCrashReportId();
        const std::string timestamp = MakeTimeStampUtf8();
        const std::string artifactSuffix = "_" + reportId;
        std::string prefix = GetCrashArtifactPrefix().string();
        if (prefix.empty())
        {
            WriteStderr("[SPARK ENGINE] Private crash-artifact directory unavailable; report not written.\n");
            signal(sig, SIG_DFL);
            raise(sig);
            return;
        }
        std::string logFile = prefix + artifactSuffix + ".log";
        std::string zipFile = prefix + artifactSuffix + ".zip";

        std::ostringstream log;
        log << "================================================================\n";
        log << "           SPARK ENGINE CRASH REPORT (POSIX)\n";
        log << "================================================================\n\n";
        log << "Timestamp  : " << timestamp << "\n";
        log << "Process ID : " << getpid() << "\n";
#ifdef SPARK_PLATFORM_LINUX
        log << "Thread ID  : " << static_cast<unsigned long>(syscall(SYS_gettid)) << "\n\n";
#else
        log << "Thread ID  : " << pthread_mach_thread_np(pthread_self()) << "\n\n";
#endif

        log << "*** CRASH DETECTED ***\n";
        log << "Signal     : " << sig << " - " << sigName << "\n";
        if (info)
        {
            log << "Fault Addr : 0x" << std::hex << reinterpret_cast<uintptr_t>(info->si_addr) << std::dec << "\n";
            log << "Signal Code: " << info->si_code << "\n";
        }
        log << "\n";

        log << CaptureStackTraceString();
        if (g_cfg.captureSystemInfo)
            log << LinuxSystemInfo();
        if (g_cfg.captureAllThreads)
            log << LinuxThreadStacks();

        std::string logStr = log.str();
        const bool logReady = WriteExclusiveFileUtf8(logFile, logStr);

        // Write core dump path hint
        std::string coreFile = prefix + artifactSuffix + ".core_hint";
        static const char coreHint[] = "Core dump may be found at the system core dump location.\n"
                                       "Check: /proc/sys/kernel/core_pattern\n"
                                       "Or run: coredumpctl list\n";
        const bool coreReady = WriteExclusiveFileUtf8(coreFile, std::string(coreHint, sizeof(coreHint) - 1));
        PinnedFile coreProbe = coreReady ? OpenPinnedInputFile(coreFile) : PinnedFile{};
        PinnedFile logProbe = logReady ? OpenPinnedInputFile(logFile) : PinnedFile{};

        const std::string crashTitle = (sig == SIGSEGV)   ? "SIGSEGV"
                                       : (sig == SIGABRT) ? "SIGABRT"
                                       : (sig == SIGFPE)  ? "SIGFPE"
                                                          : "Crash";

        // Reporter launch success is the ownership switch. Emit only raw
        // artifacts and never perform the in-process consent/archive/upload path.
        if (g_reporterLaunched)
        {
            if (logProbe.stream)
                WriteCrashManifest(reportId, coreProbe.stream ? coreFile : std::string{}, logFile, "", "", crashTitle);
        }

        // Reporter-unavailable fallback: this process alone owns consent,
        // archive construction, and upload.
        if (!g_reporterLaunched && g_cfg.enableCrashReporting)
        {
            bool userConsented = true;
            if (g_cfg.requireConsent && !g_cfg.headlessMode)
            {
                int result = MessageBoxA(nullptr,
                                         "SparkEngine has crashed. Would you like to send a crash report "
                                         "to help improve the engine?\n\nThe report can include stack traces, "
                                         "system and process information, and file paths. These diagnostics "
                                         "may contain personal or sensitive data.",
                                         "Crash Report", MB_YESNO | MB_ICONERROR);
                userConsented = (result == IDYES);
            }

            if (userConsented)
            {
                bool archiveReady = !g_cfg.zipBeforeUpload;
                PinnedFile archivePin;
                if (g_cfg.zipBeforeUpload)
                {
                    const std::vector<std::string> approvedFiles =
                        Spark::CrashHandlerDetail::BuildCrashArchiveAllowlist(
                            coreProbe.stream ? coreFile : std::string{}, logProbe.stream ? logFile : std::string{},
                            std::string{}, false);
                    std::vector<PinnedFile*> approvedPins;
                    if (coreProbe.stream)
                        approvedPins.push_back(&coreProbe);
                    if (logProbe.stream)
                        approvedPins.push_back(&logProbe);
                    archiveReady = ZipFilesUtf8(zipFile, approvedFiles, approvedPins, &archivePin);
                }
                CrashConfig uploadCfg = g_cfg;
                const std::string approvedArchive =
                    g_cfg.zipBeforeUpload && archiveReady ? StableUploadPath(archivePin, zipFile) : std::string{};
                bool uploadOk = UploadCrashReport(uploadCfg, log.str(), approvedArchive);
                if (uploadOk)
                    WriteStderr("Crash report uploaded successfully.\n");
                else
                    WriteStderr("Crash report upload failed.\n");
            }
        }

        // Print log to stderr
        WriteStderr("Log file: ");
        WriteStderr(logFile.c_str());
        WriteStderr("\n");
    }
    catch (const std::exception& e)
    {
        WriteStderr("[SPARK ENGINE] Exception: ");
        WriteStderr(e.what());
        WriteStderr("\n");
    }
    catch (...)
    {
        WriteStderr("[SPARK ENGINE] Unknown exception in crash handler\n");
    }

    // Re-raise to get core dump
    signal(sig, SIG_DFL);
    raise(sig);
}

void InstallCrashHandler(const CrashConfig& cfg)
{
    SPARK_LOG_INFO(Spark::LogCategory::Core, "Installing crash handler (Linux)");
    AssignCrashConfig(cfg);
    g_triggerCrashOnAssert = cfg.triggerCrashOnAssert;
    g_reporterLaunched = false;
    g_manifestDir.clear();
    ResetPinnedArtifactRoot();
    if (!InitializeCrashArtifactDirectory())
    {
        SPARK_LOG_ERROR(
            Spark::LogCategory::Core,
            "CrashHandler: failed to create private crash-artifact directory; filesystem artifacts disabled");
    }

#ifdef SPARK_HAS_CURL
    curl_global_init(CURL_GLOBAL_DEFAULT);
#endif

    // Install signal handlers for common crash signals
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = HandleLinuxCrash;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND; // SA_RESETHAND to avoid infinite loops
    sigemptyset(&sa.sa_mask);

    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGTRAP, &sa, nullptr);

    // The external reporter is intentionally read-only. Keep consent and upload
    // ownership in-process whenever reporting is enabled, and never show UI headlessly.
    if (Spark::CrashHandlerDetail::ShouldLaunchReadOnlyReporter(g_cfg.enableCrashReporting, g_cfg.headlessMode))
        g_reporterLaunched = LaunchCrashReporter();
}

void TriggerCrashHandler(const char* assertMsg)
{
    if (!g_triggerCrashOnAssert)
    {
        std::string logMsg = "Assert triggered but crash handling disabled: ";
        if (assertMsg)
            logMsg += assertMsg;
        try
        {
            Spark::ConsoleProcessManager::GetInstance().LogCrash(logMsg);
        }
        catch (const std::exception& e)
        {
            WriteStderr("[SPARK ENGINE] Exception: ");
            WriteStderr(e.what());
            WriteStderr("\n");
        }
        catch (...)
        {
            WriteStderr("[SPARK ENGINE] Unknown exception in crash handler\n");
        }
        return;
    }

    // Generate crash report from assert
    const std::string reportId = MakeCrashReportId();
    const std::string timestamp = MakeTimeStampUtf8();
    const std::string artifactSuffix = "_" + reportId;
    std::string prefix = GetCrashArtifactPrefix().string();
    if (prefix.empty())
    {
        WriteStderr("[SPARK ENGINE] Private crash-artifact directory unavailable; assertion report not written.\n");
        return;
    }
    std::string logFile = prefix + artifactSuffix + "_assert.log";

    std::ostringstream log;
    log << "================================================================\n";
    log << "           SPARK ENGINE ASSERTION FAILURE (Linux)\n";
    log << "================================================================\n\n";
    log << "Timestamp  : " << timestamp << "\n";
    log << "Process ID : " << getpid() << "\n\n";

    if (assertMsg)
    {
        log << "*** ASSERTION FAILURE ***\n" << assertMsg << "\n\n";
    }

    log << CaptureStackTraceString();
    if (g_cfg.captureSystemInfo)
        log << LinuxSystemInfo();

    const bool logReady = WriteExclusiveFileUtf8(logFile, log.str());
    if (!logReady)
        WriteStderr("[SPARK ENGINE] Failed to write assertion report safely.\n");
    else if (g_reporterLaunched)
        WriteCrashManifest(reportId, "", logFile, "", "", "Assertion Failure");

    try
    {
        Spark::ConsoleProcessManager::GetInstance().LogCrash(log.str().substr(0, 1000));
    }
    catch (const std::exception& e)
    {
        WriteStderr("[SPARK ENGINE] Exception: ");
        WriteStderr(e.what());
        WriteStderr("\n");
    }
    catch (...)
    {
        WriteStderr("[SPARK ENGINE] Unknown exception in crash handler\n");
    }

    std::cerr << "\n[SPARK ENGINE] ASSERTION FAILURE\n" << log.str() << "\n";
}

void SetAssertCrashBehavior(bool shouldCrash)
{
    std::lock_guard<std::mutex> lock(g_lock);
    g_triggerCrashOnAssert = shouldCrash;
    try
    {
        std::string logMsg = "Assert crash behavior changed to: ";
        logMsg += (shouldCrash ? "ENABLED" : "DISABLED");
        Spark::ConsoleProcessManager::GetInstance().LogCrash(logMsg);
    }
    catch (const std::exception& e)
    {
        WriteStderr("[SPARK ENGINE] Exception: ");
        WriteStderr(e.what());
        WriteStderr("\n");
    }
    catch (...)
    {
        WriteStderr("[SPARK ENGINE] Unknown exception in crash handler\n");
    }
}

#else
// Unsupported platform stubs
void InstallCrashHandler(const CrashConfig& cfg)
{
    SPARK_LOG_INFO(Spark::LogCategory::Core, "Installing crash handler (stub - unsupported platform)");
    AssignCrashConfig(cfg);
    g_triggerCrashOnAssert = cfg.triggerCrashOnAssert;
}

void TriggerCrashHandler(const char* assertMsg)
{
    if (assertMsg)
        std::cerr << "Assert: " << assertMsg << "\n";
}

void SetAssertCrashBehavior(bool shouldCrash)
{
    g_triggerCrashOnAssert = shouldCrash;
}
#endif
