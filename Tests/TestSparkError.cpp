// TestSparkError.cpp - Tests for SparkError utilities

#include "TestFramework.h"
#include "Utils/SparkError.h"

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
    // Redirect stderr to suppress intentional [ERROR] output that causes CTest
    // on Windows to misinterpret the test as failed
    std::FILE* origStderr = stderr;
#ifdef _WIN32
    std::FILE* devnull = std::freopen("NUL", "w", stderr);
#else
    std::FILE* devnull = std::freopen("/dev/null", "w", stderr);
#endif
    (void)devnull;

    bool result = SparkError::CheckFailed("false", __FILE__, __LINE__, __FUNCTION__);

    // Restore stderr
#ifdef _WIN32
    std::freopen("CON", "w", stderr);
#else
    std::freopen("/dev/tty", "w", stderr);
#endif
    (void)origStderr;

    EXPECT_FALSE(result);
}

TEST(SparkError_CheckFailedWithMsg)
{
#ifdef _WIN32
    std::freopen("NUL", "w", stderr);
#else
    std::freopen("/dev/null", "w", stderr);
#endif

    bool result = SparkError::CheckFailed("expr", __FILE__, __LINE__, __FUNCTION__, "custom message");

#ifdef _WIN32
    std::freopen("CON", "w", stderr);
#else
    std::freopen("/dev/tty", "w", stderr);
#endif

    EXPECT_FALSE(result);
}

TEST(SparkError_BoundsCheckFailed)
{
#ifdef _WIN32
    std::freopen("NUL", "w", stderr);
#else
    std::freopen("/dev/null", "w", stderr);
#endif

    bool result = SparkError::BoundsCheckFailed("idx", 10, 5, __FILE__, __LINE__, __FUNCTION__);

#ifdef _WIN32
    std::freopen("CON", "w", stderr);
#else
    std::freopen("/dev/tty", "w", stderr);
#endif

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
#ifdef _WIN32
    std::freopen("NUL", "w", stderr);
#else
    std::freopen("/dev/null", "w", stderr);
#endif

    bool result = SPARK_CHECK(1 == 2);

#ifdef _WIN32
    std::freopen("CON", "w", stderr);
#else
    std::freopen("/dev/tty", "w", stderr);
#endif

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
#ifdef _WIN32
    std::freopen("NUL", "w", stderr);
#else
    std::freopen("/dev/null", "w", stderr);
#endif

    bool result = SPARK_CHECK_MSG(false, "expected failure");

#ifdef _WIN32
    std::freopen("CON", "w", stderr);
#else
    std::freopen("/dev/tty", "w", stderr);
#endif

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
#ifdef _WIN32
    std::freopen("NUL", "w", stderr);
#else
    std::freopen("/dev/null", "w", stderr);
#endif

    bool result = SPARK_BOUNDS_CHECK(10, 5);

#ifdef _WIN32
    std::freopen("CON", "w", stderr);
#else
    std::freopen("/dev/tty", "w", stderr);
#endif

    EXPECT_FALSE(result);
}

TEST(SparkError_BoundsCheckNegativeIndex)
{
#ifdef _WIN32
    std::freopen("NUL", "w", stderr);
#else
    std::freopen("/dev/null", "w", stderr);
#endif

    bool result = SPARK_BOUNDS_CHECK(-1, 5);

#ifdef _WIN32
    std::freopen("CON", "w", stderr);
#else
    std::freopen("/dev/tty", "w", stderr);
#endif

    EXPECT_FALSE(result);
}

// =============================================================================
// SPARK_CATCH_ALL macro
// =============================================================================

TEST(SparkError_CatchAllStdException)
{
#ifdef _WIN32
    std::freopen("NUL", "w", stderr);
#else
    std::freopen("/dev/null", "w", stderr);
#endif

    bool reached = false;
    SPARK_CATCH_ALL("Test", { throw std::runtime_error("test error"); });
    reached = true;

#ifdef _WIN32
    std::freopen("CON", "w", stderr);
#else
    std::freopen("/dev/tty", "w", stderr);
#endif

    EXPECT_TRUE(reached); // Execution continues after catch
}

TEST(SparkError_CatchAllUnknownException)
{
#ifdef _WIN32
    std::freopen("NUL", "w", stderr);
#else
    std::freopen("/dev/null", "w", stderr);
#endif

    bool reached = false;
    SPARK_CATCH_ALL("Test", {
        throw 42; // non-std exception
    });
    reached = true;

#ifdef _WIN32
    std::freopen("CON", "w", stderr);
#else
    std::freopen("/dev/tty", "w", stderr);
#endif

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
#ifdef _WIN32
    std::freopen("NUL", "w", stderr);
#else
    std::freopen("/dev/null", "w", stderr);
#endif

    int result = SPARK_CATCH_ALL_RET("Test", -1, {
        throw std::runtime_error("error");
        return 42;
    });

#ifdef _WIN32
    std::freopen("CON", "w", stderr);
#else
    std::freopen("/dev/tty", "w", stderr);
#endif

    EXPECT_EQ(result, -1);
}

TEST(SparkError_CatchAllRetReturnsNormal)
{
    int result = SPARK_CATCH_ALL_RET("Test", -1, { return 42; });

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
#ifdef _WIN32
    std::freopen("NUL", "w", stderr);
#else
    std::freopen("/dev/null", "w", stderr);
#endif

    bool result = SparkError::HResultFailed("hrExpr", -1, __FILE__, __LINE__, __FUNCTION__);

#ifdef _WIN32
    std::freopen("CON", "w", stderr);
#else
    std::freopen("/dev/tty", "w", stderr);
#endif

    EXPECT_FALSE(result);
}

TEST(SparkError_HResultFailedWithMsg)
{
#ifdef _WIN32
    std::freopen("NUL", "w", stderr);
#else
    std::freopen("/dev/null", "w", stderr);
#endif

    bool result = SparkError::HResultFailed("hrExpr", -1, __FILE__, __LINE__, __FUNCTION__, "custom msg");

#ifdef _WIN32
    std::freopen("CON", "w", stderr);
#else
    std::freopen("/dev/tty", "w", stderr);
#endif

    EXPECT_FALSE(result);
}
