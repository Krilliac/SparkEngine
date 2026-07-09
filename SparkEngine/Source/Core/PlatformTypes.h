/**
 * @file PlatformTypes.h
 * @brief Win32 type aliases and macros for non-Windows platforms
 *
 * Provides lightweight stubs for Win32 types (BOOL, DWORD, HANDLE, HWND, etc.),
 * HRESULT macros, virtual key codes, window message constants, and other
 * Windows-specific definitions so that engine code compiles on Linux and macOS.
 */

#pragma once

#ifndef SPARK_PLATFORM_WINDOWS

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <cmath>

#ifdef SPARK_SDL2_AVAILABLE
#include <SDL.h>
#endif

// --- Basic Win32 types ---
// Apple framework headers define BOOL themselves. Defining the Win32 alias first
// can break Metal/Cocoa translation units, so keep it to non-Apple platforms.
#if !defined(__APPLE__)
using BOOL = int;
#endif
using BYTE = unsigned char;
using WORD = unsigned short;
using DWORD = unsigned long;
using UINT = unsigned int;
using INT = int;
using LONG = long;
using ULONG = unsigned long;
using FLOAT = float;
using CHAR = char;
using WCHAR = wchar_t;
using LPWSTR = wchar_t*;
using LPCWSTR = const wchar_t*;
using LPSTR = char*;
using LPCSTR = const char*;
using LPVOID = void*;
using LPCVOID = const void*;
using HANDLE = void*;
using HMODULE = void*;
using SIZE_T = size_t;

// --- HRESULT ---
// Must be 32-bit signed to match the Windows ABI. On 64-bit Linux, 'long'
// is 8 bytes which makes error codes like E_FAIL (0x80004005) positive —
// breaking SUCCEEDED()/FAILED() entirely. Use int32_t for correctness.
using HRESULT = int32_t;

#ifndef S_OK
#define S_OK ((HRESULT)0)
#endif
#ifndef S_FALSE
#define S_FALSE ((HRESULT)1)
#endif
#ifndef E_FAIL
#define E_FAIL ((HRESULT)0x80004005)
#endif
#ifndef E_INVALIDARG
#define E_INVALIDARG ((HRESULT)0x80070057)
#endif
#ifndef E_OUTOFMEMORY
#define E_OUTOFMEMORY ((HRESULT)0x8007000E)
#endif
#ifndef E_NOTIMPL
#define E_NOTIMPL ((HRESULT)0x80004001)
#endif
#ifndef E_NOINTERFACE
#define E_NOINTERFACE ((HRESULT)0x80004002)
#endif
#ifndef E_POINTER
#define E_POINTER ((HRESULT)0x80004003)
#endif
#ifndef E_ABORT
#define E_ABORT ((HRESULT)0x80004004)
#endif
#ifndef E_UNEXPECTED
#define E_UNEXPECTED ((HRESULT)0x8000FFFF)
#endif
#ifndef DXGI_ERROR_DEVICE_REMOVED
#define DXGI_ERROR_DEVICE_REMOVED ((HRESULT)0x887A0005)
#endif
#ifndef DXGI_ERROR_DEVICE_RESET
#define DXGI_ERROR_DEVICE_RESET ((HRESULT)0x887A0007)
#endif

#ifndef SUCCEEDED
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#endif
#ifndef FAILED
#define FAILED(hr) (((HRESULT)(hr)) < 0)
#endif

// For HRESULT failure logging, use the existing SPARK_HR_CHECK macro from
// Utils/SparkError.h, which handles Windows FormatMessage translation and
// stack-trace capture uniformly. That macro depends on FAILED() working
// correctly, which is now guaranteed by the int32_t HRESULT above.

// --- Boolean constants ---
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

// --- Window/GDI handles (opaque pointers on non-Windows) ---
using HWND = void*;
using HDC = void*;
using HINSTANCE = void*;
using HICON = void*;
using HCURSOR = void*;
using HBRUSH = void*;
using HMENU = void*;
using HACCEL = void*;
using ATOM = unsigned short;

