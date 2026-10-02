#include "Utils/CrashHandler.h"
#include "../Core/Platform.h"
#include "../Core/RuntimePackage.h"
#include "Utils/CrashArtifactRetention.h"
#include "Utils/CrashHandlerSupport.h"
#include "Utils/CrashSymbolication.h"
#include "Utils/Assert.h"
#include "Utils/Process.h"
#include "Utils/SparkError.h"
#include "Utils/ConsoleProcessManager.h"
#include "Utils/StackTrace.h"
#include "Validate.h"

#include <algorithm>
#include <array>
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
#include "MiniDumpWithoutStacks.h"
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
#include <cerrno>
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
#include <elf.h>
#include <link.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/uio.h>
#include <ucontext.h>
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
#ifdef SPARK_PLATFORM_LINUX
// The producer owns this private queue; reserve one of its 32 slots for a fatal signal.
static bool g_reserveFatalManifest = false;
static pid_t g_signalArtifactOwnerPid = 0;
#endif
#if defined(SPARK_PLATFORM_LINUX) || defined(SPARK_PLATFORM_MACOS)
static std::atomic_flag g_inSignalHandler = ATOMIC_FLAG_INIT;
static volatile sig_atomic_t g_posixCoreDumpPolicyEnforced = 1;
#endif

// Every faulting-thread frame line the stack-trace producers write starts with
// this marker, so a reader of the crash log can tell the faulting stack apart
// from the unmarked per-thread stacks written by ThreadStacks().
static constexpr const char* kStackFrameMarker = "FRAME ";
static constexpr const wchar_t* kStackFrameMarkerW = L"FRAME ";

static void AssignCrashConfig(const CrashConfig& cfg)
{
    g_cfg = cfg;
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
    struct stat info
    {
    };
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
    static const auto ntCreateFile = []() -> NtCreateFileFn
    {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll)
            return nullptr;
        return reinterpret_cast<NtCreateFileFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "NtCreateFile")));
    }();
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

    struct stat info
    {
    };
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
    struct stat info
    {
    };
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

/// One crash report per process.
///
/// TriggerCrashHandler(), TriggerCrashReport(), TriggerCrashReportUnattended()
/// and the unhandled-exception filter all funnel into one report writer, and a
/// single fatal assertion reaches it through two of them (Assert::Fail calls the
/// gated entry point and then the ungated one) — which produced two minidumps,
/// two archives, two upload attempts and six modal dialogs for one failure.
///
/// Exchanged BEFORE the report lock is taken, so a fault raised inside the crash
/// path returns immediately instead of self-deadlocking on the non-recursive
/// g_lock.
static std::atomic<bool> g_crashReported{false};

/// How a report leaves this process.
enum class CrashReportDelivery
{
    Interactive, ///< May capture a screenshot and hand off to the read-only reporter
    ArtifactOnly ///< Dump/log/manifest only: no dialogs, no screenshot, no upload
};

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
    const std::filesystem::path baseDirectory = Spark::CrashHandlerDetail::ResolveCrashArtifactBaseDirectory();
    if (baseDirectory.empty())
    {
        return false;
    }

    // Each process leaves one directory behind. Before adding this one, drop
    // the ones earlier, exited processes left empty or past the retention
    // limits, so launches and crashes cannot accumulate artifacts unbounded.
    const std::size_t pruned = Spark::CrashHandlerDetail::PruneStaleCrashArtifactDirectories(baseDirectory);
    if (pruned != 0)
        SPARK_LOG_INFO(Spark::LogCategory::Core, "CrashHandler: removed %zu stale crash-artifact director%s", pruned,
                       pruned == 1 ? "y" : "ies");

    const std::filesystem::path artifactDirectory =
        Spark::CrashHandlerDetail::CreatePrivateCrashArtifactDirectory(baseDirectory, GetEnginePID());
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
    // dup shares the pinned directory offset; every capacity check starts at the beginning.
    rewinddir(directory);
    bool scanSucceeded = true;
    while (count < Spark::CrashHandlerDetail::kMaxPendingCrashManifests)
    {
        errno = 0;
        const dirent* entry = readdir(directory);
        if (!entry)
        {
            scanSucceeded = errno == 0;
            break;
        }
        if (Spark::CrashHandlerDetail::IsCrashManifestReadyName(entry->d_name))
            ++count;
    }
    const bool directoryClosed = closedir(directory) == 0;
    count = Spark::CrashHandlerDetail::CrashManifestCountOrFull(count, scanSucceeded && directoryClosed);
#endif
    return count;
}

static bool PublishCrashManifest(std::string_view reportId, const std::string& json)
{
    const std::string readyName = Spark::CrashHandlerDetail::CrashManifestReadyName(reportId);
    const size_t pending = CountPendingCrashManifests();
#ifdef SPARK_PLATFORM_LINUX
    const bool capacity = Spark::CrashHandlerDetail::HasNonfatalCrashManifestCapacity(pending, g_reserveFatalManifest);
#else
    const bool capacity = Spark::CrashHandlerDetail::HasCrashManifestQueueCapacity(pending);
#endif
    if (readyName.empty() || !capacity)
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
    // Keep the destination relative to the pinned root. The Win32 rename
    // wrapper rejects this RootDirectory form with ERROR_INVALID_PARAMETER;
    // the native operation preserves the same authority used by NtCreateFile.
    using NtSetInformationFileFn = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
    static const auto ntSetInformationFile = []() -> NtSetInformationFileFn
    {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll ? reinterpret_cast<NtSetInformationFileFn>(GetProcAddress(ntdll, "NtSetInformationFile"))
                     : nullptr;
    }();
    IO_STATUS_BLOCK ioStatus{};
    // FileRenameInformation is class 10; the user-mode SDK enum omits its name.
    constexpr auto renameInformationClass = static_cast<FILE_INFORMATION_CLASS>(10);
    const bool published = ntSetInformationFile &&
                           ntSetInformationFile(temporary, &ioStatus, renameInfo,
                                                static_cast<ULONG>(renameStorage.size()), renameInformationClass) >= 0;
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
    // True only under the SPARK_CRASH_FULL_DUMP opt-in, whose dumps carry thread-stack memory; the
    // reporter's consent text then discloses that the dump can contain data held in memory.
    j << "  \"fullMemoryDump\": " << (g_cfg.includeStackMemory ? "true" : "false") << "\n";
    j << "}\n";
    return j.str();
}

