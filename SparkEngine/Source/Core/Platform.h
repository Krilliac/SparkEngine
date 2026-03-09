/**
 * @file Platform.h
 * @brief Cross-platform type definitions and compatibility layer
 * @author Spark Engine Team
 * @date 2026
 *
 * This header provides platform-agnostic type definitions so that
 * engine code can compile on Windows (MSVC), Linux (GCC/Clang), and macOS.
 * On Windows, these map directly to the native Win32/COM types.
 * On other platforms, lightweight stubs are provided.
 */

#pragma once

// ============================================================================
// Compiler Detection
// Defines SPARK_COMPILER_* macros before any platform-specific includes.
// ============================================================================

// --- MSVC (Visual Studio) ---
#if defined(_MSC_VER)
#define SPARK_COMPILER_MSVC 1
#define SPARK_COMPILER_VERSION _MSC_VER
// Visual Studio release year mapping (_MSC_VER ranges):
//   VS 2017 (v141) : 1910-1916
//   VS 2019 (v142) : 1920-1929
//   VS 2022 (v143) : 1930-1939
//   VS 2026 (v144) : 1940+
#if _MSC_VER >= 1950
#define SPARK_COMPILER_MSVC_2026_PLUS 1
#elif _MSC_VER >= 1940
#define SPARK_COMPILER_MSVC_2026 1
#elif _MSC_VER >= 1930
#define SPARK_COMPILER_MSVC_2022 1
#elif _MSC_VER >= 1920
#define SPARK_COMPILER_MSVC_2019 1
#elif _MSC_VER >= 1910
#define SPARK_COMPILER_MSVC_2017 1
#endif

// --- Apple Clang (must be checked before generic Clang) ---
#elif defined(__clang__) && defined(__apple_build_version__)
#define SPARK_COMPILER_APPLE_CLANG 1
#define SPARK_COMPILER_CLANG 1
#define SPARK_COMPILER_VERSION (__clang_major__ * 10000 + __clang_minor__ * 100 + __clang_patchlevel__)

// --- Intel oneAPI DPC++/C++ (ICPX) - Clang-compatible ---
#elif defined(__INTEL_LLVM_COMPILER)
#define SPARK_COMPILER_INTEL_LLVM 1
#define SPARK_COMPILER_CLANG 1 // ICPX is Clang-compatible
#define SPARK_COMPILER_VERSION __INTEL_LLVM_COMPILER

// --- Intel Classic C++ Compiler (ICC) ---
#elif defined(__INTEL_COMPILER)
#define SPARK_COMPILER_INTEL_CLASSIC 1
#define SPARK_COMPILER_VERSION __INTEL_COMPILER

// --- Generic LLVM Clang ---
#elif defined(__clang__)
#define SPARK_COMPILER_CLANG 1
#define SPARK_COMPILER_VERSION (__clang_major__ * 10000 + __clang_minor__ * 100 + __clang_patchlevel__)

// --- GCC ---
#elif defined(__GNUC__)
#define SPARK_COMPILER_GCC 1
#define SPARK_COMPILER_VERSION (__GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__)

#else
#define SPARK_COMPILER_UNKNOWN 1
#define SPARK_COMPILER_VERSION 0
#endif

// Convenience: true if any Clang-family compiler (Clang, Apple Clang, Intel LLVM)
#if defined(SPARK_COMPILER_CLANG) || defined(SPARK_COMPILER_APPLE_CLANG) || defined(SPARK_COMPILER_INTEL_LLVM)
#define SPARK_COMPILER_CLANG_FAMILY 1
#endif

// ============================================================================
// Platform Detection
// ============================================================================

#if defined(_WIN32) || defined(_WIN64)
#define SPARK_PLATFORM_WINDOWS 1
#elif defined(__linux__)
#define SPARK_PLATFORM_LINUX 1
#elif defined(__APPLE__)
#define SPARK_PLATFORM_MACOS 1
#endif

// ============================================================================
// Platform-Agnostic Type Aliases
// ============================================================================

namespace Spark
{

    /// Platform-agnostic window handle. Use in public APIs instead of HWND.
    using NativeWindowHandle = void*; // compatible with HWND on Windows

} // namespace Spark

// ============================================================================
// Windows: Use native types
// ============================================================================

#ifdef SPARK_PLATFORM_WINDOWS

#include <DirectXMath.h>
using namespace DirectX;

#else // !SPARK_PLATFORM_WINDOWS

// ============================================================================
// Non-Windows: Provide compatible type stubs
// ============================================================================

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <cmath>

// --- Basic Win32 types ---
using BOOL = int;
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
using HRESULT = long;

