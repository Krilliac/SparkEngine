#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
// CrashHandlerHelpers.cpp
#include "Utils/CrashHandler.h"
#include "Utils/Assert.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string.h>
#include <dbghelp.h>
#ifdef SPARK_PLATFORM_WINDOWS
#include <dxgi.h>
#include <d3d11.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <wincodec.h>
#ifdef SPARK_PLATFORM_WINDOWS
#include <wrl/client.h>
#endif // SPARK_PLATFORM_WINDOWS

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
#include <tlhelp32.h>
#include <VersionHelpers.h>
#include <iostream>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dxgi.lib")

//------------------------------------------------------------------------------
// OS Version Helpers (Option B: self‐contained, no winternl.h)
//------------------------------------------------------------------------------

// bring in NTSTATUS
typedef LONG NTSTATUS;

// your private version struct—never collides with SDK headers
typedef struct _CH_OSVERSIONINFOW {
    ULONG dwOSVersionInfoSize;
    ULONG dwMajorVersion;
    ULONG dwMinorVersion;
    ULONG dwBuildNumber;
    ULONG dwPlatformId;
    WCHAR szCSDVersion[128];
} CH_OSVERSIONINFOW, * PCH_OSVERSIONINFOW;

// pointer typedef for the native RtlGetVersion call
using CH_RtlGetVersionPtr = NTSTATUS(NTAPI*)(PCH_OSVERSIONINFOW);

// Try the undocumented native call via GetProcAddress
static bool QueryOsVersionNative(CH_OSVERSIONINFOW& os)
{
    HMODULE hNt = GetModuleHandleW(L"ntdll.dll");
    if (!hNt)
        return false;

    auto fn = reinterpret_cast<CH_RtlGetVersionPtr>(
        GetProcAddress(hNt, "RtlGetVersion")
        );
    if (!fn)
        return false;

    os = {}; // zero all fields
    os.dwOSVersionInfoSize = sizeof(os);

    NTSTATUS status = fn(&os);
    return (status >= 0); // NT_SUCCESS
}

// FIXED: Replace deprecated GetVersionExW with VersionHelpers fallback
static bool QueryOsVersionFallback(CH_OSVERSIONINFOW& os)
{
    os = {};
    os.dwOSVersionInfoSize = sizeof(os);

    // Use VersionHelpers for basic version detection
    if (IsWindows10OrGreater()) {
        os.dwMajorVersion = 10;
        os.dwMinorVersion = 0;
        wcsncpy_s(os.szCSDVersion, L"Windows 10+", _TRUNCATE);
    }
    else if (IsWindows8Point1OrGreater()) {
        os.dwMajorVersion = 6;
        os.dwMinorVersion = 3;
        wcsncpy_s(os.szCSDVersion, L"Windows 8.1+", _TRUNCATE);
    }
    else if (IsWindows8OrGreater()) {
        os.dwMajorVersion = 6;
        os.dwMinorVersion = 2;
        wcsncpy_s(os.szCSDVersion, L"Windows 8+", _TRUNCATE);
    }
    else if (IsWindows7OrGreater()) {
        os.dwMajorVersion = 6;
        os.dwMinorVersion = 1;
        wcsncpy_s(os.szCSDVersion, L"Windows 7+", _TRUNCATE);
    }
    else {
        os.dwMajorVersion = 6;
        os.dwMinorVersion = 0;
        wcsncpy_s(os.szCSDVersion, L"Windows Vista+", _TRUNCATE);
    }

    os.dwPlatformId = VER_PLATFORM_WIN32_NT;
    os.dwBuildNumber = 0; // Cannot determine without deprecated API
    return true;
}

// Public helper: always returns true (zeroes on total failure)
static void GetOsVersion(CH_OSVERSIONINFOW& os)
{
    if (QueryOsVersionNative(os)) return;
    if (QueryOsVersionFallback(os)) return;

    // total failure => zero out
    os = {};
}