static bool WriteCrashManifest(std::string_view reportId, const std::string& dumpFile, const std::string& logFile,
                               const std::string& screenshotFile, const std::string& zipFile,
                               const std::string& crashTitle)
{
    if (g_manifestDir.empty())
        return false;

    // The pinned root is the authority; persist only its immediate child names.
    // Keep absolute-path compatibility in the reader for older manifests.
    const auto leafName = [](const std::string& path, std::string& output)
    {
        if (path.empty())
            return true;
        std::filesystem::path name;
        if (!ArtifactNameInPinnedRoot(path, name))
            return false;
        output = Spark::CrashHandlerDetail::PathToUtf8(name);
        return !output.empty();
    };
    std::string dumpName, logName, screenshotName, zipName;
    if (logFile.empty() || !leafName(dumpFile, dumpName) || !leafName(logFile, logName) ||
        !leafName(screenshotFile, screenshotName) || !leafName(zipFile, zipName))
        return false;

    std::string json = MakeManifestJson(dumpName, logName, screenshotName, zipName, crashTitle);
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
                        "CrashHandler: SparkCrashReporter not found, keeping crash artifacts local");
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
static void HandleCrashInternal(EXCEPTION_POINTERS* ep, const char* assertMsg, CrashReportDelivery delivery);
static bool WriteMiniDump(const std::wstring& path, EXCEPTION_POINTERS* ep, DWORD* failureError = nullptr);
static std::wstring MakeTimeStamp();
static std::wstring SymStackTrace(EXCEPTION_POINTERS* ep);
static std::wstring SystemInfo();
static std::wstring ThreadStacks(DWORD skipThreadId);
static std::wstring ThreadStacksBounded(bool& outTimedOut);
static bool SaveScreenshot(const std::wstring& file);

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

    // Teardown detection (see block comment above): resolve the ntdll probe up
    // front so the crash path never calls GetProcAddress, and set our own flag
    // as soon as normal exit processing starts.
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll)
    {
        g_rtlDllShutdownInProgress = reinterpret_cast<RtlDllShutdownInProgressFn>(
            reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlDllShutdownInProgress")));
    }
    else
    {
        g_rtlDllShutdownInProgress = nullptr;
        SPARK_LOG_WARN(Spark::LogCategory::Core,
                       "CrashHandler: ntdll.dll module handle unavailable; RtlDllShutdownInProgress teardown probe "
                       "disabled");
    }
    std::atexit([] { g_processTeardown.store(true, std::memory_order_relaxed); });

    SetUnhandledExceptionFilter(CrashFilter);

    // The external reporter is intentionally read-only. Launch it for every
    // interactive process; headless processes keep artifacts local and never
    // start a UI.
    if (!g_cfg.headlessMode)
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

    TriggerCrashReport(assertMsg);
}

static void TriggerCrashReportWithDelivery(const char* reason, CrashReportDelivery delivery)
{
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
    HandleCrashInternal(&ep, reason, delivery);
}

void TriggerCrashReport(const char* reason)
{
    TriggerCrashReportWithDelivery(reason, CrashReportDelivery::Interactive);
}

