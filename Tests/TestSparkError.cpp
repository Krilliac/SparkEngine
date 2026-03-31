// TestSparkError.cpp - Tests for SparkError utilities

#include "TestFramework.h"
#include "Utils/SparkError.h"

#ifdef _WIN32
#include <io.h>
#define dup _dup
#define dup2 _dup2
#define fileno _fileno
#else
#include <unistd.h>
#endif

// RAII helper to suppress stderr output during tests that intentionally
// trigger error logging, then restore it reliably (works on CI without a console).
struct SuppressStderr
{
    int savedFd = -1;
    SuppressStderr()
    {
        fflush(stderr);
        savedFd = dup(fileno(stderr));
#ifdef _WIN32
        freopen("NUL", "w", stderr);
#else
        freopen("/dev/null", "w", stderr);
#endif
    }
    ~SuppressStderr()
    {
        fflush(stderr);
        if (savedFd >= 0)
        {
            dup2(savedFd, fileno(stderr));
#ifdef _WIN32
            _close(savedFd);
#else
            close(savedFd);
#endif
        }
    }
};

TEST(SparkError_SeverityToString)
{
    EXPECT_EQ(std::string(SparkError::SeverityToString(SparkError::Severity::Trace)), std::string("TRACE"));
    EXPECT_EQ(std::string(SparkError::SeverityToString(SparkError::Severity::Debug)), std::string("DEBUG"));
    EXPECT_EQ(std::string(SparkError::SeverityToString(SparkError::Severity::Info)), std::string("INFO "));
    EXPECT_EQ(std::string(SparkError::SeverityToString(SparkError::Severity::Warn)), std::string("WARN "));
    EXPECT_EQ(std::string(SparkError::SeverityToString(SparkError::Severity::Error)), std::string("ERROR"));
    EXPECT_EQ(std::string(SparkError::SeverityToString(SparkError::Severity::Fatal)), std::string("FATAL"));
}

TEST(SparkError_CheckFailedReturnsFalse)
{
    SuppressStderr guard;
    bool result = SparkError::CheckFailed("false", __FILE__, __LINE__, __FUNCTION__);
    EXPECT_FALSE(result);
}

TEST(SparkError_CheckFailedWithMsg)
{
    SuppressStderr guard;
    bool result = SparkError::CheckFailed("expr", __FILE__, __LINE__, __FUNCTION__, "custom message");
    EXPECT_FALSE(result);
}

TEST(SparkError_BoundsCheckFailed)
{
    SuppressStderr guard;
    bool result = SparkError::BoundsCheckFailed("idx", 10, 5, __FILE__, __LINE__, __FUNCTION__);
    EXPECT_FALSE(result);
}

TEST(SparkError_LogMessageDoesNotCrash)
{
    EXPECT_NO_THROW(SparkError::LogMessage(SparkError::Severity::Info, "Test", __FILE__, __LINE__, __FUNCTION__,
                                           "Test message %d", 42));
}

TEST(SparkError_ScopedContextDoesNotCrash)
{
    EXPECT_NO_THROW({
        SparkError::ScopedContext ctx("TestContext", __FILE__, __LINE__);
        // Context logs on enter and exit
    });
}

// =============================================================================
// SPARK_CHECK macro
// =============================================================================

TEST(SparkError_CheckPassesOnTrue)
{
    bool result = SPARK_CHECK(1 == 1);
    EXPECT_TRUE(result);
}

TEST(SparkError_CheckFailsOnFalse)
{
    SuppressStderr guard;
    bool result = SPARK_CHECK(1 == 2);
    EXPECT_FALSE(result);
}

// =============================================================================
// SPARK_CHECK_MSG macro
// =============================================================================

TEST(SparkError_CheckMsgPassesOnTrue)
{
    bool result = SPARK_CHECK_MSG(true, "should pass");
    EXPECT_TRUE(result);
}

TEST(SparkError_CheckMsgFailsOnFalse)
{
    SuppressStderr guard;
    bool result = SPARK_CHECK_MSG(false, "expected failure");
    EXPECT_FALSE(result);
}

// =============================================================================
// SPARK_BOUNDS_CHECK macro
// =============================================================================

TEST(SparkError_BoundsCheckInRange)
{
    bool result = SPARK_BOUNDS_CHECK(3, 10);
    EXPECT_TRUE(result);
}

TEST(SparkError_BoundsCheckAtZero)
{
    bool result = SPARK_BOUNDS_CHECK(0, 5);
    EXPECT_TRUE(result);
}

TEST(SparkError_BoundsCheckOutOfRange)
{
    SuppressStderr guard;
    bool result = SPARK_BOUNDS_CHECK(10, 5);
    EXPECT_FALSE(result);
}

TEST(SparkError_BoundsCheckNegativeIndex)
{
    SuppressStderr guard;
    bool result = SPARK_BOUNDS_CHECK(-1, 5);
    EXPECT_FALSE(result);
}

// =============================================================================
// SPARK_CATCH_ALL macro
// =============================================================================

TEST(SparkError_CatchAllStdException)
{
    SuppressStderr guard;
    bool reached = false;
    SPARK_CATCH_ALL("Test", { throw std::runtime_error("test error"); });
    reached = true;
    EXPECT_TRUE(reached);
}

TEST(SparkError_CatchAllUnknownException)
{
    SuppressStderr guard;
    bool reached = false;
    SPARK_CATCH_ALL("Test", { throw 42; });
    reached = true;
    EXPECT_TRUE(reached);
}

TEST(SparkError_CatchAllNoException)
{
    bool reached = false;
    SPARK_CATCH_ALL("Test", {
        int x = 42;
        (void)x;
    });
    reached = true;
    EXPECT_TRUE(reached);
}

// =============================================================================
// SPARK_CATCH_ALL_RET macro
// =============================================================================

TEST(SparkError_CatchAllRetReturnsDefault)
{
    SuppressStderr guard;
    int result = SPARK_CATCH_ALL_RET("Test", -1, { throw std::runtime_error("error"); });
    EXPECT_EQ(result, -1);
}

TEST(SparkError_CatchAllRetReturnsNormal)
{
    int val = 42;
    int result = SPARK_CATCH_ALL_RET("Test", -1, { return val; });
    EXPECT_EQ(result, 42);
}

// =============================================================================
// SeverityToString edge case
// =============================================================================

TEST(SparkError_SeverityToStringUnknown)
{
    auto str = SparkError::SeverityToString(static_cast<SparkError::Severity>(99));
    EXPECT_EQ(std::string(str), std::string("?????"));
}

// =============================================================================
// LogMessage with null arguments
// =============================================================================

TEST(SparkError_LogMessageNullArgs)
{
    EXPECT_NO_THROW(SparkError::LogMessage(SparkError::Severity::Info, nullptr, nullptr, 0, nullptr, "msg %d", 1));
}

// =============================================================================
// HResultFailed (non-Windows just exercises the else branch)
// =============================================================================

TEST(SparkError_HResultFailedNonWindows)
{
    SuppressStderr guard;
    bool result = SparkError::HResultFailed("hrExpr", -1, __FILE__, __LINE__, __FUNCTION__);
    EXPECT_FALSE(result);
}

TEST(SparkError_HResultFailedWithMsg)
{
    SuppressStderr guard;
    bool result = SparkError::HResultFailed("hrExpr", -1, __FILE__, __LINE__, __FUNCTION__, "custom msg");
    EXPECT_FALSE(result);
}
