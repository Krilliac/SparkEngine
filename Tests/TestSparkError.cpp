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
    bool result = SparkError::CheckFailed("false", __FILE__, __LINE__, __FUNCTION__);
    EXPECT_FALSE(result);
}

TEST(SparkError_CheckFailedWithMsg)
{
    bool result = SparkError::CheckFailed("expr", __FILE__, __LINE__, __FUNCTION__, "custom message");
    EXPECT_FALSE(result);
}

TEST(SparkError_BoundsCheckFailed)
{
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