void TriggerCrashReportUnattended(const char* reason)
{
    TriggerCrashReportWithDelivery(reason, CrashReportDelivery::ArtifactOnly);
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

void RefreshCrashModuleIdentities()
{
    // Minidumps record the loaded-module list (with PDB identity) themselves.
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep)
{
    HandleCrashInternal(ep, nullptr, CrashReportDelivery::Interactive);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void HandleCrashInternal(EXCEPTION_POINTERS* ep, const char* assertMsg, CrashReportDelivery delivery)
{
    // Before g_lock: see g_crashReported. A second report for the same failure
    // is duplicate noise; a report raised from inside this function would wedge
    // on the non-recursive lock below.
    if (g_crashReported.exchange(true, std::memory_order_acq_rel))
    {
        OutputDebugStringA("[SPARK ENGINE] Crash report already written for this process; duplicate ignored.\n");
        return;
    }

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

    DWORD dumpError = ERROR_SUCCESS;
    const bool dumpReady = WriteMiniDump(dump, ep, &dumpError);

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
        // The -1 conversion includes a terminator; keep it out of the text log.
        log << L"*** ASSERTION FAILURE ***\n" << wmsg.c_str() << L"\n\n";
    }
    else
    {
        log << L"*** CRASH DETECTED ***\n\n";
    }

    // Keep a bounded, path-free reason in the producer-owned log when the
    // Windows dump API rejects the request.  The manifest intentionally
    // carries an empty dumpFile in that case; this diagnostic lets the
    // isolated security test distinguish an OS/API failure from a packaging
    // or probe failure without exposing the artifact path.
    if (!dumpReady)
    {
        log << L"Minidump capture failed (Win32=" << dumpError << L", HRESULT=0x" << std::hex
            << static_cast<unsigned long>(HRESULT_FROM_WIN32(dumpError)) << std::dec << L")\n\n";
    }

    if (ep->ExceptionRecord)
    {
        DWORD code = ep->ExceptionRecord->ExceptionCode;
        const char* codeName = SparkError::ExceptionCodeToString(code);
        int codeNameLen = MultiByteToWideChar(CP_UTF8, 0, codeName, -1, nullptr, 0);
        std::wstring wCodeName(codeNameLen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, codeName, -1, &wCodeName[0], codeNameLen);

        log << L"Exception Code    : 0x" << std::hex << code << std::dec << L"\n";
        log << L"Exception Name    : " << wCodeName.c_str() << L"\n";
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

    // Probe the writer's result before the log is finalized.  A successful
    // MiniDumpWriteDump call can still leave an unusable artifact if the
    // pinned-root consumer cannot reopen it; keep that failure distinct from
    // the API failure above while preserving the fail-closed empty manifest
    // field.
    PinnedFile dumpProbe = dumpReady ? OpenPinnedInputFile(WideToUtf8(dump)) : PinnedFile{};
    if (dumpReady && !dumpProbe.stream)
        log << L"Minidump probe failed after writer reported success\n\n";

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
        // screenshot (render thread) and dialog
        // pumps below could all deadlock the same way. The process dump and log
        // are already on disk — publish the local manifest and die hard instead
        // of zombieing (the historical failure mode this watchdog exists for).
        if (logReady)
        {
            WriteCrashManifest(reportId, dumpProbe.stream ? WideToUtf8(dump) : std::string{}, WideToUtf8(logFile), "",
                               "",
                               assertMsg ? "Assertion Failure (thread-stack capture timed out)"
                                         : "Crash Detected (thread-stack capture timed out)");
        }
        TerminateProcess(GetCurrentProcess(), ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode
                                                                  : static_cast<DWORD>(STATUS_FATAL_APP_EXIT));
    }

    // No screenshot in ArtifactOnly mode: the only caller is the freeze watchdog,
    // and presenting the swap chain from a watchdog thread while the render
    // thread is hung is exactly the wedge this report is about.
    const bool screenshotWritten =
        delivery == CrashReportDelivery::Interactive && g_cfg.captureScreenshot && SaveScreenshot(shot);

    PinnedFile logProbe = logReady ? OpenPinnedInputFile(WideToUtf8(logFile)) : PinnedFile{};
    PinnedFile screenshotProbe = screenshotWritten ? OpenPinnedInputFile(WideToUtf8(shot)) : PinnedFile{};
    const bool screenshotAvailable = screenshotProbe.stream != nullptr;

    // Always publish the local manifest when the log is safely pinned. The
    // read-only reporter owns interactive review when it is running; the
    // artifact-only watchdog path returns without UI, and a failed reporter
    // launch falls through to the generic local-capture notice below.
    if (logProbe.stream)
    {
        const std::string crashTitle = delivery == CrashReportDelivery::ArtifactOnly
                                           ? "Unattended Failure (watchdog)"
                                           : (assertMsg ? "Assertion Failure" : "Crash Detected");
        WriteCrashManifest(reportId, dumpProbe.stream ? WideToUtf8(dump) : std::string{}, WideToUtf8(logFile),
                           screenshotAvailable ? WideToUtf8(shot) : std::string{}, "", crashTitle);
    }
    if (g_reporterLaunched || delivery == CrashReportDelivery::ArtifactOnly)
        return;

    if (!g_cfg.headlessMode)
    {
        std::wstring msg = assertMsg ? L"Assertion captured.\n" : L"Crash captured.\n";
        msg += L"Files:\n" + dump + L"\n" + logFile;
        MessageBoxW(nullptr, msg.c_str(), assertMsg ? L"Assertion Handler" : L"Crash Handler", MB_OK | MB_ICONERROR);
    }
}

static bool WriteMiniDump(const std::wstring& file, EXCEPTION_POINTERS* ep, DWORD* failureError)
{
    if (failureError)
        *failureError = ERROR_SUCCESS;

    // MiniDumpWriteDump shares DbgHelp's process-wide state with StackTrace.
    // Never race a concurrent symbol lookup or block indefinitely when the
    // faulting thread already owns the symbol lock.
    Spark::StackTrace::SymbolLockLease symbolLock(true);
    if (!symbolLock.owns_lock())
    {
        if (failureError)
            *failureError = ERROR_BUSY;
        return false;
    }

    HANDLE h =
        CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        if (failureError)
            *failureError = GetLastError();
        return false;
    }

    MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), ep, TRUE};
    constexpr MINIDUMP_TYPE dumpType = MiniDumpNormal;
    BOOL result = FALSE;
    DWORD error = ERROR_SUCCESS;
    // A minidump can race a transiently inaccessible page on a live process.
    // Retry that one DbgHelp error once using the same private file; no failed
    // or partial dump is ever advertised in the manifest.
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        if (attempt != 0)
        {
            // Give whatever made the page transiently unreadable (an exiting thread) time to finish.
            Sleep(50);
            LARGE_INTEGER start{};
            if (!SetFilePointerEx(h, start, nullptr, FILE_BEGIN) || !SetEndOfFile(h))
            {
                error = GetLastError();
                break;
            }
        }
        // OPS-100: no thread-stack memory unless the SPARK_CRASH_FULL_DUMP opt-in asked for it.
        // Either way DbgHelp runs on a dedicated writer thread, never on this (faulting) one.
        if (g_cfg.includeStackMemory)
        {
            result = Spark::CrashDump::WriteOnWriterThread(h, dumpType, &info, false,
                                                           Spark::CrashDump::kWriterStartTimeoutMs, &error);
        }
        else
        {
            result = Spark::CrashDump::WriteWithoutStacks(h, dumpType, &info, &error);
        }
        if (result ||
            (error != ERROR_PARTIAL_COPY && error != static_cast<DWORD>(HRESULT_FROM_WIN32(ERROR_PARTIAL_COPY))))
            break;
    }
    CloseHandle(h);
    if (failureError)
        *failureError = error;
    return result != FALSE;
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
    // Never block on the shared DbgHelp lock here: fall back to unsymbolized
    // frames instead — the raw addresses still resolve offline against the
    // minidump. The lease also rejects same-thread DbgHelp reentry.
    Spark::StackTrace::SymbolLockLease symbolLock(true);

    std::wstringstream out;
    if (!symbolLock.owns_lock())
    {
        // Deliberately unsymbolized: something else is inside DbgHelp and every
        // Sym*/StackWalk64 call here would race it. CaptureStackBackTrace
        // touches no DbgHelp state, and this filter runs on the faulting thread.
        out << L"*** STACK TRACE (unsymbolized: DbgHelp busy — resolve against the dump) ***\n";
        void* frames[32] = {};
        const USHORT captured = CaptureStackBackTrace(0, 32, frames, nullptr);
        for (USHORT i = 0; i < captured; ++i)
        {
            out << L"  " << kStackFrameMarkerW << L"0x" << std::hex << reinterpret_cast<uintptr_t>(frames[i])
                << std::dec << L"\n";
        }
        if (captured == 0 && ep->ExceptionRecord)
        {
            out << L"  " << kStackFrameMarkerW << L"0x" << std::hex
                << reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress) << std::dec << L"\n";
        }
        return out.str();
    }

    Spark::StackTrace::EnsureSymbolsInitialized();
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
            out << L"  " << kStackFrameMarkerW << sym->Name << L" +0x" << std::hex << disp << std::dec << L"\n";
        }
        else
        {
            out << L"  " << kStackFrameMarkerW << L"0x" << std::hex << frame.AddrPC.Offset << std::dec << L"\n";
        }
    }
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
    // Same shared DbgHelp lock as SymStackTrace, and the same refusal to block
    // on it. This runs on ThreadStacksBounded's helper thread, so a wedge here
    // does not hang the process — but the crashing thread is the likeliest
    // holder of the lock (it is parked in ThreadStacksBounded's wait), and
    // blocking would burn the full 5 s timeout and then force the truncated
    // "capture timed out" path that terminates the process early. There is no
    // unsymbolized fallback worth writing: StackWalk64 is DbgHelp too.
    //
    // These lines deliberately carry no frame marker: only the faulting thread's
    // stack feeds the crash hash.
    Spark::StackTrace::SymbolLockLease symbolLock(true);
    if (!symbolLock.owns_lock())
    {
        return L"*** THREAD STACKS ***\nSkipped: DbgHelp busy (symbol lock held elsewhere) — "
               L"resolve the other threads against the dump\n";
    }

    Spark::StackTrace::EnsureSymbolsInitialized();
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
                    out << "  " << kStackFrameMarker << demangled << " " << sym.substr(end) << "\n";
                    free(demangled);
                }
                else
                {
                    out << "  " << kStackFrameMarker << sym << "\n";
                }
            }
            else
            {
                out << "  " << kStackFrameMarker << sym << "\n";
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

#ifdef SPARK_PLATFORM_LINUX
// ----------------------------------------------------------------------------
// Build-id module identity for offline symbolication (tools/ops/symbolicate_crash.py)
// ----------------------------------------------------------------------------

namespace
{
    using Spark::CrashHandlerDetail::CrashModuleIdentity;

    /// Module identity captured outside the signal handler. Two tables, so a
    /// refresh never rewrites the one a crashing thread may be reading.
    struct CrashModuleTable
    {
        size_t count = 0;
        std::array<CrashModuleIdentity, Spark::CrashHandlerDetail::kMaxCrashModules> modules{};
    };

    CrashModuleTable g_crashModuleTables[2];
    std::atomic<int> g_activeCrashModuleTable{-1};
    static_assert(std::atomic<int>::is_always_lock_free, "signal-path module table selection must be lock-free");
    std::mutex g_crashModuleRefreshLock;

    /// Signal-path output buffer; sized for kMaxSymbolicCrashFrames frames and
    /// one module record per frame, so the section is never truncated.
    char g_symbolicSectionBuffer[32 * 1024];

    struct CrashModuleScan
    {
        CrashModuleTable* table = nullptr;
        const char* mainProgramName = nullptr;
    };

    /// Copy a module basename, keeping the record a single whitespace-free token.
    void CopyCrashModuleName(const char* path, CrashModuleIdentity& module)
    {
        const char* base = (path && *path) ? std::strrchr(path, '/') : nullptr;
        base = base ? base + 1 : ((path && *path) ? path : "unknown");
        size_t length = 0;
        for (; base[length] != '\0' && length + 1 < module.name.size(); ++length)
        {
            const char character = base[length];
            const bool safe = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                              (character >= '0' && character <= '9') || character == '.' || character == '_' ||
                              character == '-' || character == '+';
            module.name[length] = safe ? character : '_';
        }
        module.name[length] = '\0';
    }

    int CollectCrashModuleIdentity(dl_phdr_info* info, size_t /*size*/, void* userData)
    {
        auto& scan = *static_cast<CrashModuleScan*>(userData);
        CrashModuleTable& table = *scan.table;
        if (table.count >= table.modules.size())
            return 1;

        CrashModuleIdentity& module = table.modules[table.count];
        module = CrashModuleIdentity{};
        module.loadBias = static_cast<std::uintptr_t>(info->dlpi_addr);
        std::uintptr_t lowest = std::numeric_limits<std::uintptr_t>::max();
        std::uintptr_t highest = 0;
        for (ElfW(Half) index = 0; index < info->dlpi_phnum; ++index)
        {
            const ElfW(Phdr)& header = info->dlpi_phdr[index];
            const std::uintptr_t runtimeAddress = module.loadBias + static_cast<std::uintptr_t>(header.p_vaddr);
            if (header.p_type == PT_LOAD && header.p_memsz > 0)
            {
                lowest = std::min(lowest, runtimeAddress);
                highest = std::max(highest, runtimeAddress + static_cast<std::uintptr_t>(header.p_memsz));
            }
            else if (header.p_type == PT_NOTE && module.buildIdSize == 0)
            {
                module.buildIdSize =
                    Spark::CrashHandlerDetail::FindGnuBuildIdNote(reinterpret_cast<const std::uint8_t*>(runtimeAddress),
                                                                  static_cast<size_t>(header.p_memsz), module.buildId);
            }
        }
        if (lowest >= highest)
            return 0;

        module.beginAddress = lowest;
        module.endAddress = highest;
        // The main program reports an empty dlpi_name.
        const bool isMainProgram = !info->dlpi_name || info->dlpi_name[0] == '\0';
        CopyCrashModuleName(isMainProgram ? scan.mainProgramName : info->dlpi_name, module);
        ++table.count;
        return 0;
    }

    /// Exact faulting instruction address from the signal context, or 0.
    std::uintptr_t ContextProgramCounter(const void* context)
    {
        if (!context)
            return 0;
        const auto* userContext = static_cast<const ucontext_t*>(context);
#if defined(__x86_64__)
        return static_cast<std::uintptr_t>(userContext->uc_mcontext.gregs[REG_RIP]);
#elif defined(__aarch64__)
        return static_cast<std::uintptr_t>(userContext->uc_mcontext.pc);
#else
        (void)userContext;
        return 0;
#endif
    }

    /// Capture frames for the symbolic section. With a signal context the exact
    /// faulting PC is frame 0 and the handler/trampoline frames above it are
    /// dropped; everything else is a return address.
    size_t CaptureSymbolicFrames(const void* context, std::uintptr_t* frames, bool& firstFrameIsExactPc, bool nonfatal)
    {
        constexpr size_t kMaxFrames = Spark::CrashHandlerDetail::kMaxSymbolicCrashFrames;
        if (context)
        {
            const auto* captured = static_cast<const ucontext_t*>(context);
            const std::uintptr_t pc = ContextProgramCounter(context);
            firstFrameIsExactPc = pc != 0;
            size_t count = 0;
            if (pc)
            {
                frames[count++] = pc;
            }
            std::uintptr_t frame = 0;
            std::uintptr_t stack = 0;
#if defined(__x86_64__)
            frame = static_cast<std::uintptr_t>(captured->uc_mcontext.gregs[REG_RBP]);
            stack = static_cast<std::uintptr_t>(captured->uc_mcontext.gregs[REG_RSP]);
#elif defined(__aarch64__)
            frame = static_cast<std::uintptr_t>(captured->uc_mcontext.regs[29]);
            stack = static_cast<std::uintptr_t>(captured->uc_mcontext.sp);
#endif
            // Read only two frame-chain words from this process. No stack payload
            // is persisted, and invalid/omitted frame pointers stop at the exact PC.
            constexpr std::uintptr_t kMaxStackWalkBytes = 8 * 1024 * 1024;
            while (stack && frame >= stack && frame - stack < kMaxStackWalkBytes &&
                   frame % alignof(std::uintptr_t) == 0 && count < kMaxFrames)
            {
                std::uintptr_t words[2]{};
                iovec local{words, sizeof(words)};
                iovec remote{reinterpret_cast<void*>(frame), sizeof(words)};
                if (syscall(SYS_process_vm_readv, getpid(), &local, 1UL, &remote, 1UL, 0UL) !=
                    static_cast<long>(sizeof(words)))
                {
                    break;
                }
                if (words[1] == 0)
                {
                    break;
                }
                frames[count++] = words[1];
                if (words[0] <= frame)
                {
                    break;
                }
                frame = words[0];
            }
            return count;
        }
        if (!nonfatal)
        {
            firstFrameIsExactPc = false;
            return 0;
        }
        void* raw[kMaxFrames];
        const int rawCount = backtrace(raw, static_cast<int>(kMaxFrames));
        const size_t available = rawCount > 0 ? static_cast<size_t>(rawCount) : 0;

        size_t count = 0;
        size_t firstReturnFrame = 0;
        const std::uintptr_t pc = ContextProgramCounter(context);
        firstFrameIsExactPc = pc != 0;
        if (firstFrameIsExactPc)
        {
            frames[count++] = pc;
            for (size_t index = 0; index < available; ++index)
            {
                if (reinterpret_cast<std::uintptr_t>(raw[index]) == pc)
                {
                    firstReturnFrame = index + 1;
                    break;
                }
            }
        }
        for (size_t index = firstReturnFrame; index < available && count < kMaxFrames; ++index)
            frames[count++] = reinterpret_cast<std::uintptr_t>(raw[index]);
        return count;
    }

    /// Format the symbolic section into @p buffer using only precomputed module identity.
    size_t FormatSymbolicSection(const void* context, char* buffer, size_t capacity, bool nonfatal = false)
    {
        std::uintptr_t frames[Spark::CrashHandlerDetail::kMaxSymbolicCrashFrames];
        bool firstFrameIsExactPc = false;
        const size_t frameCount = CaptureSymbolicFrames(context, frames, firstFrameIsExactPc, nonfatal);
        const int active = g_activeCrashModuleTable.load(std::memory_order_acquire);
        const CrashModuleTable* table = active >= 0 ? &g_crashModuleTables[active] : nullptr;
        return Spark::CrashHandlerDetail::FormatSymbolicCrashFrames(table ? table->modules.data() : nullptr,
                                                                    table ? table->count : 0, frames, frameCount,
                                                                    firstFrameIsExactPc, buffer, capacity);
    }
} // namespace

void RefreshCrashModuleIdentities()
{
    std::lock_guard<std::mutex> lock(g_crashModuleRefreshLock);
    if (g_inSignalHandler.test(std::memory_order_acquire))
        return;

    char executablePath[PATH_MAX] = {};
    const ssize_t pathLength = readlink("/proc/self/exe", executablePath, sizeof(executablePath) - 1);
    executablePath[pathLength > 0 ? pathLength : 0] = '\0';

    const int active = g_activeCrashModuleTable.load(std::memory_order_acquire);
    const int next = active == 0 ? 1 : 0;
    CrashModuleScan scan;
    scan.table = &g_crashModuleTables[next];
    scan.table->count = 0;
    scan.mainProgramName = executablePath;
    dl_iterate_phdr(CollectCrashModuleIdentity, &scan);
    g_activeCrashModuleTable.store(next, std::memory_order_release);
}
#else
void RefreshCrashModuleIdentities()
{
    // Build-id capture is ELF-specific; macOS reports keep the backtrace text only.
}
#endif

// ----------------------------------------------------------------------------
// Fatal-signal report
//
// A fatal signal often arrives while the crashing thread holds a malloc,
// stdio, locale or loader lock. Linux uses only bounded buffers and descriptor
// operations after installation; its optional sections are labeled snapshots.
// The separately qualified macOS handler retains its two-stage behavior:
//
// 1. Async-signal-safe: arm a watchdog, then write the report's header, raw
//    stack and symbolic-frame section to a new file with open()/write() only,
//    using names and buffers prepared by InstallCrashHandler().
// 2. Best effort: append system and thread information, write the core hint
//    and publish the manifest. This stage allocates and may block on a lock
//    the crashed thread holds. The watchdog bounds it: after
//    kSignalReportBudgetSeconds the process is terminated by the original
//    signal, so a crash never becomes a hang.
// ----------------------------------------------------------------------------

namespace
{
    /// Wall-clock budget for the whole signal report.
    constexpr unsigned kSignalReportBudgetSeconds = 10;

    /// Report file-name prefix (the configured dump-prefix basename), copied
    /// at install time so the handler never builds a path on the heap.
    char g_signalLogPrefix[128] = "GameEngineCrash";
    /// "<prefix>_<16 hex>.log" for the report being written.
    char g_signalLogName[sizeof(g_signalLogPrefix) + 32] = {};
#ifdef SPARK_PLATFORM_LINUX
    bool g_signalWriteFailed = false; // Only the atomic-flag winner writes report buffers.
#endif

    /// The fatal signal being reported; read by the watchdog.
    volatile sig_atomic_t g_fatalSignal = 0;

    /// Test seam (CrashSymbolicationProbe --stall-report): block the
    /// best-effort stage as a crash under a held malloc lock would.
    volatile sig_atomic_t g_stallSignalReportForTesting = 0;

    /// Alternate stack for the installing thread, so a stack-overflow SIGSEGV
    /// still runs the handler.
    alignas(16) unsigned char g_signalAlternateStack[256 * 1024];

    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "the signal handler reserves report IDs with a lock-free atomic");

    size_t SignalSafeLength(const char* text)
    {
        size_t length = 0;
        while (text[length] != '\0')
            ++length;
        return length;
    }

    void SignalSafeWrite(int fd, const char* data, size_t length)
    {
        while (fd >= 0 && length > 0)
        {
            const ssize_t count = write(fd, data, length);
            if (count < 0 && errno == EINTR)
            {
                continue;
            }
            if (count <= 0)
            {
#ifdef SPARK_PLATFORM_LINUX
                g_signalWriteFailed = true;
#endif
                return;
            }
            data += count;
            length -= static_cast<size_t>(count);
        }
    }

    void SignalSafeWrite(int fd, const char* text)
    {
        SignalSafeWrite(fd, text, SignalSafeLength(text));
    }

    /// Append @p value in @p base (10 or 16, lowercase) without allocating.
    void SignalSafeWriteNumber(int fd, std::uint64_t value, unsigned base, size_t minDigits = 1)
    {
        constexpr char kDigits[] = "0123456789abcdef";
        char buffer[24];
        size_t position = sizeof(buffer);
        do
        {
            buffer[--position] = kDigits[value % base];
            value /= base;
        } while (value != 0 && position > 0);
        while (sizeof(buffer) - position < minDigits && position > 0)
        {
            buffer[--position] = '0';
        }
        SignalSafeWrite(fd, buffer + position, sizeof(buffer) - position);
    }

    /// Terminate with the original fatal signal (default action: core dump).
    [[noreturn]] void TerminateWithFatalSignal(int sig)
    {
        // If either startup control could not be applied, never re-raise the
        // signal: doing so would hand control back to the kernel's core writer.
        // The report remains available, but a failed policy cannot expose a
        // full process image.
        if (!g_posixCoreDumpPolicyEnforced)
        {
            _exit(128 + sig);
        }
        struct sigaction defaultAction;
        memset(&defaultAction, 0, sizeof(defaultAction));
        defaultAction.sa_handler = SIG_DFL;
        sigemptyset(&defaultAction.sa_mask);
        sigaction(sig, &defaultAction, nullptr);

        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, sig);
        sigprocmask(SIG_UNBLOCK, &unblock, nullptr);
        raise(sig);
        _exit(128 + sig); // only if the signal did not terminate the process
    }

    bool ApplyPosixCoreDumpPolicy(bool allowFullDump)
    {
        if (allowFullDump)
        {
            return true;
        }

        bool enforced = true;
        const struct rlimit disabledCore = {0, 0};
        if (setrlimit(RLIMIT_CORE, &disabledCore) != 0)
        {
            enforced = false;
            SPARK_LOG_ERROR(Spark::LogCategory::Core,
                            "CrashHandler: setrlimit(RLIMIT_CORE=0) failed; fatal signals will exit without re-raise");
        }
#ifdef SPARK_PLATFORM_LINUX
        if (prctl(PR_SET_DUMPABLE, 0L, 0L, 0L, 0L) != 0)
        {
            enforced = false;
            SPARK_LOG_ERROR(Spark::LogCategory::Core,
                            "CrashHandler: prctl(PR_SET_DUMPABLE=0) failed; fatal signals will exit without re-raise");
        }
#endif
        return enforced;
    }

    /// SIGALRM: the report overran its budget, most likely deadlocked on a
    /// lock the crashed thread holds. Finish the crash without the rest.
    void OnSignalReportTimeout(int)
    {
        WriteStderr("[SPARK ENGINE] Crash report timed out; terminating without the remaining sections.\n");
        const int sig = g_fatalSignal != 0 ? static_cast<int>(g_fatalSignal) : SIGABRT;
        TerminateWithFatalSignal(sig);
    }

    void ArmSignalReportWatchdog(int sig)
    {
        g_fatalSignal = sig;
        struct sigaction timeoutAction;
        memset(&timeoutAction, 0, sizeof(timeoutAction));
        timeoutAction.sa_handler = OnSignalReportTimeout;
        sigemptyset(&timeoutAction.sa_mask);
        timeoutAction.sa_flags = SA_ONSTACK;
        sigaction(SIGALRM, &timeoutAction, nullptr);

        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, SIGALRM);
        sigprocmask(SIG_UNBLOCK, &unblock, nullptr);
        alarm(kSignalReportBudgetSeconds);
    }

    /// Create "<prefix>_<id>.log" in the pinned artifact root with open()
    /// only. Returns -1 when no private root exists or the name is taken.
    int OpenSignalReportLog(std::uint64_t reportSequence)
    {
#ifdef SPARK_PLATFORM_LINUX
        // A fork child must install its own pinned directory before reporting.
        if (getpid() != g_signalArtifactOwnerPid)
        {
            return -1;
        }
#endif
        if (g_artifactRootHandle < 0)
        {
            return -1;
        }
        size_t length = 0;
        const auto append = [&](const char* text)
        {
            for (; *text != '\0' && length + 1 < sizeof(g_signalLogName); ++text)
            {
                g_signalLogName[length++] = *text;
            }
        };
        append(g_signalLogPrefix);
        append("_");
        constexpr char kDigits[] = "0123456789abcdef";
        for (int shift = 60; shift >= 0 && length + 1 < sizeof(g_signalLogName); shift -= 4)
        {
            g_signalLogName[length++] = kDigits[(reportSequence >> shift) & 0xF];
        }
        append(".log");
        g_signalLogName[length] = '\0';

        int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
        return openat(g_artifactRootHandle, g_signalLogName, flags, S_IRUSR | S_IWUSR);
    }

    /// Stage 1: everything a post-mortem needs, written with async-signal-safe calls only.
    void WriteSignalSafeReport(int fd, int sig, const char* sigName, const siginfo_t* info, void* context)
    {
        SignalSafeWrite(fd, "================================================================\n"
                            "           SPARK ENGINE CRASH REPORT (POSIX)\n"
                            "================================================================\n\n");
        SignalSafeWrite(fd, "Timestamp  : ");
        SignalSafeWriteNumber(fd, static_cast<std::uint64_t>(time(nullptr)), 10);
        SignalSafeWrite(fd, " (seconds since the Unix epoch, UTC)\nProcess ID : ");
        SignalSafeWriteNumber(fd, static_cast<std::uint64_t>(getpid()), 10);
        SignalSafeWrite(fd, "\nThread ID  : ");
#ifdef SPARK_PLATFORM_LINUX
        SignalSafeWriteNumber(fd, static_cast<std::uint64_t>(syscall(SYS_gettid)), 10);
#else
        SignalSafeWriteNumber(fd, static_cast<std::uint64_t>(pthread_mach_thread_np(pthread_self())), 10);
#endif
        SignalSafeWrite(fd, "\n\n*** CRASH DETECTED ***\nSignal     : ");
        SignalSafeWriteNumber(fd, static_cast<std::uint64_t>(sig), 10);
        SignalSafeWrite(fd, " - ");
        SignalSafeWrite(fd, sigName);
        SignalSafeWrite(fd, "\n");
        if (info)
        {
            SignalSafeWrite(fd, "Fault Addr : 0x");
            SignalSafeWriteNumber(fd, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(info->si_addr)), 16);
            SignalSafeWrite(fd, "\nSignal Code: ");
            if (info->si_code < 0)
            {
                SignalSafeWrite(fd, "-");
                SignalSafeWriteNumber(fd, static_cast<std::uint64_t>(-static_cast<std::int64_t>(info->si_code)), 10);
            }
            else
            {
                SignalSafeWriteNumber(fd, static_cast<std::uint64_t>(info->si_code), 10);
            }
            SignalSafeWrite(fd, "\n");
        }

        SignalSafeWrite(fd, "\n*** STACK TRACE ***\n");