// --- Message types ---
using WPARAM = uintptr_t;
using LPARAM = intptr_t;
using LRESULT = intptr_t;

// --- Win32 virtual key codes ---
#ifndef VK_BACK
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12 // Alt key
#define VK_PAUSE 0x13
#define VK_CAPITAL 0x14
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_PRIOR 0x21 // Page Up
#define VK_NEXT 0x22  // Page Down
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_INSERT 0x2D
#define VK_DELETE 0x2E
#define VK_LSHIFT 0xA0
#define VK_RSHIFT 0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU 0xA4
#define VK_RMENU 0xA5
#define VK_OEM_4 0xDB // [{
#define VK_OEM_6 0xDD // ]}
#define VK_F1 0x70
#define VK_F2 0x71
#define VK_F3 0x72
#define VK_F4 0x73
#define VK_F5 0x74
#define VK_F6 0x75
#define VK_F7 0x76
#define VK_F8 0x77
#define VK_F9 0x78
#define VK_F10 0x79
#define VK_F11 0x7A
#define VK_F12 0x7B
#endif

// --- Win32 window message constants ---
#ifndef WM_KEYDOWN
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP 0x0205
#define WM_MBUTTONDOWN 0x0207
#define WM_MBUTTONUP 0x0208
#define WM_SIZE 0x0005
#define WM_DESTROY 0x0002
#define WM_QUIT 0x0012
#define WM_CLOSE 0x0010
#endif

// --- Win32 cursor/mouse helpers ---
#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) ((int)(short)((lp) & 0xFFFF))
#define GET_Y_LPARAM(lp) ((int)(short)(((lp) >> 16) & 0xFFFF))
#endif

// --- Calling convention macros (no-ops on non-Windows) ---
#ifndef APIENTRY
#define APIENTRY
#endif
#ifndef CALLBACK
#define CALLBACK
#endif
#ifndef WINAPI
#define WINAPI
#endif

// --- SAL annotations (no-ops) ---
#ifndef _In_
#define _In_
#endif
#ifndef _In_opt_
#define _In_opt_
#endif
#ifndef _Out_
#define _Out_
#endif
#ifndef _Inout_
#define _Inout_
#endif

// --- String macros ---
#ifndef MAX_PATH
#define MAX_PATH 260
#endif
#ifndef _countof
#define _countof(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

// --- Secure string functions (stubs) ---
#ifndef wcscpy_s
inline int wcscpy_s(wchar_t* dest, size_t destsz, const wchar_t* src)
{
    if (!dest || !src || destsz == 0)
        return -1;
    wcsncpy(dest, src, destsz - 1);
    dest[destsz - 1] = L'\0';
    return 0;
}
#endif

#ifndef swprintf_s
#define swprintf_s swprintf
#endif

// --- COM / DirectX stub macros ---
#ifndef MAKEINTRESOURCE
#define MAKEINTRESOURCE(i) ((const char*)(uintptr_t)(i))
#endif
#ifndef MAKEINTRESOURCEW
#define MAKEINTRESOURCEW(i) ((const wchar_t*)(uintptr_t)(i))
#endif

// --- Thread ID ---
#ifdef __linux__
#include <unistd.h>
#include <sys/syscall.h>
#endif

inline unsigned long SparkGetCurrentThreadId()
{
#ifdef __linux__
    return static_cast<unsigned long>(syscall(SYS_gettid));
#else
    return 0;
#endif
}

// --- OutputDebugString stubs ---
inline void OutputDebugStringA(const char*) {}
inline void OutputDebugStringW(const wchar_t*) {}

// --- MessageBox — SDL2 fallback on Linux, no-op otherwise ---
#ifndef MB_ICONERROR
#define MB_ICONERROR 0x10
#endif
#ifndef MB_OK
#define MB_OK 0
#endif
#ifndef MB_YESNO
#define MB_YESNO 0x04
#endif
#ifndef IDYES
#define IDYES 6
#endif
#ifndef IDNO
#define IDNO 7
#endif
#ifndef IDOK
#define IDOK 1
#endif