//------------------------------------------------------------------------------
// SystemInfo()
//------------------------------------------------------------------------------

// Compose a human-readable string with OS, CPU, RAM, and GPU details
static std::wstring SystemInfo()
{
    // 1) OS Version
    CH_OSVERSIONINFOW os = {};
    GetOsVersion(os);

    std::wstringstream ss;
    ss << L"OS Version: "
        << os.dwMajorVersion << L"." << os.dwMinorVersion
        << L" (Build " << os.dwBuildNumber << L") "
        << os.szCSDVersion << L"\n";

    // 2) CPU & Architecture
    SYSTEM_INFO sysInfo = {};
    GetNativeSystemInfo(&sysInfo);

    const wchar_t* arch = L"Unknown";
    switch (sysInfo.wProcessorArchitecture)
    {
    case PROCESSOR_ARCHITECTURE_AMD64: arch = L"x64"; break;
    case PROCESSOR_ARCHITECTURE_INTEL: arch = L"x86"; break;
    case PROCESSOR_ARCHITECTURE_ARM64: arch = L"ARM64"; break;
    case PROCESSOR_ARCHITECTURE_ARM: arch = L"ARM"; break;
    }

    ss << L"CPU: " << arch
        << L", " << sysInfo.dwNumberOfProcessors << L" logical cores\n";

    // 3) Physical Memory
    MEMORYSTATUSEX mem = {};
    mem.dwLength = sizeof(mem);
    GlobalMemoryStatusEx(&mem);

    ss << L"Memory: "
        << (mem.ullTotalPhys / (static_cast<ULONGLONG>(1024) * 1024)) << L" MB total, "
        << (mem.ullAvailPhys / (static_cast<ULONGLONG>(1024) * 1024)) << L" MB available\n";

    // 4) Primary GPU via DXGI
    IDXGIFactory* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory))))
    {
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(factory->EnumAdapters(0, &adapter)))
        {
            DXGI_ADAPTER_DESC desc = {};
            adapter->GetDesc(&desc);
            ss << L"GPU: " << desc.Description << L"\n";
            adapter->Release();
        }
        factory->Release();
    }
    else
    {
        ss << L"GPU: Unknown (DXGI factory failed)\n";
    }

    return ss.str();
}

//------------------------------------------------------------------------------
// UTF-8 Conversion Helper
//------------------------------------------------------------------------------
static std::string WideToUtf8(const std::wstring& wstr)
{
    if (wstr.empty()) return {};
    int size = WideCharToMultiByte(
        CP_UTF8, 0,
        wstr.c_str(), -1,
        nullptr, 0,
        nullptr, nullptr
    );
    std::string utf8(size, 0);
    WideCharToMultiByte(
        CP_UTF8, 0,
        wstr.c_str(), -1,
        &utf8[0], size,
        nullptr, nullptr
    );
    return utf8;
}

//------------------------------------------------------------------------------
// Crash Handler Implementation
//------------------------------------------------------------------------------

static CrashConfig g_cfg;
static std::mutex g_lock;

// Externals from your engine
extern IDXGISwapChain* GetMainSwapChain();
extern ID3D11Device* GetD3DDevice();
extern ID3D11DeviceContext* GetD3DContext();

// Forward declarations
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS*);
static void HandleCrashInternal(EXCEPTION_POINTERS*, const char*);
static void WriteMiniDump(const std::wstring&, EXCEPTION_POINTERS*);
static std::wstring MakeTimeStamp();
static std::wstring SymStackTrace(EXCEPTION_POINTERS*);
static std::wstring ThreadStacks();
static void SaveScreenshot(const std::wstring&);
static void ZipFiles(const std::wstring&, const std::vector<std::wstring>&);