#ifndef SPARK_PLATFORM_LINUX
        // The unchanged macOS backend remains separately qualified.
        void* frames[64];
        const int frameCount = backtrace(frames, 64);
        for (int index = 0; index < frameCount; ++index)
        {
            SignalSafeWrite(fd, "  ");
            SignalSafeWrite(fd, kStackFrameMarker);
            backtrace_symbols_fd(&frames[index], 1, fd);
        }
#endif
#ifdef SPARK_PLATFORM_LINUX
        SignalSafeWrite(fd, "Context frames follow; an unavailable frame chain retains the exact PC only.\n");
        SignalSafeWrite(fd, g_symbolicSectionBuffer,
                        FormatSymbolicSection(context, g_symbolicSectionBuffer, sizeof(g_symbolicSectionBuffer)));
#else
        (void)context;
#endif
    }

#ifdef SPARK_PLATFORM_LINUX
    // Written during quiescent installation, before signal actions are exposed.
    // No late config reads, allocations, directory scans or thread enumeration.
    struct PreparedSignalArtifacts
    {
        Spark::CrashHandlerDetail::SignalCrashManifest manifest;
        char optionalSections[16 * 1024]{};
        size_t optionalSize = 0;
        bool ready = false;
    };
    PreparedSignalArtifacts g_preparedSignalArtifacts;

    int OpenSignalArtifact(const char* name)
    {
        if (g_artifactRootHandle < 0)
        {
            return -1;
        }
        return openat(g_artifactRootHandle, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                      S_IRUSR | S_IWUSR);
    }

    bool FinishSignalArtifact(int fd, bool complete)
    {
        struct stat metadata
        {
        };
        const bool privateFile = fstat(fd, &metadata) == 0 && S_ISREG(metadata.st_mode) && metadata.st_nlink == 1 &&
                                 metadata.st_uid == geteuid() && (metadata.st_mode & 0777) == 0600;
        const bool closed = close(fd) == 0;
        return complete && privateFile && closed;
    }

    void WriteBoundedSignalArtifacts(int fd, int sig, std::uint64_t sequence)
    {
        if (g_stallSignalReportForTesting)
        {
            for (;;)
            {
                pause();
            }
        }
        const auto& prepared = g_preparedSignalArtifacts;
        if (prepared.optionalSize)
        {
            SignalSafeWrite(fd, prepared.optionalSections, prepared.optionalSize);
        }
        const bool logReady = FinishSignalArtifact(fd, !g_signalWriteFailed);
        if (!logReady || !prepared.ready)
        {
            WriteStderr("[SPARK ENGINE] Incomplete signal log; manifest not published.\n");
            return;
        }

        using Spark::CrashHandlerDetail::SignalArtifactBuffer;
        char coreName[sizeof(g_signalLogName) + 16]{};
        SignalArtifactBuffer core(coreName, sizeof(coreName));
        core.Append(g_signalLogPrefix);
        core.Append("_");
        core.Number(sequence, 16, 16);
        core.Append(".core_hint");
        const char* hint = prepared.manifest.fullMemoryDump
                               ? "Full-dump debugging opt-in: the OS core policy is unchanged.\n"
                                 "On Linux check /proc/sys/kernel/core_pattern or coredumpctl list.\n"
                               : "Kernel core dumps are disabled by the default crash privacy policy.\n";
        const int coreFd = core.Size() ? OpenSignalArtifact(coreName) : -1;
        bool coreReady = false;
        if (coreFd >= 0)
        {
            g_signalWriteFailed = false;
            SignalSafeWrite(coreFd, hint);
            coreReady = FinishSignalArtifact(coreFd, !g_signalWriteFailed);
        }

        char readyName[64]{};
        SignalArtifactBuffer ready(readyName, sizeof(readyName));
        ready.Append("crash_manifest_");
        ready.Number(sequence, 16, 16);
        ready.Append(".json");
        char temporaryName[72]{};
        SignalArtifactBuffer temporary(temporaryName, sizeof(temporaryName));
        temporary.Append(readyName);
        temporary.Append(".tmp");
        const time_t now = time(nullptr);
        if (!ready.Size() || !temporary.Size() || now < 0)
        {
            return;
        }
        auto manifest = prepared.manifest;
        manifest.processId = static_cast<std::uint64_t>(getpid());
        manifest.epochSeconds = static_cast<std::uint64_t>(now);
        manifest.logName = g_signalLogName;
        manifest.coreHintName = coreReady ? std::string_view(coreName) : std::string_view{};
        switch (sig)
        {
        case SIGSEGV:
            manifest.title = "SIGSEGV";
            break;
        case SIGABRT:
            manifest.title = "SIGABRT";
            break;
        case SIGFPE:
            manifest.title = "SIGFPE";
            break;
        case SIGBUS:
            manifest.title = "SIGBUS";
            break;
        case SIGILL:
            manifest.title = "SIGILL";
            break;
        case SIGTRAP:
            manifest.title = "SIGTRAP";
            break;
        default:
            manifest.title = "Crash";
            break;
        }
        char json[4096];
        const size_t jsonSize = Spark::CrashHandlerDetail::FormatSignalCrashManifest(manifest, json, sizeof(json));
        const int manifestFd = jsonSize ? OpenSignalArtifact(temporaryName) : -1;
        if (manifestFd < 0)
        {
            return;
        }
        g_signalWriteFailed = false;
        SignalSafeWrite(manifestFd, json, jsonSize);
        const bool complete = FinishSignalArtifact(manifestFd, !g_signalWriteFailed);
        // Linux renameat2 with RENAME_NOREPLACE publishes atomically without
        // overwriting an existing report. Unsupported kernels fail closed.
        constexpr unsigned kRenameNoReplace = 1;
        if (!complete || syscall(SYS_renameat2, g_artifactRootHandle, temporaryName, g_artifactRootHandle, readyName,
                                 kRenameNoReplace) != 0)
        {
            unlinkat(g_artifactRootHandle, temporaryName, 0);
            WriteStderr("[SPARK ENGINE] Signal manifest publication failed.\n");
        }
        WriteStderr("Log file (private artifact directory): ");
        WriteStderr(g_signalLogName);
        WriteStderr("\n");
    }