#ifndef S_OK
#define S_OK ((HRESULT)0L)
#endif
#ifndef S_FALSE
#define S_FALSE ((HRESULT)1L)
#endif
#ifndef E_FAIL
#define E_FAIL ((HRESULT)0x80004005L)
#endif
#ifndef E_INVALIDARG
#define E_INVALIDARG ((HRESULT)0x80070057L)
#endif
#ifndef E_OUTOFMEMORY
#define E_OUTOFMEMORY ((HRESULT)0x8007000EL)
#endif
#ifndef E_NOTIMPL
#define E_NOTIMPL ((HRESULT)0x80004001L)
#endif

#ifndef SUCCEEDED
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#endif
#ifndef FAILED
#define FAILED(hr) (((HRESULT)(hr)) < 0)
#endif

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

// --- MessageBox stub ---
#ifndef MB_ICONERROR
#define MB_ICONERROR 0x10
#endif
#ifndef MB_OK
#define MB_OK 0
#endif
inline int MessageBoxW(void*, const wchar_t*, const wchar_t*, unsigned int)
{
    return 0;
}
inline int MessageBoxA(void*, const char*, const char*, unsigned int)
{
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

// --- XAudio2 stubs ---
#ifndef XAUDIO2_DEFAULT_PROCESSOR
#define XAUDIO2_DEFAULT_PROCESSOR 1
#endif
#ifndef XAUDIO2_END_OF_STREAM
#define XAUDIO2_END_OF_STREAM 0x0040
#endif
#ifndef XAUDIO2_LOOP_INFINITE
#define XAUDIO2_LOOP_INFINITE 255
#endif

struct XAUDIO2_BUFFER
{
    uint32_t Flags;
    uint32_t AudioBytes;
    const uint8_t* pAudioData;
    uint32_t PlayBegin;
    uint32_t PlayLength;
    uint32_t LoopBegin;
    uint32_t LoopLength;
    uint32_t LoopCount;
    void* pContext;
};
struct XAUDIO2_VOICE_STATE
{
    void* pCurrentBufferContext;
    uint32_t BuffersQueued;
    uint64_t SamplesPlayed;
};
struct XAUDIO2_VOICE_DETAILS
{
    uint32_t CreationFlags;
    uint32_t ActiveFlags;
    uint32_t InputChannels;
    uint32_t InputSampleRate;
};

// Stub XAudio2 interfaces with no-op methods so code compiles on Linux
struct IXAudio2SourceVoice
{
    virtual ~IXAudio2SourceVoice() = default;
    long SetVolume(float) { return 0; }
    long SetFrequencyRatio(float) { return 0; }
    long SubmitSourceBuffer(const XAUDIO2_BUFFER*) { return 0; }
    long Start(uint32_t = 0, uint32_t = 0) { return 0; }
    long Stop(uint32_t = 0, uint32_t = 0) { return 0; }
    long FlushSourceBuffers() { return 0; }
    void GetState(XAUDIO2_VOICE_STATE* s)
    {
        if (s)
        {
            s->BuffersQueued = 0;
            s->SamplesPlayed = 0;
            s->pCurrentBufferContext = nullptr;
        }
    }
    void GetVoiceDetails(XAUDIO2_VOICE_DETAILS* d)
    {
        if (d)
        {
            d->InputChannels = 1;
            d->InputSampleRate = 44100;
            d->CreationFlags = 0;
            d->ActiveFlags = 0;
        }
    }
    long SetOutputMatrix(void*, uint32_t, uint32_t, const float*) { return 0; }
    void DestroyVoice() {}
    void Release() {}
};

struct IXAudio2MasteringVoice
{
    virtual ~IXAudio2MasteringVoice() = default;
    long SetVolume(float) { return 0; }
    void GetVoiceDetails(XAUDIO2_VOICE_DETAILS* d)
    {
        if (d)
        {
            d->InputChannels = 2;
            d->InputSampleRate = 44100;
            d->CreationFlags = 0;
            d->ActiveFlags = 0;
        }
    }
    void DestroyVoice() {}
    void Release() {}
};

struct IXAudio2SubmixVoice
{
    virtual ~IXAudio2SubmixVoice() = default;
    void DestroyVoice() {}
};

struct IXAudio2
{
    virtual ~IXAudio2() = default;
    long CreateMasteringVoice(IXAudio2MasteringVoice** pp, uint32_t = 0, uint32_t = 0, uint32_t = 0, void* = nullptr,
                              void* = nullptr)
    {
        static IXAudio2MasteringVoice stubMaster;
        if (pp)
            *pp = &stubMaster;
        return 0;
    }
    long CreateSourceVoice(IXAudio2SourceVoice** pp, const WAVEFORMATEX* = nullptr, uint32_t = 0, float = 2.0f,
                           void* = nullptr, void* = nullptr, void* = nullptr)
    {
        static IXAudio2SourceVoice stubSource;
        if (pp)
            *pp = &stubSource;
        return 0;
    }
    void Release() {}
};

inline long XAudio2Create(IXAudio2** pp, uint32_t = 0, uint32_t = XAUDIO2_DEFAULT_PROCESSOR)
{
    static IXAudio2 stubEngine;
    if (pp)
        *pp = &stubEngine;
    return 0; // S_OK
}

// --- XInput stubs ---
struct XINPUT_GAMEPAD
{
    uint16_t wButtons;
    uint8_t bLeftTrigger;
    uint8_t bRightTrigger;
    short sThumbLX;
    short sThumbLY;
    short sThumbRX;
    short sThumbRY;
};
struct XINPUT_STATE
{
    uint32_t dwPacketNumber;
    XINPUT_GAMEPAD Gamepad;
};
struct XINPUT_VIBRATION
{
    uint16_t wLeftMotorSpeed;
    uint16_t wRightMotorSpeed;
};

#ifndef XINPUT_GAMEPAD_A
#define XINPUT_GAMEPAD_DPAD_UP 0x0001
#define XINPUT_GAMEPAD_DPAD_DOWN 0x0002
#define XINPUT_GAMEPAD_DPAD_LEFT 0x0004
#define XINPUT_GAMEPAD_DPAD_RIGHT 0x0008
#define XINPUT_GAMEPAD_START 0x0010
#define XINPUT_GAMEPAD_BACK 0x0020
#define XINPUT_GAMEPAD_LEFT_THUMB 0x0040
#define XINPUT_GAMEPAD_RIGHT_THUMB 0x0080
#define XINPUT_GAMEPAD_LEFT_SHOULDER 0x0100
#define XINPUT_GAMEPAD_RIGHT_SHOULDER 0x0200
#define XINPUT_GAMEPAD_A 0x1000
#define XINPUT_GAMEPAD_B 0x2000
#define XINPUT_GAMEPAD_X 0x4000
#define XINPUT_GAMEPAD_Y 0x8000
#endif

inline uint32_t XInputGetState(uint32_t, XINPUT_STATE*)
{
    return 1; /* ERROR_DEVICE_NOT_CONNECTED */
}
inline uint32_t XInputSetState(uint32_t, XINPUT_VIBRATION*)
{
    return 1;
}

// --- Win32 process/module stubs ---
#ifndef PROCESS_QUERY_INFORMATION
#define PROCESS_QUERY_INFORMATION 0x0400
#endif
#ifndef GetCurrentThreadId
#define GetCurrentThreadId SparkGetCurrentThreadId
#endif

// --- Console color constants ---
#ifndef FOREGROUND_RED
#define FOREGROUND_RED 0x0004
#define FOREGROUND_GREEN 0x0002
#define FOREGROUND_BLUE 0x0001
#define FOREGROUND_INTENSITY 0x0008
#endif

// ============================================================================
// DirectXMath Compatibility (minimal stubs for non-Windows)
// ============================================================================

namespace DirectX
{

    struct XMFLOAT2
    {
        float x, y;
        constexpr XMFLOAT2() : x(0), y(0) {}
        constexpr XMFLOAT2(float _x, float _y) : x(_x), y(_y) {}
    };

    struct XMFLOAT3
    {
        float x, y, z;
        constexpr XMFLOAT3() : x(0), y(0), z(0) {}
        constexpr XMFLOAT3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}
    };

    struct XMFLOAT4
    {
        float x, y, z, w;
        constexpr XMFLOAT4() : x(0), y(0), z(0), w(0) {}
        constexpr XMFLOAT4(float _x, float _y, float _z, float _w) : x(_x), y(_y), z(_z), w(_w) {}
    };

    struct XMFLOAT4X4
    {
        float m[4][4];
        XMFLOAT4X4() { memset(m, 0, sizeof(m)); }
    };

    // XMVECTOR as a 4-component float vector (must come before XMMATRIX for union)
    struct XMVECTOR
    {
        float x, y, z, w;
    };

    // XMMATRIX as a simple 4x4 float matrix with row access
    struct XMMATRIX
    {
        union
        {
            float m[4][4];
            XMVECTOR r[4];
        };
        XMMATRIX() { memset(m, 0, sizeof(m)); }
        XMMATRIX(float m00, float m01, float m02, float m03, float m10, float m11, float m12, float m13, float m20,
                 float m21, float m22, float m23, float m30, float m31, float m32, float m33)
        {
            m[0][0] = m00;
            m[0][1] = m01;
            m[0][2] = m02;
            m[0][3] = m03;
            m[1][0] = m10;
            m[1][1] = m11;
            m[1][2] = m12;
            m[1][3] = m13;
            m[2][0] = m20;
            m[2][1] = m21;
            m[2][2] = m22;
            m[2][3] = m23;
            m[3][0] = m30;
            m[3][1] = m31;
            m[3][2] = m32;
            m[3][3] = m33;
        }
    };

    inline XMMATRIX XMMatrixIdentity()
    {
        XMMATRIX mat;
        mat.m[0][0] = mat.m[1][1] = mat.m[2][2] = mat.m[3][3] = 1.0f;
        return mat;
    }

    inline XMMATRIX XMMatrixTranslation(float x, float y, float z)
    {
        XMMATRIX mat = XMMatrixIdentity();
        mat.m[3][0] = x;
        mat.m[3][1] = y;
        mat.m[3][2] = z;
        return mat;
    }

    inline XMMATRIX XMMatrixScaling(float x, float y, float z)
    {
        XMMATRIX mat;
        memset(&mat, 0, sizeof(mat));
        mat.m[0][0] = x;
        mat.m[1][1] = y;
        mat.m[2][2] = z;
        mat.m[3][3] = 1.0f;
        return mat;
    }

    inline XMMATRIX XMMatrixRotationY(float angle)
    {
        XMMATRIX mat = XMMatrixIdentity();
        float c = cosf(angle), s = sinf(angle);
        mat.m[0][0] = c;
        mat.m[0][2] = -s;
        mat.m[2][0] = s;
        mat.m[2][2] = c;
        return mat;
    }

    inline XMMATRIX XMMatrixMultiply(const XMMATRIX& a, const XMMATRIX& b)
    {
        XMMATRIX result;
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
            {
                result.m[i][j] = 0;
                for (int k = 0; k < 4; k++)
                    result.m[i][j] += a.m[i][k] * b.m[k][j];
            }
        return result;
    }

    inline XMMATRIX operator*(const XMMATRIX& a, const XMMATRIX& b)
    {
        return XMMatrixMultiply(a, b);
    }

    inline XMMATRIX XMMatrixInverse(XMVECTOR* det, XMMATRIX mat)
    {
        const float* m = &mat.m[0][0];
        float inv[16];
        inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
                 m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
        inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
                 m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
        inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
                 m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
        inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
                  m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
        inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
                 m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
        inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
                 m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
        inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
                 m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
        inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
                  m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
        inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
                 m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
        inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
                 m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
        inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
                  m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
        inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
                  m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
        inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
                 m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
        inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
                 m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
        inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
                  m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
        inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
                  m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
        float d = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
        if (det)
        {
            det->x = d;
            det->y = d;
            det->z = d;
            det->w = d;
        }
        if (fabsf(d) < 1e-12f)
            return XMMatrixIdentity();
        float invDet = 1.0f / d;
        XMMATRIX result;
        for (int i = 0; i < 16; i++)
            (&result.m[0][0])[i] = inv[i] * invDet;
        return result;
    }

    // Non-const overload (called with rvalue)
    inline XMMATRIX XMMatrixTranspose(XMMATRIX m)
    {
        XMMATRIX result;
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                result.m[i][j] = m.m[j][i];
        return result;
    }

    inline XMMATRIX XMMatrixPerspectiveFovLH(float fov, float aspect, float nearZ, float farZ)
    {
        float yScale = 1.0f / tanf(fov * 0.5f);
        float xScale = yScale / aspect;
        float range = farZ / (farZ - nearZ);
        XMMATRIX mat;
        memset(&mat, 0, sizeof(mat));
        mat.m[0][0] = xScale;
        mat.m[1][1] = yScale;
        mat.m[2][2] = range;
        mat.m[2][3] = 1.0f;
        mat.m[3][2] = -range * nearZ;
        return mat;
    }

    inline XMMATRIX XMMatrixOrthographicLH(float width, float height, float nearZ, float farZ)
    {
        float range = 1.0f / (farZ - nearZ);
        XMMATRIX mat;
        memset(&mat, 0, sizeof(mat));
        mat.m[0][0] = 2.0f / width;
        mat.m[1][1] = 2.0f / height;
        mat.m[2][2] = range;
        mat.m[3][2] = -range * nearZ;
        mat.m[3][3] = 1.0f;
        return mat;
    }

    inline XMMATRIX XMMatrixOrthographicOffCenterLH(float left, float right, float bottom, float top, float nearZ,
                                                    float farZ)
    {
        float range = 1.0f / (farZ - nearZ);
        XMMATRIX mat;
        memset(&mat, 0, sizeof(mat));
        mat.m[0][0] = 2.0f / (right - left);
        mat.m[1][1] = 2.0f / (top - bottom);
        mat.m[2][2] = range;
        mat.m[3][0] = -(right + left) / (right - left);
        mat.m[3][1] = -(top + bottom) / (top - bottom);
        mat.m[3][2] = -range * nearZ;
        mat.m[3][3] = 1.0f;
        return mat;
    }

    inline XMVECTOR XMVectorSet(float x, float y, float z, float w)
    {
        return {x, y, z, w};
    }

    inline void XMStoreFloat3(XMFLOAT3* dest, XMVECTOR v)
    {
        dest->x = v.x;
        dest->y = v.y;
        dest->z = v.z;
    }

    inline void XMStoreFloat4(XMFLOAT4* dest, XMVECTOR v)
    {
        dest->x = v.x;
        dest->y = v.y;
        dest->z = v.z;
        dest->w = v.w;
    }

    inline XMVECTOR XMLoadFloat3(const XMFLOAT3* src)
    {
        return {src->x, src->y, src->z, 0.0f};
    }

    inline XMVECTOR XMLoadFloat4(const XMFLOAT4* src)
    {
        return {src->x, src->y, src->z, src->w};
    }

    inline float XMConvertToRadians(float degrees)
    {
        return degrees * 3.14159265358979323846f / 180.0f;
    }

    inline float XMConvertToDegrees(float radians)
    {
        return radians * 180.0f / 3.14159265358979323846f;
    }

    // Constants
    constexpr float XM_PI = 3.14159265358979323846f;
    constexpr float XM_2PI = 6.28318530717958647692f;
    constexpr float XM_PIDIV2 = 1.57079632679489661923f;
    constexpr float XM_PIDIV4 = 0.78539816339744830961f;
    constexpr float XM_1DIVPI = 0.31830988618379067154f;
    constexpr float XM_1DIV2PI = 0.15915494309189533577f;

    // Vector arithmetic
    inline XMVECTOR XMVectorAdd(XMVECTOR a, XMVECTOR b)
    {
        return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
    }
    inline XMVECTOR XMVectorSubtract(XMVECTOR a, XMVECTOR b)
    {
        return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
    }
    inline XMVECTOR XMVectorScale(XMVECTOR v, float s)
    {
        return {v.x * s, v.y * s, v.z * s, v.w * s};
    }
    inline XMVECTOR XMVectorMultiply(XMVECTOR a, XMVECTOR b)
    {
        return {a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w};
    }
    inline XMVECTOR XMVectorNegate(XMVECTOR v)
    {
        return {-v.x, -v.y, -v.z, -v.w};
    }
    inline XMVECTOR XMVectorZero()
    {
        return {0, 0, 0, 0};
    }
    inline XMVECTOR XMVectorReplicate(float v)
    {
        return {v, v, v, v};
    }
    inline XMVECTOR XMVectorMin(XMVECTOR a, XMVECTOR b)
    {
        return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z, a.w < b.w ? a.w : b.w};
    }
    inline XMVECTOR XMVectorMax(XMVECTOR a, XMVECTOR b)
    {
        return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z, a.w > b.w ? a.w : b.w};
    }

    // Vector operators
    inline XMVECTOR operator+(XMVECTOR a, XMVECTOR b)
    {
        return XMVectorAdd(a, b);
    }
    inline XMVECTOR operator-(XMVECTOR a, XMVECTOR b)
    {
        return XMVectorSubtract(a, b);
    }
    inline XMVECTOR operator*(XMVECTOR v, float s)
    {
        return XMVectorScale(v, s);
    }
    inline XMVECTOR operator*(float s, XMVECTOR v)
    {
        return XMVectorScale(v, s);
    }

    // Vector3 math (return XMVECTOR for DirectXMath API compatibility)
    inline XMVECTOR XMVector3Dot(XMVECTOR a, XMVECTOR b)
    {
        float d = a.x * b.x + a.y * b.y + a.z * b.z;
        return {d, d, d, d};
    }
    inline XMVECTOR XMVector3Length(XMVECTOR v)
    {
        float len = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
        return {len, len, len, len};
    }
    inline XMVECTOR XMVector3LengthSq(XMVECTOR v)
    {
        float sq = v.x * v.x + v.y * v.y + v.z * v.z;
        return {sq, sq, sq, sq};
    }
    inline XMVECTOR XMVector3Normalize(XMVECTOR v)
    {
        float len = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
        if (len < 1e-8f)
            return {0, 0, 0, 0};
        float inv = 1.0f / len;
        return {v.x * inv, v.y * inv, v.z * inv, 0.0f};
    }
    inline XMVECTOR XMVector3Cross(XMVECTOR a, XMVECTOR b)
    {
        return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x, 0.0f};
    }
    inline XMVECTOR XMVector3TransformCoord(XMVECTOR v, const XMMATRIX& m)
    {
        XMVECTOR result;
        result.x = v.x * m.m[0][0] + v.y * m.m[1][0] + v.z * m.m[2][0] + m.m[3][0];
        result.y = v.x * m.m[0][1] + v.y * m.m[1][1] + v.z * m.m[2][1] + m.m[3][1];
        result.z = v.x * m.m[0][2] + v.y * m.m[1][2] + v.z * m.m[2][2] + m.m[3][2];
        float w = v.x * m.m[0][3] + v.y * m.m[1][3] + v.z * m.m[2][3] + m.m[3][3];
        if (fabsf(w) > 1e-8f)
        {
            result.x /= w;
            result.y /= w;
            result.z /= w;
        }
        result.w = 0.0f;
        return result;
    }

    // Rotation matrices
    inline XMMATRIX XMMatrixRotationX(float angle)
    {
        XMMATRIX mat = XMMatrixIdentity();
        float c = cosf(angle), s = sinf(angle);
        mat.m[1][1] = c;
        mat.m[1][2] = s;
        mat.m[2][1] = -s;
        mat.m[2][2] = c;
        return mat;
    }
    inline XMMATRIX XMMatrixRotationZ(float angle)
    {
        XMMATRIX mat = XMMatrixIdentity();
        float c = cosf(angle), s = sinf(angle);
        mat.m[0][0] = c;
        mat.m[0][1] = s;
        mat.m[1][0] = -s;
        mat.m[1][1] = c;
        return mat;
    }
    inline XMMATRIX XMMatrixRotationRollPitchYaw(float pitch, float yaw, float roll)
    {
        return XMMatrixRotationX(pitch) * XMMatrixRotationY(yaw) * XMMatrixRotationZ(roll);
    }

    // Store/Load additional
    inline void XMStoreFloat4x4(XMFLOAT4X4* dest, const XMMATRIX& m)
    {
        memcpy(dest->m, m.m, sizeof(float) * 16);
    }
    inline XMMATRIX XMLoadFloat4x4(const XMFLOAT4X4* src)
    {
        XMMATRIX m;
        memcpy(m.m, src->m, sizeof(float) * 16);
        return m;
    }

    inline XMVECTOR XMVectorLerp(XMVECTOR a, XMVECTOR b, float t)
    {
        return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
    }

    // Component accessors
    inline float XMVectorGetX(XMVECTOR v)
    {
        return v.x;
    }
    inline float XMVectorGetY(XMVECTOR v)
    {
        return v.y;
    }
    inline float XMVectorGetZ(XMVECTOR v)
    {
        return v.z;
    }
    inline float XMVectorGetW(XMVECTOR v)
    {
        return v.w;
    }

    inline XMMATRIX XMMatrixLookAtLH(XMVECTOR eye, XMVECTOR target, XMVECTOR up)
    {
        XMVECTOR zAxis = XMVector3Normalize(XMVectorSubtract(target, eye));
        XMVECTOR xAxis = XMVector3Normalize(XMVector3Cross(up, zAxis));
        XMVECTOR yAxis = XMVector3Cross(zAxis, xAxis);
        float dx = -(XMVectorGetX(XMVector3Dot(xAxis, eye)));
        float dy = -(XMVectorGetX(XMVector3Dot(yAxis, eye)));
        float dz = -(XMVectorGetX(XMVector3Dot(zAxis, eye)));
        XMMATRIX mat;
        mat.m[0][0] = xAxis.x;
        mat.m[0][1] = yAxis.x;
        mat.m[0][2] = zAxis.x;
        mat.m[0][3] = 0.0f;
        mat.m[1][0] = xAxis.y;
        mat.m[1][1] = yAxis.y;
        mat.m[1][2] = zAxis.y;
        mat.m[1][3] = 0.0f;
        mat.m[2][0] = xAxis.z;
        mat.m[2][1] = yAxis.z;
        mat.m[2][2] = zAxis.z;
        mat.m[2][3] = 0.0f;
        mat.m[3][0] = dx;
        mat.m[3][1] = dy;
        mat.m[3][2] = dz;
        mat.m[3][3] = 1.0f;
        return mat;
    }

    // Transform functions
    inline XMVECTOR XMVector3Transform(XMVECTOR v, const XMMATRIX& m)
    {
        return XMVector3TransformCoord(v, m);
    }

    // Quaternion stubs
    inline XMVECTOR XMQuaternionSlerp(XMVECTOR a, XMVECTOR b, float t)
    {
        return XMVectorLerp(a, b, t); // Simplified linear interpolation
    }

    inline XMMATRIX XMMatrixRotationQuaternion(XMVECTOR q)
    {
        // Simplified quaternion to matrix
        float x = q.x, y = q.y, z = q.z, w = q.w;
        XMMATRIX m = XMMatrixIdentity();
        m.m[0][0] = 1 - 2 * (y * y + z * z);
        m.m[0][1] = 2 * (x * y + w * z);
        m.m[0][2] = 2 * (x * z - w * y);
        m.m[1][0] = 2 * (x * y - w * z);
        m.m[1][1] = 1 - 2 * (x * x + z * z);
        m.m[1][2] = 2 * (y * z + w * x);
        m.m[2][0] = 2 * (x * z + w * y);
        m.m[2][1] = 2 * (y * z - w * x);
        m.m[2][2] = 1 - 2 * (x * x + y * y);
        return m;
    }

    inline XMMATRIX XMMatrixRotationAxis(XMVECTOR axis, float angle)
    {
        float c = cosf(angle), s = sinf(angle), t = 1.0f - c;
        float x = axis.x, y = axis.y, z = axis.z;
        XMMATRIX m = XMMatrixIdentity();
        m.m[0][0] = t * x * x + c;
        m.m[0][1] = t * x * y + s * z;
        m.m[0][2] = t * x * z - s * y;
        m.m[1][0] = t * x * y - s * z;
        m.m[1][1] = t * y * y + c;
        m.m[1][2] = t * y * z + s * x;
        m.m[2][0] = t * x * z + s * y;
        m.m[2][1] = t * y * z - s * x;
        m.m[2][2] = t * z * z + c;
        return m;
    }

    inline XMMATRIX XMMatrixTranslationFromVector(XMVECTOR v)
    {
        return XMMatrixTranslation(v.x, v.y, v.z);
    }

    inline XMMATRIX XMMatrixScalingFromVector(XMVECTOR v)
    {
        return XMMatrixScaling(v.x, v.y, v.z);
    }

    inline bool XMMatrixDecompose(XMVECTOR* outScale, XMVECTOR* outRotQuat, XMVECTOR* outTrans, const XMMATRIX& m)
    {
        // Extract translation
        *outTrans = {m.m[3][0], m.m[3][1], m.m[3][2], 1.0f};
        // Extract scale (column lengths)
        float sx = sqrtf(m.m[0][0] * m.m[0][0] + m.m[0][1] * m.m[0][1] + m.m[0][2] * m.m[0][2]);
        float sy = sqrtf(m.m[1][0] * m.m[1][0] + m.m[1][1] * m.m[1][1] + m.m[1][2] * m.m[1][2]);
        float sz = sqrtf(m.m[2][0] * m.m[2][0] + m.m[2][1] * m.m[2][1] + m.m[2][2] * m.m[2][2]);
        *outScale = {sx, sy, sz, 0.0f};
        *outRotQuat = {0, 0, 0, 1}; // Identity quaternion as stub
        return true;
    }

    // XMMATRIX 16-float constructor
    inline XMMATRIX XMMatrixSet(float m00, float m01, float m02, float m03, float m10, float m11, float m12, float m13,
                                float m20, float m21, float m22, float m23, float m30, float m31, float m32, float m33)
    {
        XMMATRIX mat;
        mat.m[0][0] = m00;
        mat.m[0][1] = m01;
        mat.m[0][2] = m02;
        mat.m[0][3] = m03;
        mat.m[1][0] = m10;
        mat.m[1][1] = m11;
        mat.m[1][2] = m12;
        mat.m[1][3] = m13;
        mat.m[2][0] = m20;
        mat.m[2][1] = m21;
        mat.m[2][2] = m22;
        mat.m[2][3] = m23;
        mat.m[3][0] = m30;
        mat.m[3][1] = m31;
        mat.m[3][2] = m32;
        mat.m[3][3] = m33;
        return mat;
    }

    // Plane operations (used by frustum culling)
    inline XMVECTOR XMPlaneNormalize(XMVECTOR plane)
    {
        float len = sqrtf(plane.x * plane.x + plane.y * plane.y + plane.z * plane.z);
        if (len > 1e-8f)
        {
            float invLen = 1.0f / len;
            return {plane.x * invLen, plane.y * invLen, plane.z * invLen, plane.w * invLen};
        }
        return plane;
    }

    inline XMVECTOR XMPlaneDotCoord(XMVECTOR plane, XMVECTOR point)
    {
        float d = plane.x * point.x + plane.y * point.y + plane.z * point.z + plane.w;
        return {d, d, d, d};
    }

} // namespace DirectX