// Internal helper for SDL2 MessageBox with Yes/No buttons
#ifdef SPARK_SDL2_AVAILABLE
inline int SparkSDL2MessageBox(Uint32 flags, const char* caption, const char* text, unsigned int type)
{
    if (type & MB_YESNO)
    {
        // Use SDL_ShowMessageBox for Yes/No button support
        SDL_MessageBoxButtonData buttons[2] = {
            {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, IDNO, "No"},
            {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, IDYES, "Yes"},
        };
        SDL_MessageBoxData data{};
        data.flags = flags;
        data.window = nullptr;
        data.title = caption;
        data.message = text;
        data.numbuttons = 2;
        data.buttons = buttons;
        int buttonId = IDNO;
        if (SDL_ShowMessageBox(&data, &buttonId) < 0)
            return IDNO; // SDL error — treat as "No"
        return buttonId;
    }
    SDL_ShowSimpleMessageBox(flags, caption, text, nullptr);
    return IDOK;
}
#endif

inline int MessageBoxW(void*, const wchar_t* text, const wchar_t* caption, unsigned int type)
{
#ifdef SPARK_SDL2_AVAILABLE
    // Convert wchar_t to char for SDL (simple truncation — fine for ASCII error messages)
    char textBuf[512];
    char captionBuf[128];
    size_t i = 0;
    if (text)
        for (; text[i] && i < sizeof(textBuf) - 1; ++i)
            textBuf[i] = static_cast<char>(text[i]);
    textBuf[i] = '\0';
    i = 0;
    if (caption)
        for (; caption[i] && i < sizeof(captionBuf) - 1; ++i)
            captionBuf[i] = static_cast<char>(caption[i]);
    captionBuf[i] = '\0';

    Uint32 flags = SDL_MESSAGEBOX_INFORMATION;
    if (type & MB_ICONERROR)
        flags = SDL_MESSAGEBOX_ERROR;

    return SparkSDL2MessageBox(flags, captionBuf, textBuf, type);
#else
    (void)text;
    (void)caption;
    (void)type;
#endif
    return 0;
}
inline int MessageBoxA(void*, const char* text, const char* caption, unsigned int type)
{
#ifdef SPARK_SDL2_AVAILABLE
    Uint32 flags = SDL_MESSAGEBOX_INFORMATION;
    if (type & MB_ICONERROR)
        flags = SDL_MESSAGEBOX_ERROR;
    return SparkSDL2MessageBox(flags, caption ? caption : "", text ? text : "", type);
#else
    (void)text;
    (void)caption;
    (void)type;
#endif
    return 0;
}

// --- Win32 misc macros ---
#ifndef ZeroMemory
#define ZeroMemory(dest, size) memset((dest), 0, (size))
#endif
#ifndef CopyMemory
#define CopyMemory(dest, src, size) memcpy((dest), (src), (size))
#endif
#ifndef MAX_LOADSTRING
#define MAX_LOADSTRING 100
#endif
#ifndef HRESULT_FROM_WIN32
#define HRESULT_FROM_WIN32(x) ((HRESULT)(x) <= 0 ? (HRESULT)(x) : (HRESULT)(((x) & 0x0000FFFF) | 0x80070000))
#endif
#ifndef ERROR_SUCCESS
#define ERROR_SUCCESS 0L
#endif
#ifndef ERROR_FILE_NOT_FOUND
#define ERROR_FILE_NOT_FOUND 2L
#endif
#ifndef WAVE_FORMAT_PCM
#define WAVE_FORMAT_PCM 1
#endif
#ifndef WAVE_FORMAT_IEEE_FLOAT
#define WAVE_FORMAT_IEEE_FLOAT 3
#endif

// --- WAVEFORMATEX stub ---
struct WAVEFORMATEX
{
    uint16_t wFormatTag;
    uint16_t nChannels;
    uint32_t nSamplesPerSec;
    uint32_t nAvgBytesPerSec;
    uint16_t nBlockAlign;
    uint16_t wBitsPerSample;
    uint16_t cbSize;
};

#endif // !SPARK_PLATFORM_WINDOWS