#endif

#ifndef SPARK_PLATFORM_LINUX
    /// Stage 2: optional sections and the manifest. Allocates; bounded by the watchdog.
    void WriteBestEffortReport(int fd, int sig)
    {
        if (g_stallSignalReportForTesting)
        {
            for (;;)
            {
                pause();
            }
        }

        const std::string logName = g_signalLogName;
        const std::string reportId = logName.substr(logName.size() - 20, 16); // "<id>.log"
        if (g_cfg.captureSystemInfo)
        {
            const std::string section = LinuxSystemInfo();
            SignalSafeWrite(fd, section.data(), section.size());
        }
        if (g_cfg.captureAllThreads)
        {
            const std::string section = LinuxThreadStacks();
            SignalSafeWrite(fd, section.data(), section.size());
        }
        const bool logReady = close(fd) == 0;

        const std::string prefix = GetCrashArtifactPrefix().string();
        const std::string logFile = (g_artifactRootPath / logName).string();
        const std::string coreFile = prefix + "_" + reportId + ".core_hint";
        const std::string coreHint = g_cfg.includeStackMemory
                                         ? "Full-dump debugging opt-in: the OS core policy is unchanged.\n"
                                           "On Linux check /proc/sys/kernel/core_pattern or coredumpctl list.\n"
                                         : "Kernel core dumps are disabled by the default crash privacy policy.\n";
        const bool coreReady = WriteExclusiveFileUtf8(coreFile, coreHint);
        PinnedFile coreProbe = coreReady ? OpenPinnedInputFile(coreFile) : PinnedFile{};
        PinnedFile logProbe = logReady ? OpenPinnedInputFile(logFile) : PinnedFile{};

        const std::string crashTitle = (sig == SIGSEGV)   ? "SIGSEGV"
                                       : (sig == SIGABRT) ? "SIGABRT"
                                       : (sig == SIGFPE)  ? "SIGFPE"
                                                          : "Crash";

        // Always publish the local manifest when the log is safely pinned. A
        // non-headless process normally has a read-only reporter watching this
        // directory; headless processes retain the same local artifacts without
        // starting UI or transport work.
        if (logProbe.stream)
            WriteCrashManifest(reportId, coreProbe.stream ? coreFile : std::string{}, logFile, "", "", crashTitle);

        WriteStderr("Log file: ");
        WriteStderr(logFile.c_str());
        WriteStderr("\n");
    }

