#include "Utils/CrashHandler.h"
#include "Utils/Assert.h"
#include "Utils/SparkError.h"
#include "Utils/ConsoleProcessManager.h"

#include <windows.h>
#include <dbghelp.h>
#include <dxgi.h>
#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>

// Only include CURL when networking is enabled
#ifdef NETWORKING_ENABLED
#include <curl/curl.h>
#endif

#include <miniz.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <mutex>
#include <VersionHelpers.h>
#include <TlHelp32.h>
#include <iostream>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dxgi.lib")

static CrashConfig g_cfg;
static std::mutex g_lock;
static bool g_triggerCrashOnAssert = false; // NEW: Control assert crash behavior

// Engine must implement these
extern IDXGISwapChain* GetMainSwapChain();
extern ID3D11Device* GetD3DDevice();
extern ID3D11DeviceContext* GetD3DContext();

// Helper: convert wide string to UTF-8
static std::string WideToUtf8(const std::wstring& w) {
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], len, nullptr, nullptr);
    return s;
}

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

#ifdef NETWORKING_ENABLED
static bool Upload(const std::string& url, const std::wstring& file, const std::string& field);
#endif

void InstallCrashHandler(const CrashConfig& cfg) {
    g_cfg = cfg;
    g_triggerCrashOnAssert = cfg.triggerCrashOnAssert; // Store the flag
    
#ifdef NETWORKING_ENABLED
    curl_global_init(CURL_GLOBAL_DEFAULT);
#endif
    
    SetUnhandledExceptionFilter(CrashFilter); // This stays active always
}

void TriggerCrashHandler(const char* assertMsg) {
    if (!g_triggerCrashOnAssert) {
        // Just log the assertion but don't trigger full crash handling
        std::string logMsg = "Assert triggered but crash handling disabled: ";
        if (assertMsg) {
            logMsg += assertMsg;
        }
        
        // **NEW: Log to external console**
        try {
            Spark::ConsoleProcessManager::GetInstance().LogCrash(logMsg);
        } catch (...) {
            // Fallback to OutputDebugString if console logging fails
            OutputDebugStringA(logMsg.c_str());
            OutputDebugStringA("\n");
        }
        
        return; // Early exit - no crash, no dumps, just continue
    }

    // Full crash handling when enabled
    EXCEPTION_RECORD rec{};
    rec.ExceptionCode = STATUS_FATAL_APP_EXIT;
    rec.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    rec.ExceptionAddress = _ReturnAddress();

    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    RtlCaptureContext(&ctx);

    EXCEPTION_POINTERS ep{ &rec, &ctx };
    HandleCrashInternal(&ep, assertMsg);
}