#ifdef NETWORKING_ENABLED
static bool Upload(const std::string&, const std::wstring&, const std::string&);
#endif
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep)
{
    HandleCrashInternal(ep, nullptr);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void HandleCrashInternal(EXCEPTION_POINTERS* ep, const char* msg)
{
    std::lock_guard<std::mutex> lock(g_lock);
    if (!ep) return;

    auto stamp = MakeTimeStamp();
    std::wstring dump = g_cfg.dumpPrefix + stamp + L".dmp";
    std::wstring logFile = g_cfg.dumpPrefix + stamp + L".log";
    std::wstring shot = g_cfg.dumpPrefix + stamp + L".png";
    std::wstring zipFile = g_cfg.dumpPrefix + stamp + L".zip";

    WriteMiniDump(dump, ep);

    std::wstringstream log;
    if (msg) {
        int len = MultiByteToWideChar(CP_UTF8, 0, msg, -1, nullptr, 0);
        std::wstring wmsg(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, msg, -1, &wmsg[0], len);
        log << L"*** Assertion ***\n" << wmsg << L"\n\n";
    }
    log << SymStackTrace(ep);
    if (g_cfg.captureSystemInfo) log << SystemInfo();
    if (g_cfg.captureAllThreads) log << ThreadStacks();

    std::wofstream ofs(logFile, std::ios::out | std::ios::trunc);
    if (ofs) {
        ofs << log.str();
    }
    else {
        MessageBoxW(nullptr,
            L"Failed to open crash log file",
            L"CrashHandler",
            MB_OK | MB_ICONERROR);
    }

    if (g_cfg.captureScreenshot)
        SaveScreenshot(shot);

    std::vector<std::wstring> files{ dump, logFile };
    if (g_cfg.captureScreenshot)
        files.push_back(shot);
    if (g_cfg.zipBeforeUpload)
        ZipFiles(zipFile, files);

    bool ok = true;
#ifdef NETWORKING_ENABLED
    if (!g_cfg.uploadURL.empty()) {
        if (g_cfg.zipBeforeUpload)
            ok = Upload(g_cfg.uploadURL, zipFile, "package");
        else {
            ok &= Upload(g_cfg.uploadURL, dump, "minidump");
            ok &= Upload(g_cfg.uploadURL, logFile, "logfile");
            if (g_cfg.captureScreenshot)
                ok &= Upload(g_cfg.uploadURL, shot, "screenshot");
        }
    }
#else
    // When networking is disabled, mark upload as successful (no-op)
    if (!g_cfg.uploadURL.empty()) {
        ok = true; // Consider it successful since we can't upload
    }
#endif

    std::wstring msgBox = msg ? L"Assertion captured.\n"
        : L"Crash captured.\n";
    msgBox += L"Files:\n" + dump + L"\n" + logFile;
    if (g_cfg.captureScreenshot)
        msgBox += L"\n" + shot;
        
#ifdef NETWORKING_ENABLED
    if (!g_cfg.uploadURL.empty())
        msgBox += L"\nUpload: " + std::wstring(ok ? L"Success" : L"FAILED");
#else
    if (!g_cfg.uploadURL.empty())
        msgBox += L"\nUpload: Disabled (networking not enabled)";
#endif

    MessageBoxW(nullptr,
        msgBox.c_str(),
        msg ? L"Assertion" : L"Crash",
        MB_OK | MB_ICONERROR);
}

//------------------------------------------------------------------------------
// MiniDump Writer
//------------------------------------------------------------------------------

static void WriteMiniDump(const std::wstring& file, EXCEPTION_POINTERS* ep)
{
    HANDLE hFile = CreateFileW(
        file.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (hFile == INVALID_HANDLE_VALUE)
        return;

    MINIDUMP_EXCEPTION_INFORMATION info{};
    info.ThreadId = GetCurrentThreadId();
    info.ExceptionPointers = ep;
    info.ClientPointers = TRUE;

    MINIDUMP_TYPE dumpType =
        static_cast<MINIDUMP_TYPE>(
            MiniDumpWithFullMemory |
            MiniDumpWithHandleData |
            MiniDumpWithUnloadedModules
            );

    MiniDumpWriteDump(
        GetCurrentProcess(),
        GetCurrentProcessId(),
        hFile,
        dumpType,
        &info,
        nullptr,
        nullptr
    );

    CloseHandle(hFile);
}

//------------------------------------------------------------------------------
// Timestamp Helper
//------------------------------------------------------------------------------

static std::wstring MakeTimeStamp()
{
    SYSTEMTIME t;
    GetLocalTime(&t);

    wchar_t buf[32];
    swprintf_s(buf, L"_%04d%02d%02d_%02d%02d%02d",
        t.wYear, t.wMonth, t.wDay,
        t.wHour, t.wMinute, t.wSecond);
    return buf;
}

//------------------------------------------------------------------------------
// Single-Thread Stack Trace (FIXED)
//------------------------------------------------------------------------------

static std::wstring SymStackTrace(EXCEPTION_POINTERS* ep)
{
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);

    std::wstringstream out;
    out << L"*** Stack Trace ***\n";

    CONTEXT& c = *ep->ContextRecord;
    STACKFRAME64 f{};
    DWORD m;

#ifdef _WIN64
    m = IMAGE_FILE_MACHINE_AMD64;
    f.AddrPC.Offset = c.Rip;
    f.AddrFrame.Offset = c.Rbp;
    f.AddrStack.Offset = c.Rsp;
#else
    m = IMAGE_FILE_MACHINE_I386;
    f.AddrPC.Offset = c.Eip;
    f.AddrFrame.Offset = c.Ebp;
    f.AddrStack.Offset = c.Esp;
#endif

    f.AddrPC.Mode = f.AddrFrame.Mode = f.AddrStack.Mode = AddrModeFlat;

    // FIXED: Pre-allocate buffer outside loop instead of alloca in loop
    BYTE bufSym[sizeof(SYMBOL_INFO) + 256] = {};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(bufSym);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;

    for (int i = 0; i < 32; ++i) {
        if (!StackWalk64(
            m,
            GetCurrentProcess(),
            GetCurrentThread(),
            &f,
            &c,
            nullptr,
            SymFunctionTableAccess64,
            SymGetModuleBase64,
            nullptr
        ) || !f.AddrPC.Offset)
        {
            break;
        }

        DWORD64 disp = 0;
        if (SymFromAddr(
            GetCurrentProcess(),
            f.AddrPC.Offset,
            &disp,
            sym
        ))
        {
            out << L" " << sym->Name
                << L"+0x" << std::hex << disp << std::dec << L"\n";
        }
        else {
            out << L" 0x" << std::hex
                << f.AddrPC.Offset << std::dec << L"\n";
        }
    }

    SymCleanup(GetCurrentProcess());
    return out.str();
}

//------------------------------------------------------------------------------
// All-Threads Stack Trace (FIXED)
//------------------------------------------------------------------------------

static std::wstring ThreadStacks()
{
    std::wstringstream out;
    out << L"\n*** Thread Stacks ***\n";

    DWORD pid = GetCurrentProcessId();
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hSnap == INVALID_HANDLE_VALUE)
        return out.str();

    // FIXED: Pre-allocate buffer outside loops
    BYTE bufSym[sizeof(SYMBOL_INFO) + 256] = {};

    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    if (Thread32First(hSnap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid &&
                te.th32ThreadID != GetCurrentThreadId())
            {
                out << L"\n-- Thread " << te.th32ThreadID << L" --\n";

                HANDLE hThread = OpenThread(
                    THREAD_SUSPEND_RESUME |
                    THREAD_GET_CONTEXT |
                    THREAD_QUERY_INFORMATION,
                    FALSE,
                    te.th32ThreadID
                );
                if (!hThread) continue;

                SuspendThread(hThread);
                CONTEXT ctx = {};
                ctx.ContextFlags = CONTEXT_FULL;
                if (GetThreadContext(hThread, &ctx)) {
                    SymInitialize(GetCurrentProcess(), nullptr, TRUE);

                    STACKFRAME64 f{};
                    DWORD m;
#ifdef _WIN64
                    m = IMAGE_FILE_MACHINE_AMD64;
                    f.AddrPC.Offset = ctx.Rip;
                    f.AddrFrame.Offset = ctx.Rbp;
                    f.AddrStack.Offset = ctx.Rsp;
#else
                    m = IMAGE_FILE_MACHINE_I386;
                    f.AddrPC.Offset = ctx.Eip;
                    f.AddrFrame.Offset = ctx.Ebp;
                    f.AddrStack.Offset = ctx.Esp;
#endif
                    f.AddrPC.Mode = f.AddrFrame.Mode = f.AddrStack.Mode = AddrModeFlat;

                    // FIXED: Reuse pre-allocated buffer instead of alloca in loop
                    auto* sym = reinterpret_cast<SYMBOL_INFO*>(bufSym);
                    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                    sym->MaxNameLen = 255;

                    for (int i = 0; i < 32; ++i) {
                        if (!StackWalk64(
                            m,
                            GetCurrentProcess(),
                            hThread,
                            &f,
                            &ctx,
                            nullptr,
                            SymFunctionTableAccess64,
                            SymGetModuleBase64,
                            nullptr
                        ) || !f.AddrPC.Offset)
                        {
                            break;
                        }

                        DWORD64 disp = 0;
                        if (SymFromAddr(
                            GetCurrentProcess(),
                            f.AddrPC.Offset,
                            &disp,
                            sym
                        ))
                        {
                            out << L" " << sym->Name
                                << L"+0x" << std::hex << disp << std::dec << L"\n";
                        }
                        else {
                            out << L" 0x" << std::hex
                                << f.AddrPC.Offset << std::dec << L"\n";
                        }
                    }
                    SymCleanup(GetCurrentProcess());
                }
                ResumeThread(hThread);
                CloseHandle(hThread);
            }
            te.dwSize = sizeof(te);
        } while (Thread32Next(hSnap, &te));
    }
    CloseHandle(hSnap);

    out << L"\n";
    return out.str();
}