#endif

    /// Prepare everything the signal handler reads, then give the installing
    /// thread an alternate signal stack. Called from InstallCrashHandler().
    void PrepareSignalReport()
    {
        const std::string prefix = GetCrashArtifactPrefix().filename().string();
        const std::string safePrefix = prefix.empty() ? std::string("GameEngineCrash") : prefix;
        const size_t length = std::min(safePrefix.size(), sizeof(g_signalLogPrefix) - 1);
        memcpy(g_signalLogPrefix, safePrefix.data(), length);
        g_signalLogPrefix[length] = '\0';
#ifdef SPARK_PLATFORM_LINUX
        // Install/reinstall is a quiescent lifecycle operation, as for the pinned root.
        // Snapshot optional sections now; never enumerate threads or use stdio in a signal.
        g_preparedSignalArtifacts = {};
        auto& manifest = g_preparedSignalArtifacts.manifest;
        manifest.requireConsent = g_cfg.requireConsent;
        manifest.allowScreenshotRefusal = g_cfg.allowScreenshotRefusal;
        manifest.promptUserDescription = g_cfg.promptUserDescription;
        manifest.fullMemoryDump = g_cfg.includeStackMemory;
        std::string sections;
        if (g_cfg.captureSystemInfo || g_cfg.captureAllThreads)
        {
            sections = "\n*** INSTALL-TIME DIAGNOSTIC SNAPSHOT (NOT CRASH-TIME THREAD STATE) ***\n";
            sections += MakeTimeStampUtf8() + "\n";
            if (g_cfg.captureSystemInfo)
            {
                sections += LinuxSystemInfo();
            }
            if (g_cfg.captureAllThreads)
            {
                sections += LinuxThreadStacks();
            }
        }
        constexpr std::string_view truncated = "\n[install-time snapshot truncated]\n";
        const size_t available = sizeof(g_preparedSignalArtifacts.optionalSections) - truncated.size();
        const size_t copied = std::min(sections.size(), available);
        memcpy(g_preparedSignalArtifacts.optionalSections, sections.data(), copied);
        g_preparedSignalArtifacts.optionalSize = copied;
        if (sections.size() > copied)
        {
            memcpy(g_preparedSignalArtifacts.optionalSections + copied, truncated.data(), truncated.size());
            g_preparedSignalArtifacts.optionalSize += truncated.size();
        }
        // Initialization creates a fresh private per-process root. Verify the
        // empty-queue invariant before reserving a slot; scan failure disables publication.
        g_preparedSignalArtifacts.ready = g_artifactRootHandle >= 0 && CountPendingCrashManifests() == 0;
        g_reserveFatalManifest = g_preparedSignalArtifacts.ready;
        g_signalArtifactOwnerPid = getpid();
#endif

        stack_t current;
        memset(&current, 0, sizeof(current));
        if (sigaltstack(nullptr, &current) == 0 && (current.ss_flags & SS_DISABLE) != 0)
        {
            stack_t alternate;
            memset(&alternate, 0, sizeof(alternate));
            alternate.ss_sp = g_signalAlternateStack;
            alternate.ss_size = sizeof(g_signalAlternateStack);
            sigaltstack(&alternate, nullptr);
        }
    }
} // namespace

