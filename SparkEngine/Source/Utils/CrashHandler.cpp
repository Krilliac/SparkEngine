#include "Utils/CrashHandler.h"
#include "../Core/Platform.h"
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
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <mutex>
#include <iostream>
#include <ctime>
#include <string>
#include <cstring>

#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
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
#if defined(SPARK_PLATFORM_LINUX) || defined(SPARK_PLATFORM_MACOS)
static volatile sig_atomic_t g_inSignalHandler = 0;
#endif


// ============================================================================
// Cross-platform helpers
// ============================================================================

static std::string WideToUtf8(const std::wstring& w)
{
#ifdef SPARK_PLATFORM_WINDOWS
    if (w.empty())
        return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1)
        return {};                // Only null terminator
    std::string s(len - 1, '\0'); // Exclude the null terminator from std::string length
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], len, nullptr, nullptr);
    return s;
#else
    std::string result;
    result.reserve(w.size());
    for (wchar_t c : w)
    {
        if (c < 0x80)
        {
            result.push_back(static_cast<char>(c));
        }
        else if (c < 0x800)
        {
            result.push_back(static_cast<char>(0xC0 | (c >> 6)));
            result.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
        else
        {
            result.push_back(static_cast<char>(0xE0 | (c >> 12)));
            result.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return result;
#endif
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

static void ZipFilesUtf8(const std::string& zip, const std::vector<std::string>& files)
{
    mz_zip_archive za{};
    if (!mz_zip_writer_init_file(&za, zip.c_str(), 0))
        return;
    for (const auto& f : files)
    {
        if (std::filesystem::exists(f))
        {
            std::string entry = std::filesystem::path(f).filename().string();
            mz_zip_writer_add_file(&za, entry.c_str(), f.c_str(), nullptr, 0, MZ_BEST_COMPRESSION);
        }
    }
    mz_zip_writer_finalize_archive(&za);
    mz_zip_writer_end(&za);
}

// ============================================================================
// Crash manifest for out-of-process reporter
// ============================================================================

static std::string g_manifestDir; // Set during InstallCrashHandler

static unsigned long GetEnginePID()
{
#ifdef SPARK_PLATFORM_WINDOWS
    return static_cast<unsigned long>(GetCurrentProcessId());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

static std::string MakeManifestJson(const std::string& dumpFile, const std::string& logFile,
                                    const std::string& screenshotFile, const std::string& zipFile,
                                    const std::string& crashTitle)
{
    auto jsonEsc = [](const std::string& s) -> std::string
    {
        std::string out;
        for (char c : s)
        {
            if (c == '"')
                out += "\\\"";
            else if (c == '\\')
                out += "\\\\";
            else if (c == '\n')
                out += "\\n";
            else
                out += c;
        }
        return out;
    };

    std::ostringstream j;
    j << "{\n";
    j << "  \"enginePID\": \"" << GetEnginePID() << "\",\n";
    j << "  \"timestamp\": \"" << jsonEsc(crashTitle) << "\",\n";
    j << "  \"dumpFile\": \"" << jsonEsc(dumpFile) << "\",\n";
    j << "  \"logFile\": \"" << jsonEsc(logFile) << "\",\n";
    j << "  \"screenshotFile\": \"" << jsonEsc(screenshotFile) << "\",\n";
    j << "  \"zipFile\": \"" << jsonEsc(zipFile) << "\",\n";
    j << "  \"crashTitle\": \"" << jsonEsc(crashTitle) << "\",\n";
    j << "  \"uploadURL\": \"" << jsonEsc(g_cfg.uploadURL) << "\",\n";
    j << "  \"proxyURL\": \"" << jsonEsc(g_cfg.proxyURL) << "\",\n";
    j << "  \"githubRepo\": \"" << jsonEsc(g_cfg.githubRepo) << "\",\n";
    j << "  \"githubToken\": \"" << jsonEsc(g_cfg.githubToken) << "\",\n";
    j << "  \"githubLabels\": \"" << jsonEsc(g_cfg.githubLabels) << "\",\n";
    j << "  \"smtpUser\": \"" << jsonEsc(g_cfg.smtpUser) << "\",\n";
    j << "  \"smtpPass\": \"" << jsonEsc(g_cfg.smtpPass) << "\",\n";
    j << "  \"emailTo\": \"" << jsonEsc(g_cfg.emailTo) << "\",\n";
    j << "  \"emailFrom\": \"" << jsonEsc(g_cfg.emailFrom) << "\",\n";
    j << "  \"requireConsent\": " << (g_cfg.requireConsent ? "true" : "false") << ",\n";
    j << "  \"allowScreenshotRefusal\": " << (g_cfg.allowScreenshotRefusal ? "true" : "false") << ",\n";
    j << "  \"promptUserDescription\": " << (g_cfg.promptUserDescription ? "true" : "false") << ",\n";
    j << "  \"timeoutSeconds\": " << g_cfg.connectTimeoutSeconds << "\n";
    j << "}\n";
    return j.str();
}

static void WriteCrashManifest(const std::string& dumpFile, const std::string& logFile,
                               const std::string& screenshotFile, const std::string& zipFile,
                               const std::string& crashTitle)
{
    if (g_manifestDir.empty())
        return;

    std::string manifestPath = g_manifestDir + "/crash_manifest.json";
    std::string json = MakeManifestJson(dumpFile, logFile, screenshotFile, zipFile, crashTitle);

    // Write using POSIX/Win32 low-level I/O for signal safety
    std::ofstream f(manifestPath);
    if (f.is_open())
    {
        f << json;
        f.close();
    }
}

// Try to launch SparkCrashReporter in watchdog mode
static void LaunchCrashReporter()
{
    // Look for SparkCrashReporter next to the engine executable
    std::string reporterName = "SparkCrashReporter";
#ifdef SPARK_PLATFORM_WINDOWS
    reporterName += ".exe";
#endif

    // Try multiple locations
    std::vector<std::string> searchPaths;
    searchPaths.push_back("./" + reporterName);
    searchPaths.push_back("./bin/" + reporterName);

    std::string reporterPath;
    for (const auto& p : searchPaths)
    {
        if (std::filesystem::exists(p))
        {
            reporterPath = p;
            break;
        }
    }

    if (reporterPath.empty())
    {
        SPARK_LOG_DEBUG(Spark::LogCategory::Core,
                        "CrashHandler: SparkCrashReporter not found, using in-process reporting");
        return;
    }

    // Create manifest directory
    g_manifestDir = std::filesystem::temp_directory_path().string() + "/spark_crash_" + std::to_string(GetEnginePID());
    std::filesystem::create_directories(g_manifestDir);

    // Launch reporter in watchdog mode (detached — survives engine crash)
    auto result = Spark::Process::Builder(reporterPath)
                      .Arg("--watch")
                      .Arg(g_manifestDir)
                      .Arg(std::to_string(GetEnginePID()))
                      .Detached()
                      .Launch();

    if (result)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Core, "CrashHandler: Launched SparkCrashReporter (watchdog mode)");
    }
    else
    {
        SPARK_LOG_WARN(Spark::LogCategory::Core, "CrashHandler: Failed to launch SparkCrashReporter: %s",
                       result.error().c_str());
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
static void WriteMiniDump(const std::wstring& path, EXCEPTION_POINTERS* ep);
static std::wstring MakeTimeStamp();
static std::wstring SymStackTrace(EXCEPTION_POINTERS* ep);
static std::wstring SystemInfo();
static std::wstring ThreadStacks();
static void SaveScreenshot(const std::wstring& file);
static void ZipFiles(const std::wstring& zip, const std::vector<std::wstring>& files);

void InstallCrashHandler(const CrashConfig& cfg)
{
    SPARK_LOG_INFO(Spark::LogCategory::Core, "Installing crash handler (Windows)");
    g_cfg = cfg;
    g_triggerCrashOnAssert = cfg.triggerCrashOnAssert;

#ifdef SPARK_HAS_CURL
    curl_global_init(CURL_GLOBAL_DEFAULT);
#endif

    SetUnhandledExceptionFilter(CrashFilter);

    // Launch out-of-process crash reporter if available
    LaunchCrashReporter();

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

    std::wstring stamp = MakeTimeStamp();
    std::wstring dump = g_cfg.dumpPrefix + stamp + L".dmp";
    std::wstring logFile = g_cfg.dumpPrefix + stamp + L".log";
    std::wstring shot = g_cfg.dumpPrefix + stamp + L".png";
    std::wstring zipFile = g_cfg.dumpPrefix + stamp + L".zip";

    WriteMiniDump(dump, ep);

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
    if (g_cfg.captureAllThreads)
        log << ThreadStacks();

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

    {
#if defined(_MSC_VER)
        std::wofstream ofs(logFile, std::ios::out | std::ios::trunc);
#else
        // MinGW libstdc++ doesn't support wstring paths in fstream
        std::string narrowLogFile(logFile.begin(), logFile.end());
        std::wofstream ofs(narrowLogFile.c_str(), std::ios::out | std::ios::trunc);
#endif
        ofs << log.str();
    }

    if (g_cfg.captureScreenshot)
        SaveScreenshot(shot);

    std::vector<std::wstring> files{dump, logFile};
    if (g_cfg.captureScreenshot)
        files.push_back(shot);
    if (g_cfg.zipBeforeUpload)
        ZipFiles(zipFile, files);

    // Write crash manifest for out-of-process reporter
    {
        std::string crashTitle = assertMsg ? "Assertion Failure" : "Crash Detected";
        WriteCrashManifest(WideToUtf8(dump), WideToUtf8(logFile), WideToUtf8(shot), WideToUtf8(zipFile), crashTitle);
    }

    // ---- Upload crash report (with optional consent dialog) ----
    // If the out-of-process reporter is running, it will handle the upload.
    // The in-process upload is a fallback for when the reporter isn't available.
    bool ok = true;
    if (g_cfg.enableCrashReporting)
    {
        bool userConsented = true;
        bool includeScreenshot = true;

        if (g_cfg.requireConsent && !g_cfg.headlessMode)
        {
            // Build consent message — mention screenshots if they can be refused
            std::wstring consentMsg = L"SparkEngine has crashed. Would you like to send a crash report "
                                      L"to help improve the engine?\n\nNo personal data is included.";
            if (g_cfg.allowScreenshotRefusal && g_cfg.captureScreenshot)
                consentMsg += L"\n\n(A screenshot was captured — you can opt out in the next dialog.)";

            int result = MessageBoxW(nullptr, consentMsg.c_str(), L"Crash Report", MB_YESNO | MB_ICONERROR);
            userConsented = (result == IDYES);

            // Screenshot opt-out dialog
            if (userConsented && g_cfg.allowScreenshotRefusal && g_cfg.captureScreenshot)
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
            // Remove screenshot from zip if user refused
            if (!includeScreenshot)
            {
                try
                {
                    std::filesystem::remove(std::filesystem::path(shot));
                }
                catch (...)
                {
                }
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

            std::string zipUtf8 = WideToUtf8(zipFile);
            // Copy config and attach user description
            CrashConfig uploadCfg = g_cfg;
            uploadCfg.userDescription = userDesc;
            ok = UploadCrashReport(uploadCfg, logUtf8, g_cfg.zipBeforeUpload ? zipUtf8 : "");
        }
    }

    if (!g_cfg.headlessMode)
    {
        std::wstring msg = assertMsg ? L"Assertion captured.\n" : L"Crash captured.\n";
        msg += L"Files:\n" + dump + L"\n" + logFile;
        MessageBoxW(nullptr, msg.c_str(), assertMsg ? L"Assertion Handler" : L"Crash Handler", MB_OK | MB_ICONERROR);
    }
}

static void WriteMiniDump(const std::wstring& file, EXCEPTION_POINTERS* ep)
{
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;

    MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), ep, TRUE};
    MiniDumpWriteDump(
        GetCurrentProcess(), GetCurrentProcessId(), h,
        static_cast<MINIDUMP_TYPE>(MiniDumpWithFullMemory | MiniDumpWithHandleData | MiniDumpWithUnloadedModules),
        &info, nullptr, nullptr);
    CloseHandle(h);
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

static std::wstring ThreadStacks()
{
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return L"*** THREAD STACKS ***\nFailed to create snapshot\n";

    std::wstringstream out;
    out << L"*** THREAD STACKS ***\n";

    BYTE symBuffer[sizeof(SYMBOL_INFO) + 256] = {};

    THREADENTRY32 te{sizeof(te)};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
    {
        if (te.th32OwnerProcessID != pid)
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

static void SaveScreenshot(const std::wstring& file)
{
    auto sc = GetMainSwapChain();
    if (!sc)
        return;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), &back)))
        return;

    D3D11_TEXTURE2D_DESC d;
    back->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.BindFlags = d.MiscFlags = 0;

    auto dev = GetD3DDevice();
    auto ctx = GetD3DContext();
    if (!dev || !ctx)
        return;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> cpu;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &cpu)))
        return;
    ctx->CopyResource(cpu.Get(), back.Get());

    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(cpu.Get(), 0, D3D11_MAP_READ, 0, &m)))
        return;

    HRESULT hrCom = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    (void)hrCom;
    IWICImagingFactory* wic = nullptr;
    IWICStream* stm = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frm = nullptr;

    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
        SUCCEEDED(wic->CreateStream(&stm)) && SUCCEEDED(stm->InitializeFromFilename(file.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
        SUCCEEDED(enc->Initialize(stm, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&frm, nullptr)) &&
        SUCCEEDED(frm->Initialize(nullptr)) && SUCCEEDED(frm->SetSize(d.Width, d.Height)))
    {
        WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
        frm->SetPixelFormat(&pf);
        frm->WritePixels(d.Height, m.RowPitch, static_cast<UINT>(m.RowPitch * d.Height),
                         reinterpret_cast<BYTE*>(m.pData));
        frm->Commit();
        enc->Commit();
    }
    if (frm)
        frm->Release();
    if (enc)
        enc->Release();
    if (stm)
        stm->Release();
    if (wic)
        wic->Release();
    CoUninitialize();
    ctx->Unmap(cpu.Get(), 0);
}