// ============================================================================
// D3D11 Type Stubs (opaque forward declarations for non-Windows)
// ============================================================================

// These are forward-declared as opaque types so that headers referencing
// D3D11 types can compile on non-Windows platforms. The actual implementations
// are never instantiated on non-Windows.

struct ID3D11Device;
struct ID3D11Device1;
struct ID3D11DeviceContext;
struct ID3D11DeviceContext1;
struct IDXGISwapChain;
struct IDXGISwapChain1;
struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;
struct ID3D11ShaderResourceView;
struct ID3D11UnorderedAccessView;
struct ID3D11Texture2D;
struct ID3D11Buffer;
struct ID3D11SamplerState;
struct ID3D11RasterizerState;
struct ID3D11DepthStencilState;
struct ID3D11BlendState;
struct ID3D11VertexShader;
struct ID3D11PixelShader;
struct ID3D11ComputeShader;
struct ID3D11GeometryShader;
struct ID3D11HullShader;
struct ID3D11DomainShader;
struct ID3D11InputLayout;
struct ID3D11Query;
struct ID3DBlob;
struct IDXGIAdapter;
struct IDXGIFactory;
struct IDXGIFactory2;
struct IDXGIOutput;
struct IDXGIDebug;
struct ID3D11Resource;
struct ID3D11DeviceChild;