namespace Spark::CrashHandlerDetail
{
    void SetSignalReportStallForTesting(bool stall) noexcept
    {
        g_stallSignalReportForTesting = stall ? 1 : 0;
    }
} // namespace Spark::CrashHandlerDetail

static void HandleLinuxCrash(int sig, siginfo_t* info, void* context)
{
    // Prevent re-entrant crashes (e.g. crash inside the handler itself).
    // atomic_flag is always lock-free and also excludes another crashing thread.
    if (g_inSignalHandler.test_and_set(std::memory_order_acq_rel))
    {
        TerminateWithFatalSignal(sig);
    }

    // A deadlocked report must still end the process.
    ArmSignalReportWatchdog(sig);

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

    // One report per process; the sequence is shared with nonfatal reports.
    const std::uint64_t reportSequence = g_reportSequence.fetch_add(1, std::memory_order_relaxed) + 1;
    const int fd = OpenSignalReportLog(reportSequence);
    if (fd < 0)
    {
        WriteStderr("[SPARK ENGINE] Private crash-artifact directory unavailable; report not written.\n");
        TerminateWithFatalSignal(sig);
    }
#ifdef SPARK_PLATFORM_LINUX
    g_signalWriteFailed = false;
#endif
    WriteSignalSafeReport(fd, sig, sigName, info, context);

#ifdef SPARK_PLATFORM_LINUX
    WriteBoundedSignalArtifacts(fd, sig, reportSequence);
#else
    // The macOS best-effort path is unchanged by the Linux signal-safety fix.
    try
    {
        WriteBestEffortReport(fd, sig);
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

#endif
    // Preserve fatal-signal termination; the default policy prevents kernel cores.
    alarm(0);
    TerminateWithFatalSignal(sig);
}

void InstallCrashHandler(const CrashConfig& cfg)
{
    SPARK_LOG_INFO(Spark::LogCategory::Core, "Installing crash handler (Linux)");
    AssignCrashConfig(cfg);
    // Kernel core files are a second crash producer outside the private
    // artifact directory. Keep them disabled unless the explicit local-debug
    // full-dump opt-in was selected by SparkEngine.cpp.
    g_posixCoreDumpPolicyEnforced = ApplyPosixCoreDumpPolicy(cfg.includeStackMemory);
    if (!g_posixCoreDumpPolicyEnforced)
    {
        // A nested fault can bypass a SA_RESETHAND handler. Do not continue
        // startup with an unenforced kernel policy and rely on re-raise gating.
        WriteStderr("[SPARK ENGINE] Cannot enforce kernel core-dump privacy; terminating startup.\n");
        _exit(EXIT_FAILURE);
    }
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

    // Capture module identity now so the signal handler only reads it, and
    // prime backtrace(): its first call loads the unwinder, which allocates.
    RefreshCrashModuleIdentities();
    void* primeFrame[1];
    (void)backtrace(primeFrame, 1);
    PrepareSignalReport();

    // Install signal handlers for common crash signals. SA_ONSTACK runs the
    // handler on the alternate stack PrepareSignalReport() gave this thread,
    // so a stack overflow is reported too; other threads use their own stack.
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = HandleLinuxCrash;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND | SA_ONSTACK; // SA_RESETHAND to avoid infinite loops
    sigemptyset(&sa.sa_mask);

    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGTRAP, &sa, nullptr);

    // The external reporter is intentionally read-only. Launch it for every
    // interactive process; headless processes keep artifacts local and never
    // start a UI.
    if (!g_cfg.headlessMode)
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

    TriggerCrashReport(assertMsg);
}