//------------------------------------------------------------------------------
// Screenshot via D3D11 + WIC
//------------------------------------------------------------------------------

static void SaveScreenshot(const std::wstring& file)
{
    auto* swap = GetMainSwapChain();
    auto* device = GetD3DDevice();
    auto* ctx = GetD3DContext();
    if (!swap || !device || !ctx) return;

    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer)))
        return;

    D3D11_TEXTURE2D_DESC desc;
    backBuffer->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;

    ID3D11Texture2D* staging = nullptr;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) {
        backBuffer->Release(); return;
    }

    ctx->CopyResource(staging, backBuffer);
    backBuffer->Release();

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        staging->Release(); return;
    }

    HRESULT hrCom = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    (void)hrCom;

    IWICImagingFactory* wicFactory = nullptr;
    if (FAILED(CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wicFactory))))
    {
        ctx->Unmap(staging, 0);
        staging->Release();
        CoUninitialize();
        return;
    }

    IWICBitmap* wicBitmap = nullptr;
    if (FAILED(wicFactory->CreateBitmapFromMemory(
        desc.Width, desc.Height,
        GUID_WICPixelFormat32bppBGRA,
        mapped.RowPitch,
        mapped.RowPitch * desc.Height,
        reinterpret_cast<BYTE*>(mapped.pData),
        &wicBitmap)))
    {
        ctx->Unmap(staging, 0);
        staging->Release();
        wicFactory->Release();
        CoUninitialize();
        return;
    }

    IWICStream* wicStream = nullptr;
    if (FAILED(wicFactory->CreateStream(&wicStream))) {
        wicBitmap->Release();
        ctx->Unmap(staging, 0);
        staging->Release();
        wicFactory->Release();
        CoUninitialize();
        return;
    }

    if (FAILED(wicStream->InitializeFromFilename(
        file.c_str(),
        GENERIC_WRITE)))
    {
        wicStream->Release();
        wicBitmap->Release();
        ctx->Unmap(staging, 0);
        staging->Release();
        wicFactory->Release();
        CoUninitialize();
        return;
    }

    IWICBitmapEncoder* encoder = nullptr;
    if (FAILED(wicFactory->CreateEncoder(
        GUID_ContainerFormatPng,
        nullptr,
        &encoder)))
    {
        wicStream->Release();
        wicBitmap->Release();
        ctx->Unmap(staging, 0);
        staging->Release();
        wicFactory->Release();
        CoUninitialize();
        return;
    }

    if (FAILED(encoder->Initialize(
        wicStream,
        WICBitmapEncoderNoCache)))
    {
        encoder->Release();
        wicStream->Release();
        wicBitmap->Release();
        ctx->Unmap(staging, 0);
        staging->Release();
        wicFactory->Release();
        CoUninitialize();
        return;
    }

    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;
    if (FAILED(encoder->CreateNewFrame(&frame, &props))) {
        encoder->Release();
        wicStream->Release();
        wicBitmap->Release();
        ctx->Unmap(staging, 0);
        staging->Release();
        wicFactory->Release();
        CoUninitialize();
        return;
    }

    frame->Initialize(props);
    frame->SetSize(desc.Width, desc.Height);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&fmt);
    frame->WriteSource(wicBitmap, nullptr);
    frame->Commit();
    encoder->Commit();

    frame->Release();
    props->Release();
    encoder->Release();
    wicStream->Release();
    wicBitmap->Release();
    ctx->Unmap(staging, 0);
    staging->Release();
    wicFactory->Release();
    CoUninitialize();
}