// FILETIME stub
struct FILETIME
{
    uint32_t dwLowDateTime;
    uint32_t dwHighDateTime;
};

// Minimal ComPtr stub for non-Windows
namespace Microsoft
{
    namespace WRL
    {
        template <typename T> class ComPtr
        {
            T* ptr = nullptr;

          public:
            ComPtr() = default;
            ~ComPtr() { /* No Release() on stubs */ }
            ComPtr(const ComPtr&) = default;
            ComPtr& operator=(const ComPtr&) = default;
            ComPtr(ComPtr&& o) noexcept : ptr(o.ptr) { o.ptr = nullptr; }
            ComPtr& operator=(ComPtr&& o) noexcept
            {
                ptr = o.ptr;
                o.ptr = nullptr;
                return *this;
            }
            T* Get() const { return ptr; }
            T** GetAddressOf() { return &ptr; }
            T** ReleaseAndGetAddressOf()
            {
                ptr = nullptr;
                return &ptr;
            }
            T* operator->() const { return ptr; }
            T& operator*() const { return *ptr; }
            operator bool() const { return ptr != nullptr; }
            bool operator==(std::nullptr_t) const { return ptr == nullptr; }
            bool operator!=(std::nullptr_t) const { return ptr != nullptr; }
            void Reset() { ptr = nullptr; }
        };
    } // namespace WRL
} // namespace Microsoft