void TriggerCrashReport(const char* reason)
{
    // See g_crashReported: Assert::Fail reaches this through both the gated and
    // the ungated entry point, and one failure must leave exactly one report.
    if (g_crashReported.exchange(true, std::memory_order_acq_rel))
    {
        WriteStderr("[SPARK ENGINE] Crash report already written for this process; duplicate ignored.\n");
        return;
    }

    // Generate the on-disk report for a failure the process will not survive
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

    if (reason)
    {
        log << "*** ASSERTION FAILURE ***\n" << reason << "\n\n";
    }

    log << CaptureStackTraceString();
#ifdef SPARK_PLATFORM_LINUX
    {
        std::vector<char> section(sizeof(g_symbolicSectionBuffer));
        log.write(section.data(),
                  static_cast<std::streamsize>(FormatSymbolicSection(nullptr, section.data(), section.size(), true)));
    }
#endif
    if (g_cfg.captureSystemInfo)
        log << LinuxSystemInfo();

    const bool logReady = WriteExclusiveFileUtf8(logFile, log.str());
    if (!logReady)
        WriteStderr("[SPARK ENGINE] Failed to write assertion report safely.\n");
    else
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

void TriggerCrashReportUnattended(const char* reason)
{
    // This path already writes artifacts only — no dialog, no upload — so the
    // watchdog needs nothing beyond the shared once-guard above.
    TriggerCrashReport(reason);
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

void TriggerCrashReport(const char* reason)
{
    if (reason)
        std::cerr << "Crash report (stub platform): " << reason << "\n";
}

void TriggerCrashReportUnattended(const char* reason)
{
    TriggerCrashReport(reason);
}

void SetAssertCrashBehavior(bool shouldCrash)
{
    g_triggerCrashOnAssert = shouldCrash;
}

void RefreshCrashModuleIdentities() {}
#endif