//------------------------------------------------------------------------------
// Zip Files via miniz
//------------------------------------------------------------------------------

static void ZipFiles(
    const std::wstring& zipPath,
    const std::vector<std::wstring>& files)
{
    std::string zipUtf8 = WideToUtf8(zipPath);
    mz_zip_archive zip = {};
    if (!mz_zip_writer_init_file(&zip, zipUtf8.c_str(), 0))
        return;

    for (auto& wfile : files) {
        std::filesystem::path p(wfile);
        std::string nameUtf8 = WideToUtf8(p.filename().wstring());
        std::string fileUtf8 = WideToUtf8(wfile);
        mz_zip_writer_add_file(
            &zip,
            nameUtf8.c_str(),
            fileUtf8.c_str(),
            nullptr,
            0,
            MZ_BEST_COMPRESSION
        );
    }

    mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
}

//------------------------------------------------------------------------------
// Upload via libcurl (only if networking is enabled)
//------------------------------------------------------------------------------

#ifdef NETWORKING_ENABLED
static bool Upload(
    const std::string& url,
    const std::wstring& wfile,
    const std::string& fieldName)
{
    std::string fileUtf8 = WideToUtf8(wfile);
    std::filesystem::path p(wfile);
    std::string filenameUtf8 = WideToUtf8(p.filename().wstring());

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    curl_mime* mime = curl_mime_init(curl);
    curl_mimepart* part = curl_mime_addpart(mime);
    curl_mime_name(part, fieldName.c_str());
    curl_mime_filedata(part, fileUtf8.c_str());
    curl_mime_filename(part, filenameUtf8.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "CrashHandler/1.0");

    CURLcode res = curl_easy_perform(curl);

    curl_mime_free(mime);
    curl_easy_cleanup(curl);

    return (res == CURLE_OK);
}
#endif // NETWORKING_ENABLED
#endif // SPARK_PLATFORM_WINDOWS