void SetAssertCrashBehavior(bool shouldCrash) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_triggerCrashOnAssert = shouldCrash;
    
    // **NEW: Log the change to external console**
    try {
        std::string logMsg = "Assert crash behavior changed to: ";
        logMsg += (shouldCrash ? "ENABLED" : "DISABLED");
        Spark::ConsoleProcessManager::GetInstance().LogCrash(logMsg);
    } catch (...) {
        // Fallback if console logging fails
    }
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    HandleCrashInternal(ep, nullptr);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void HandleCrashInternal(EXCEPTION_POINTERS* ep, const char* assertMsg) {
    std::lock_guard<std::mutex> guard(g_lock);
    if (!ep) return;

    std::wstring stamp = MakeTimeStamp();
    std::wstring dump = g_cfg.dumpPrefix + stamp + L".dmp";
    std::wstring logFile = g_cfg.dumpPrefix + stamp + L".log";
    std::wstring shot = g_cfg.dumpPrefix + stamp + L".png";
    std::wstring zipFile = g_cfg.dumpPrefix + stamp + L".zip";

    WriteMiniDump(dump, ep);

    // Build log with enhanced crash context
    std::wstringstream log;
    log << L"================================================================\n";
    log << L"           SPARK ENGINE CRASH REPORT\n";
    log << L"================================================================\n\n";

    // Timestamp
    log << L"Timestamp  : " << stamp << L"\n";
    log << L"Process ID : " << GetCurrentProcessId() << L"\n";
    log << L"Thread ID  : 0x" << std::hex << GetCurrentThreadId() << std::dec << L"\n\n";

    if (assertMsg) {
        int len = MultiByteToWideChar(CP_UTF8, 0, assertMsg, -1, nullptr, 0);
        std::wstring wmsg(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, assertMsg, -1, &wmsg[0], len);
        log << L"*** ASSERTION FAILURE ***\n" << wmsg << L"\n\n";
    } else {
        log << L"*** CRASH DETECTED ***\n\n";
    }

    // Decode exception code into human-readable name
    if (ep->ExceptionRecord) {
        DWORD code = ep->ExceptionRecord->ExceptionCode;
        const char* codeName = SparkError::ExceptionCodeToString(code);

        int codeNameLen = MultiByteToWideChar(CP_UTF8, 0, codeName, -1, nullptr, 0);
        std::wstring wCodeName(codeNameLen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, codeName, -1, &wCodeName[0], codeNameLen);

        log << L"Exception Code    : 0x" << std::hex << code << std::dec << L"\n";
        log << L"Exception Name    : " << wCodeName << L"\n";
        log << L"Exception Address : 0x" << std::hex
            << reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress)
            << std::dec << L"\n";
        log << L"Exception Flags   : " << ep->ExceptionRecord->ExceptionFlags << L"\n";
        log << L"Number Parameters : " << ep->ExceptionRecord->NumberParameters << L"\n";

        // For access violations, decode read/write and target address
        if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
            ULONG_PTR accessType = ep->ExceptionRecord->ExceptionInformation[0];
            ULONG_PTR targetAddr = ep->ExceptionRecord->ExceptionInformation[1];
            const wchar_t* accessStr = (accessType == 0) ? L"READ" :
                                       (accessType == 1) ? L"WRITE" :
                                       (accessType == 8) ? L"DEP_VIOLATION" : L"UNKNOWN";
            log << L"Access Type       : " << accessStr << L"\n";
            log << L"Target Address    : 0x" << std::hex << targetAddr << std::dec << L"\n";
        }
        log << L"\n";
    }

    // Process memory snapshot
    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        log << L"*** PROCESS MEMORY ***\n";
        log << L"Working Set       : " << (pmc.WorkingSetSize >> 20) << L" MiB\n";
        log << L"Peak Working Set  : " << (pmc.PeakWorkingSetSize >> 20) << L" MiB\n";
        log << L"Private Bytes     : " << (pmc.PrivateUsage >> 20) << L" MiB\n";
        log << L"Page Faults       : " << pmc.PageFaultCount << L"\n\n";
    }

    // Module that faulted
    if (ep->ExceptionRecord && ep->ExceptionRecord->ExceptionAddress) {
        HMODULE hMod = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)ep->ExceptionRecord->ExceptionAddress, &hMod)) {
            wchar_t modPath[MAX_PATH] = {};
            GetModuleFileNameW(hMod, modPath, MAX_PATH);
            log << L"Faulting Module   : " << modPath << L"\n\n";
        }
    }

    log << SymStackTrace(ep);
    if (g_cfg.captureSystemInfo) log << SystemInfo();
    if (g_cfg.captureAllThreads) log << ThreadStacks();

    // **NEW: Log crash info to external console**
    try {
        std::string crashSummary = assertMsg ? "ASSERTION FAILURE" : "CRASH DETECTED";
        crashSummary += "\nDump file: " + WideToUtf8(dump);
        crashSummary += "\nLog file: " + WideToUtf8(logFile);
        if (g_cfg.captureScreenshot) {
            crashSummary += "\nScreenshot: " + WideToUtf8(shot);
        }
        
        Spark::ConsoleProcessManager::GetInstance().LogCrash(crashSummary);
        
        // Also log the actual crash details (truncated for console)
        std::string logContent = WideToUtf8(log.str());
        if (logContent.length() > 1000) {
            logContent = logContent.substr(0, 1000) + "... (truncated, see log file for full details)";
        }
        Spark::ConsoleProcessManager::GetInstance().LogCrash(logContent);
        
    } catch (...) {
        // If external console logging fails, continue with normal crash handling
    }

    // Write log file
    {
        std::wofstream ofs(logFile, std::ios::out | std::ios::trunc);
        ofs << log.str();
    }

    if (g_cfg.captureScreenshot) SaveScreenshot(shot);

    // Package files
    std::vector<std::wstring> files{ dump, logFile };
    if (g_cfg.captureScreenshot) files.push_back(shot);
    if (g_cfg.zipBeforeUpload) ZipFiles(zipFile, files);

    // Optional upload (only if networking is enabled)
    bool ok = true;
#ifdef NETWORKING_ENABLED
    if (!g_cfg.uploadURL.empty()) {
        if (g_cfg.zipBeforeUpload)
            ok = Upload(g_cfg.uploadURL, zipFile, "package");
        else {
            ok &= Upload(g_cfg.uploadURL, dump, "minidump");
            ok &= Upload(g_cfg.uploadURL, logFile, "logfile");
            if (g_cfg.captureScreenshot) ok &= Upload(g_cfg.uploadURL, shot, "screenshot");
        }
    }
#else
    // When networking is disabled, mark upload as successful (no-op)
    if (!g_cfg.uploadURL.empty()) {
        ok = true; // Consider it successful since we can't upload
    }
