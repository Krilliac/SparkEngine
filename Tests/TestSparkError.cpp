// TestSparkError.cpp - Tests for SparkError utilities

#include "TestFramework.h"
#include "Utils/SparkError.h"

// Helper macros to suppress stderr for tests that intentionally trigger error logging.
// Uses the same freopen pattern as the original tests that pass on all platforms.
#ifdef _WIN32
#define SUPPRESS_STDERR() std::freopen("NUL", "w", stderr)
#define RESTORE_STDERR() std::freopen("CON", "w", stderr)
#else
#define SUPPRESS_STDERR() std::freopen("/dev/null", "w", stderr)
#define RESTORE_STDERR() std::freopen("/dev/tty", "w", stderr)
#endif

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
    SUPPRESS_STDERR();
    bool result = SparkError::CheckFailed("false", __FILE__, __LINE__, __FUNCTION__);
    RESTORE_STDERR();
    EXPECT_FALSE(result);
}

TEST(SparkError_CheckFailedWithMsg)
{
    SUPPRESS_STDERR();
    bool result = SparkError::CheckFailed("expr", __FILE__, __LINE__, __FUNCTION__, "custom message");
    RESTORE_STDERR();
    EXPECT_FALSE(result);
}

TEST(SparkError_BoundsCheckFailed)
{
    SUPPRESS_STDERR();
    bool result = SparkError::BoundsCheckFailed("idx", 10, 5, __FILE__, __LINE__, __FUNCTION__);
    RESTORE_STDERR();
    EXPECT_FALSE(result);
}

TEST(SparkError_LogMessageDoesNotCrash)
{
    EXPECT_NO_THROW(SparkError::LogMessage(SparkError::Severity::Info, "Test", __FILE__, __LINE__, __FUNCTION__,
                                           "Test message %d", 42));
}

TEST(SparkError_ScopedContextDoesNotCrash)
{
    EXPECT_NO_THROW({ SparkError::ScopedContext ctx("TestContext", __FILE__, __LINE__); });
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
    SUPPRESS_STDERR();
    bool result = SPARK_CHECK(1 == 2);
    RESTORE_STDERR();
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
    SUPPRESS_STDERR();
    bool result = SPARK_CHECK_MSG(false, "expected failure");
    RESTORE_STDERR();
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
    SUPPRESS_STDERR();
    bool result = SPARK_BOUNDS_CHECK(10, 5);
    RESTORE_STDERR();
    EXPECT_FALSE(result);
}

TEST(SparkError_BoundsCheckNegativeIndex)
{
    SUPPRESS_STDERR();
    bool result = SPARK_BOUNDS_CHECK(-1, 5);
    RESTORE_STDERR();
    EXPECT_FALSE(result);
}

// =============================================================================
// SPARK_CATCH_ALL macro
// =============================================================================

TEST(SparkError_CatchAllStdException)
{
    SUPPRESS_STDERR();
    bool reached = false;
    SPARK_CATCH_ALL("Test", { throw std::runtime_error("test error"); });
    reached = true;
    RESTORE_STDERR();
    EXPECT_TRUE(reached);
}

TEST(SparkError_CatchAllUnknownException)
{
    SUPPRESS_STDERR();
    bool reached = false;
    SPARK_CATCH_ALL("Test", { throw 42; });
    reached = true;
    RESTORE_STDERR();
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
    SUPPRESS_STDERR();
    int result = SPARK_CATCH_ALL_RET("Test", -1, { throw std::runtime_error("error"); });
    RESTORE_STDERR();
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
// HResultFailed
// =============================================================================

TEST(SparkError_HResultFailedReturnsFalse)
{
    SUPPRESS_STDERR();
    bool result = SparkError::HResultFailed("hrExpr", -1, __FILE__, __LINE__, __FUNCTION__);
    RESTORE_STDERR();
    EXPECT_FALSE(result);
}

TEST(SparkError_HResultFailedWithMsg)
{
    SUPPRESS_STDERR();
    bool result = SparkError::HResultFailed("hrExpr", -1, __FILE__, __LINE__, __FUNCTION__, "custom msg");
    RESTORE_STDERR();
    EXPECT_FALSE(result);
}