static void ZipFiles(const std::wstring& zip, const std::vector<std::wstring>& files)
{
    std::string zipUtf = WideToUtf8(zip);
    std::vector<std::string> utf8Files;
    for (const auto& f : files)
        utf8Files.push_back(WideToUtf8(f));
    ZipFilesUtf8(zipUtf, utf8Files);
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
        std::string stamp = MakeTimeStampUtf8();
        std::string prefix = WideToUtf8(g_cfg.dumpPrefix);
        std::string logFile = prefix + stamp + ".log";
        std::string zipFile = prefix + stamp + ".zip";

        std::ostringstream log;
        log << "================================================================\n";
        log << "           SPARK ENGINE CRASH REPORT (POSIX)\n";
        log << "================================================================\n\n";
        log << "Timestamp  : " << stamp << "\n";
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

        // Write log file using POSIX open/write (safer than std::ofstream
        // in a signal context, though still best-effort).
        std::string logStr = log.str();
        int fd = open(logFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0)
        {
            (void)write(fd, logStr.c_str(), logStr.size());
            close(fd);
        }

        // Write core dump path hint
        std::string coreFile = prefix + stamp + ".core_hint";
        static const char coreHint[] = "Core dump may be found at the system core dump location.\n"
                                       "Check: /proc/sys/kernel/core_pattern\n"
                                       "Or run: coredumpctl list\n";
        fd = open(coreFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0)
        {
            (void)write(fd, coreHint, sizeof(coreHint) - 1);
            close(fd);
        }

        // Package files
        std::vector<std::string> files{logFile, coreFile};
        if (g_cfg.zipBeforeUpload)
            ZipFilesUtf8(zipFile, files);

        // Write crash manifest for out-of-process reporter
        {
            std::string crashTitle = (sig == SIGSEGV)   ? "SIGSEGV"
                                     : (sig == SIGABRT) ? "SIGABRT"
                                     : (sig == SIGFPE)  ? "SIGFPE"
                                                        : "Crash";
            WriteCrashManifest("", logFile, "", zipFile, crashTitle);
        }

        // Upload crash report (with optional consent + screenshot refusal)
        if (g_cfg.enableCrashReporting)
        {
            bool userConsented = true;
            if (g_cfg.requireConsent && !g_cfg.headlessMode)
            {
                int result = MessageBoxA(nullptr,
                                         "SparkEngine has crashed. Would you like to send a crash report "
                                         "to help improve the engine?\n\nNo personal data is included.",
                                         "Crash Report", MB_YESNO | MB_ICONERROR);
                userConsented = (result == IDYES);
            }

            if (userConsented)
            {
                CrashConfig uploadCfg = g_cfg;
                bool uploadOk = UploadCrashReport(uploadCfg, log.str(), g_cfg.zipBeforeUpload ? zipFile : "");
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
    g_cfg = cfg;
    g_triggerCrashOnAssert = cfg.triggerCrashOnAssert;

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

    // Launch out-of-process crash reporter if available
    LaunchCrashReporter();
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
    std::string stamp = MakeTimeStampUtf8();
    std::string prefix = WideToUtf8(g_cfg.dumpPrefix);
    std::string logFile = prefix + stamp + "_assert.log";

    std::ostringstream log;
    log << "================================================================\n";
    log << "           SPARK ENGINE ASSERTION FAILURE (Linux)\n";
    log << "================================================================\n\n";
    log << "Timestamp  : " << stamp << "\n";
    log << "Process ID : " << getpid() << "\n\n";

    if (assertMsg)
    {
        log << "*** ASSERTION FAILURE ***\n" << assertMsg << "\n\n";
    }

    log << CaptureStackTraceString();
    if (g_cfg.captureSystemInfo)
        log << LinuxSystemInfo();

    {
        std::ofstream ofs(logFile, std::ios::out | std::ios::trunc);
        ofs << log.str();
    }

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
    g_cfg = cfg;
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