#endif

    // Notify user
    std::wstring msg = assertMsg ? L"Assertion captured.\n" : L"Crash captured.\n";
    msg += L"Files:\n" + dump + L"\n" + logFile;
    if (g_cfg.captureScreenshot) msg += L"\n" + shot;
    
#ifdef NETWORKING_ENABLED
    if (!g_cfg.uploadURL.empty()) msg += L"\nUpload: " + std::wstring(ok ? L"Success" : L"FAILED");
#else
    if (!g_cfg.uploadURL.empty()) msg += L"\nUpload: Disabled (networking not enabled)";
#endif

    MessageBoxW(nullptr, msg.c_str(),
        assertMsg ? L"Assertion Handler" : L"Crash Handler",
        MB_OK | MB_ICONERROR);
}

static void WriteMiniDump(const std::wstring& file, EXCEPTION_POINTERS* ep) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    MINIDUMP_EXCEPTION_INFORMATION info{ GetCurrentThreadId(), ep, TRUE };
    MiniDumpWriteDump(GetCurrentProcess(),
        GetCurrentProcessId(),
        h,
        static_cast<MINIDUMP_TYPE>(
            MiniDumpWithFullMemory |
            MiniDumpWithHandleData |
            MiniDumpWithUnloadedModules
            ),
        &info,
        nullptr,
        nullptr);
    CloseHandle(h);
}

static std::wstring MakeTimeStamp() {
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t buf[32];
    swprintf_s(buf, L"_%04d%02d%02d_%02d%02d%02d",
        t.wYear, t.wMonth, t.wDay,
        t.wHour, t.wMinute, t.wSecond);
    return buf;
}

// FIXED: Replace alloca usage in loops with pre-allocated buffer
static std::wstring SymStackTrace(EXCEPTION_POINTERS* ep) {
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

    // FIXED: Pre-allocate buffer outside loop instead of alloca in loop
    BYTE symBuffer[sizeof(SYMBOL_INFO) + 256] = {};
    SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(symBuffer);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;

    for (int i = 0; i < 32; ++i) {
        if (!StackWalk64(machine,
            GetCurrentProcess(),
            GetCurrentThread(),
            &frame,
            &ctx,
            nullptr,
            SymFunctionTableAccess64,
            SymGetModuleBase64,
            nullptr) ||
            !frame.AddrPC.Offset) break;

        DWORD64 disp = 0;
        if (SymFromAddr(GetCurrentProcess(), frame.AddrPC.Offset, &disp, sym)) {
            out << L" " << sym->Name << L" +0x"
                << std::hex << disp << std::dec << L"\n";
        }
        else {
            out << L" 0x" << std::hex << frame.AddrPC.Offset << std::dec << L"\n";
        }
    }

    SymCleanup(GetCurrentProcess());
    return out.str();
}

// FIXED: Replace GetVersionExW with IsWindows* functions
static std::wstring SystemInfo() {
    SYSTEM_INFO si; GetNativeSystemInfo(&si);
    MEMORYSTATUSEX ms{ sizeof(ms) }; GlobalMemoryStatusEx(&ms);

    std::wstring gpu = L"Unknown GPU";
    IDXGIFactory* fac = nullptr;
    if (SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&fac))) {
        IDXGIAdapter* adp = nullptr;
        if (fac->EnumAdapters(0, &adp) != DXGI_ERROR_NOT_FOUND) {
            DXGI_ADAPTER_DESC desc; adp->GetDesc(&desc);
            gpu = desc.Description;
            adp->Release();
        }
        fac->Release();
    }

    std::wstringstream s;
    s << L"*** SYSTEM INFO ***\n";

    // FIXED: Use VersionHelpers instead of deprecated GetVersionExW
    if (IsWindows10OrGreater()) {
        s << L"OS Version: Windows 10 or greater\n";
    }
    else if (IsWindows8Point1OrGreater()) {
        s << L"OS Version: Windows 8.1 or greater\n";
    }
    else if (IsWindows8OrGreater()) {
        s << L"OS Version: Windows 8 or greater\n";
    }
    else if (IsWindows7OrGreater()) {
        s << L"OS Version: Windows 7 or greater\n";
    }
    else {
        s << L"OS Version: Windows (version unknown)\n";
    }

    s << L"CPU Cores : " << si.dwNumberOfProcessors << L"\n"
        << L"RAM Total : " << (ms.ullTotalPhys >> 20) << L" MiB\n"
        << L"RAM Avail : " << (ms.ullAvailPhys >> 20) << L" MiB\n"
        << L"GPU : " << gpu << L"\n\n";
    return s.str();
}

