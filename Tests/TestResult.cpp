/**
 * @file TestResult.cpp
 * @brief Tests for Spark::Result<T> error handling type
 */

#include "TestFramework.h"
#include "../SparkEngine/Source/Utils/Result.h"

// ============================================================================
// Result<T> Tests
// ============================================================================

TEST(Result_OkInt) {
    Spark::Result<int> r(42);
    EXPECT_TRUE(r.IsOk());
    EXPECT_FALSE(r.IsErr());
    EXPECT_TRUE(static_cast<bool>(r));
    EXPECT_EQ(r.Value(), 42);
}

TEST(Result_ErrInt) {
    Spark::Result<int> r(Spark::Err("not found", 404));
    EXPECT_FALSE(r.IsOk());
    EXPECT_TRUE(r.IsErr());
    EXPECT_FALSE(static_cast<bool>(r));
    EXPECT_EQ(r.GetError().code, 404);
    EXPECT_EQ(r.ErrorMessage(), std::string("not found"));
}

TEST(Result_ValueOr) {
    Spark::Result<int> ok(42);
    Spark::Result<int> err(Spark::Err("oops"));

    EXPECT_EQ(ok.ValueOr(0), 42);
    EXPECT_EQ(err.ValueOr(99), 99);
}

TEST(Result_OkString) {
    Spark::Result<std::string> r(std::string("hello"));
    EXPECT_TRUE(r.IsOk());
    EXPECT_EQ(r.Value(), std::string("hello"));
}

TEST(Result_VoidOk) {
    Spark::Result<void> r = Spark::Ok();
    EXPECT_TRUE(r.IsOk());
    EXPECT_FALSE(r.IsErr());
}

TEST(Result_VoidErr) {
    Spark::Result<void> r(Spark::Err("failed"));
    EXPECT_FALSE(r.IsOk());
    EXPECT_TRUE(r.IsErr());
    EXPECT_EQ(r.ErrorMessage(), std::string("failed"));
}

TEST(Result_OkHelper) {
    auto r = Spark::Ok(123);
    EXPECT_TRUE(r.IsOk());
    EXPECT_EQ(r.Value(), 123);
}

TEST(Result_ErrorMessageOnOk) {
    Spark::Result<int> r(42);
    EXPECT_EQ(r.ErrorMessage(), std::string(""));
}