// DXGI_FORMAT stub
#ifndef DXGI_FORMAT_UNKNOWN
enum DXGI_FORMAT
{
    DXGI_FORMAT_UNKNOWN = 0,
    DXGI_FORMAT_R8G8B8A8_UNORM = 28,
    DXGI_FORMAT_R32G32B32A32_FLOAT = 2,
    DXGI_FORMAT_R32G32B32_FLOAT = 6,
    DXGI_FORMAT_R32G32_FLOAT = 16,
    DXGI_FORMAT_D24_UNORM_S8_UINT = 45,
    DXGI_FORMAT_R16G16B16A16_FLOAT = 10,
};
#endif

// D3D11 enum stubs
#ifndef D3D11_BIND_VERTEX_BUFFER
enum D3D11_BIND_FLAG
{
    D3D11_BIND_VERTEX_BUFFER = 0x1,
    D3D11_BIND_INDEX_BUFFER = 0x2,
    D3D11_BIND_CONSTANT_BUFFER = 0x4,
    D3D11_BIND_SHADER_RESOURCE = 0x8,
    D3D11_BIND_RENDER_TARGET = 0x20,
    D3D11_BIND_DEPTH_STENCIL = 0x40,
};
#endif

#ifndef D3D11_FILTER_MIN_MAG_MIP_LINEAR
enum D3D11_FILTER
{
    D3D11_FILTER_MIN_MAG_MIP_POINT = 0,
    D3D11_FILTER_MIN_MAG_MIP_LINEAR = 0x15,
    D3D11_FILTER_ANISOTROPIC = 0x55,
};
enum D3D11_TEXTURE_ADDRESS_MODE
{
    D3D11_TEXTURE_ADDRESS_WRAP = 1,
    D3D11_TEXTURE_ADDRESS_MIRROR = 2,
    D3D11_TEXTURE_ADDRESS_CLAMP = 3,
    D3D11_TEXTURE_ADDRESS_BORDER = 4,
};
constexpr float D3D11_FLOAT32_MAX = 3.402823466e+38F;
#endif

// XMUINT4 type
namespace DirectX
{
    struct XMUINT4
    {
        uint32_t x, y, z, w;
        XMUINT4() : x(0), y(0), z(0), w(0) {}
        XMUINT4(uint32_t _x, uint32_t _y, uint32_t _z, uint32_t _w) : x(_x), y(_y), z(_z), w(_w) {}
    };
} // namespace DirectX

// On non-Windows, bring DirectX stub types into global scope so that
// existing code using bare XMFLOAT3, XMVECTOR, etc. compiles without
// requiring DirectX:: qualification everywhere.
using namespace DirectX;

#endif // !SPARK_PLATFORM_WINDOWS
