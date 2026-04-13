/**
 * @file TestHResultPlatform.cpp
 * @brief Regression tests for HRESULT type and SUCCEEDED/FAILED macros
 *
 * On 64-bit Linux with HRESULT=long (8 bytes), error codes like E_FAIL
 * (0x80004005) have the high bit clear and are positive, making
 * SUCCEEDED() return true for every failure code. These tests lock in
 * the fix: HRESULT must be a 32-bit signed type so sign bits work.
 */

#include "TestFramework.h"
// On Linux/macOS, PlatformTypes.h provides the HRESULT type and macros used
// below. On Windows, these come from the system's <windows.h> — include it
// directly so this test compiles on both platforms.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include "Core/PlatformTypes.h"
#endif
#include <cstdint>
#include <type_traits>

// ============================================================================
// HRESULT type identity
// ============================================================================

TEST(HResult_IsSigned)
{
    EXPECT_TRUE(std::is_signed<HRESULT>::value);
}

TEST(HResult_Is32BitWide)
{
    // HRESULT must be exactly 32 bits — matches Windows ABI and ensures
    // high-bit error codes are negative.
    EXPECT_EQ(sizeof(HRESULT), 4u);
}

// ============================================================================
// Error code values
// ============================================================================

TEST(HResult_SOK_IsZero)
{
    EXPECT_EQ(S_OK, static_cast<HRESULT>(0));
}

TEST(HResult_SFALSE_IsOne)
{
    EXPECT_EQ(S_FALSE, static_cast<HRESULT>(1));
}

TEST(HResult_EFAIL_IsNegative)
{
    // 0x80004005 on a 32-bit signed type has the high bit set, making it
    // negative. This is the whole point of the fix — it was previously
    // positive when HRESULT was 'long' on 64-bit systems.
    EXPECT_TRUE(E_FAIL < 0);
}

TEST(HResult_EINVALIDARG_IsNegative)
{
    EXPECT_TRUE(E_INVALIDARG < 0);
}

TEST(HResult_EOUTOFMEMORY_IsNegative)
{
    EXPECT_TRUE(E_OUTOFMEMORY < 0);
}

TEST(HResult_ENOTIMPL_IsNegative)
{
    EXPECT_TRUE(E_NOTIMPL < 0);
}

TEST(HResult_ENOINTERFACE_IsNegative)
{
    EXPECT_TRUE(E_NOINTERFACE < 0);
}

TEST(HResult_EPOINTER_IsNegative)
{
    EXPECT_TRUE(E_POINTER < 0);
}

TEST(HResult_EABORT_IsNegative)
{
    EXPECT_TRUE(E_ABORT < 0);
}

TEST(HResult_EUNEXPECTED_IsNegative)
{
    EXPECT_TRUE(E_UNEXPECTED < 0);
}

TEST(HResult_DXGIErrors_AreNegative)
{
    EXPECT_TRUE(DXGI_ERROR_DEVICE_REMOVED < 0);
    EXPECT_TRUE(DXGI_ERROR_DEVICE_RESET < 0);
}

// ============================================================================
// SUCCEEDED / FAILED macros
// ============================================================================

TEST(HResult_SUCCEEDED_ReturnsTrueForSOK)
{
    EXPECT_TRUE(SUCCEEDED(S_OK));
}

TEST(HResult_SUCCEEDED_ReturnsTrueForSFALSE)
{
    // S_FALSE is not an error — it's a "successful but no change" signal
    EXPECT_TRUE(SUCCEEDED(S_FALSE));
}

TEST(HResult_SUCCEEDED_ReturnsFalseForEFAIL)
{
    // Critical regression test: before the fix, this returned true on Linux
    EXPECT_TRUE(!SUCCEEDED(E_FAIL));
}

TEST(HResult_SUCCEEDED_ReturnsFalseForEInvalidArg)
{
    EXPECT_TRUE(!SUCCEEDED(E_INVALIDARG));
}

TEST(HResult_SUCCEEDED_ReturnsFalseForAllErrorCodes)
{
    EXPECT_TRUE(!SUCCEEDED(E_FAIL));
    EXPECT_TRUE(!SUCCEEDED(E_INVALIDARG));
    EXPECT_TRUE(!SUCCEEDED(E_OUTOFMEMORY));
    EXPECT_TRUE(!SUCCEEDED(E_NOTIMPL));
    EXPECT_TRUE(!SUCCEEDED(E_NOINTERFACE));
    EXPECT_TRUE(!SUCCEEDED(E_POINTER));
    EXPECT_TRUE(!SUCCEEDED(E_ABORT));
    EXPECT_TRUE(!SUCCEEDED(E_UNEXPECTED));
    EXPECT_TRUE(!SUCCEEDED(DXGI_ERROR_DEVICE_REMOVED));
    EXPECT_TRUE(!SUCCEEDED(DXGI_ERROR_DEVICE_RESET));
}

TEST(HResult_FAILED_ReturnsFalseForSOK)
{
    EXPECT_TRUE(!FAILED(S_OK));
}

TEST(HResult_FAILED_ReturnsTrueForEFAIL)
{
    EXPECT_TRUE(FAILED(E_FAIL));
}

TEST(HResult_FAILED_ReturnsTrueForAllErrorCodes)
{
    EXPECT_TRUE(FAILED(E_FAIL));
    EXPECT_TRUE(FAILED(E_INVALIDARG));
    EXPECT_TRUE(FAILED(E_OUTOFMEMORY));
    EXPECT_TRUE(FAILED(E_NOTIMPL));
    EXPECT_TRUE(FAILED(E_NOINTERFACE));
    EXPECT_TRUE(FAILED(E_POINTER));
    EXPECT_TRUE(FAILED(E_ABORT));
    EXPECT_TRUE(FAILED(E_UNEXPECTED));
    EXPECT_TRUE(FAILED(DXGI_ERROR_DEVICE_REMOVED));
    EXPECT_TRUE(FAILED(DXGI_ERROR_DEVICE_RESET));
}

// ============================================================================
// SUCCEEDED and FAILED are mutually exclusive
// ============================================================================

TEST(HResult_SUCCEEDEDAndFAILED_AreMutuallyExclusive)
{
    HRESULT codes[] = {
        S_OK,
        S_FALSE,
        E_FAIL,
        E_INVALIDARG,
        E_OUTOFMEMORY,
        E_NOTIMPL,
        E_NOINTERFACE,
        E_POINTER,
        E_ABORT,
        E_UNEXPECTED,
        DXGI_ERROR_DEVICE_REMOVED,
        DXGI_ERROR_DEVICE_RESET,
    };
    for (HRESULT hr : codes)
    {
        // Exactly one of SUCCEEDED/FAILED must be true
        EXPECT_TRUE(SUCCEEDED(hr) != FAILED(hr));
    }
}

// ============================================================================
// Custom error code construction
// ============================================================================

TEST(HResult_CustomHighBitError_IsNegative)
{
    // Any HRESULT with the high bit set must be negative
    HRESULT customErr = static_cast<HRESULT>(0x80001234);
    EXPECT_TRUE(customErr < 0);
    EXPECT_TRUE(FAILED(customErr));
    EXPECT_TRUE(!SUCCEEDED(customErr));
}

TEST(HResult_CustomLowBitError_IsPositive)
{
    // Low-bit "success" codes remain positive
    HRESULT customSuccess = static_cast<HRESULT>(0x00001234);
    EXPECT_TRUE(customSuccess >= 0);
    EXPECT_TRUE(!FAILED(customSuccess));
    EXPECT_TRUE(SUCCEEDED(customSuccess));
}