// FIXED: Replace alloca usage in loops
static std::wstring ThreadStacks() {
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return L"*** THREAD STACKS ***\nFailed to create snapshot\n";

    std::wstringstream out;
    out << L"*** THREAD STACKS ***\n";

    // FIXED: Pre-allocate buffer outside loops
    BYTE symBuffer[sizeof(SYMBOL_INFO) + 256] = {};

    THREADENTRY32 te{ sizeof(te) };
    for (Thread32First(snap, &te); Thread32Next(snap, &te);) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE th = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
        if (!th) continue;
        SuspendThread(th);

        CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(th, &ctx)) {
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

            // FIXED: Reuse pre-allocated buffer instead of alloca in loop
            SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(symBuffer);
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 255;

            for (int i = 0; i < 32; ++i) {
                if (!StackWalk64(mach,
                    GetCurrentProcess(),
                    th,
                    &sf,
                    &ctx,
                    nullptr,
                    SymFunctionTableAccess64,
                    SymGetModuleBase64,
                    nullptr) ||
                    !sf.AddrPC.Offset) break;

                DWORD64 disp = 0;
                if (SymFromAddr(GetCurrentProcess(), sf.AddrPC.Offset, &disp, sym)) {
                    out << L" " << sym->Name << L" +0x"
                        << std::hex << disp << std::dec << L"\n";
                }
                else {
                    out << L" 0x" << std::hex << sf.AddrPC.Offset << std::dec << L"\n";
                }
            }
        }

        ResumeThread(th);
        CloseHandle(th);
    }
    CloseHandle(snap);
    SymCleanup(GetCurrentProcess());
    return out.str();
}

static void SaveScreenshot(const std::wstring& file) {
    auto sc = GetMainSwapChain();
    if (!sc) return;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), &back))) return;

    D3D11_TEXTURE2D_DESC d; back->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.BindFlags = d.MiscFlags = 0;

    auto dev = GetD3DDevice();
    auto ctx = GetD3DContext();
    Microsoft::WRL::ComPtr<ID3D11Texture2D> cpu;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &cpu))) return;
    ctx->CopyResource(cpu.Get(), back.Get());

    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(cpu.Get(), 0, D3D11_MAP_READ, 0, &m))) return;

    HRESULT hrCom = CoInitializeEx(NULL, COINIT_MULTITHREADED); (void)hrCom;
    IWICImagingFactory* wic = nullptr;
    IWICStream* stm = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frm = nullptr;

    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,
        nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wic))) &&
        SUCCEEDED(wic->CreateStream(&stm)) &&
        SUCCEEDED(stm->InitializeFromFilename(file.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
        SUCCEEDED(enc->Initialize(stm, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&frm, nullptr)) &&
        SUCCEEDED(frm->Initialize(nullptr)) &&
        SUCCEEDED(frm->SetSize(d.Width, d.Height)))
    {
        WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
        frm->SetPixelFormat(&pf);
        frm->WritePixels(d.Height, m.RowPitch,
            static_cast<UINT>(m.RowPitch * d.Height),
            reinterpret_cast<BYTE*>(m.pData));
        frm->Commit(); enc->Commit();
    }

    if (frm) frm->Release();
    if (enc) enc->Release();
    if (stm) stm->Release();
    if (wic) wic->Release();
    ctx->Unmap(cpu.Get(), 0);
}

static void ZipFiles(const std::wstring& zip, const std::vector<std::wstring>& files) {
    mz_zip_archive za{};
    std::string zipUtf = WideToUtf8(zip);
    if (!mz_zip_writer_init_file(&za, zipUtf.c_str(), 0)) return;
    for (const auto& f : files) {
        if (std::filesystem::exists(f)) {
            std::string fileUtf = WideToUtf8(f);
            std::string entry = WideToUtf8(std::filesystem::path(f).filename().wstring());
            mz_zip_writer_add_file(&za,
                entry.c_str(),
                fileUtf.c_str(),
                nullptr, 0,
                MZ_BEST_COMPRESSION);
        }
    }
    mz_zip_writer_finalize_archive(&za);
    mz_zip_writer_end(&za);
}

#ifdef NETWORKING_ENABLED
static bool Upload(const std::string& url,
    const std::wstring& file,
    const std::string& field) {
    CURL* c = curl_easy_init();
    if (!c) return false;
    curl_mime* mime = curl_mime_init(c);
    curl_mimepart* part = curl_mime_addpart(mime);
    curl_mime_name(part, field.c_str());
    std::string path = WideToUtf8(file);
    curl_mime_filedata(part, path.c_str());
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, g_cfg.connectTimeoutSeconds);
    CURLcode res = curl_easy_perform(c);
    curl_mime_free(mime);
    curl_easy_cleanup(c);
    return (res == CURLE_OK);
}
#endif