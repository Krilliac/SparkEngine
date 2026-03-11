// TestBitFlags.cpp - Tests for Spark::BitFlags and SPARK_ENABLE_BITMASK_OPERATORS
#include "TestFramework.h"
#include "Utils/BitFlags.h"

enum class TestFlag : uint32_t
{
    None = 0,
    Read = 1 << 0,
    Write = 1 << 1,
    Execute = 1 << 2,
    All = Read | Write | Execute
};
SPARK_ENABLE_BITMASK_OPERATORS(TestFlag)

TEST(BitFlags_DefaultEmpty)
{
    Spark::BitFlags<TestFlag> flags;
    EXPECT_TRUE(flags.IsEmpty());
    EXPECT_EQ(flags.GetRaw(), 0u);
}

TEST(BitFlags_SetAndHas)
{
    Spark::BitFlags<TestFlag> flags;
    flags.Set(TestFlag::Read);
    EXPECT_TRUE(flags.Has(TestFlag::Read));
    EXPECT_FALSE(flags.Has(TestFlag::Write));
}

TEST(BitFlags_SetMultiple)
{
    Spark::BitFlags<TestFlag> flags;
    flags.Set(TestFlag::Read);
    flags.Set(TestFlag::Write);
    EXPECT_TRUE(flags.Has(TestFlag::Read));
    EXPECT_TRUE(flags.Has(TestFlag::Write));
    EXPECT_FALSE(flags.Has(TestFlag::Execute));
}

TEST(BitFlags_Clear)
{
    Spark::BitFlags<TestFlag> flags;
    flags.Set(TestFlag::Read);
    flags.Set(TestFlag::Write);
    flags.Clear(TestFlag::Read);
    EXPECT_FALSE(flags.Has(TestFlag::Read));
    EXPECT_TRUE(flags.Has(TestFlag::Write));
}

TEST(BitFlags_Toggle)
{
    Spark::BitFlags<TestFlag> flags;
    flags.Toggle(TestFlag::Read);
    EXPECT_TRUE(flags.Has(TestFlag::Read));
    flags.Toggle(TestFlag::Read);
    EXPECT_FALSE(flags.Has(TestFlag::Read));
}

TEST(BitFlags_HasAny)
{
    Spark::BitFlags<TestFlag> flags;
    flags.Set(TestFlag::Read);
    EXPECT_TRUE(flags.HasAny(TestFlag::Read));
    EXPECT_FALSE(flags.HasAny(TestFlag::Write));
}

TEST(BitFlags_ClearAll)
{
    Spark::BitFlags<TestFlag> flags;
    flags.Set(TestFlag::Read);
    flags.Set(TestFlag::Write);
    flags.ClearAll();
    EXPECT_TRUE(flags.IsEmpty());
}

TEST(BitFlags_ConstructFromEnum)
{
    Spark::BitFlags<TestFlag> flags(TestFlag::Execute);
    EXPECT_TRUE(flags.Has(TestFlag::Execute));
    EXPECT_FALSE(flags.Has(TestFlag::Read));
}

TEST(BitFlags_Equality)
{
    Spark::BitFlags<TestFlag> a(TestFlag::Read);
    Spark::BitFlags<TestFlag> b(TestFlag::Read);
    Spark::BitFlags<TestFlag> c(TestFlag::Write);
    EXPECT_TRUE(a == b);
    EXPECT_TRUE(a != c);
}

// Test SPARK_ENABLE_BITMASK_OPERATORS
TEST(BitmaskOperators_Or)
{
    TestFlag combined = TestFlag::Read | TestFlag::Write;
    EXPECT_TRUE((combined & TestFlag::Read) != TestFlag::None);
    EXPECT_TRUE((combined & TestFlag::Write) != TestFlag::None);
    EXPECT_TRUE((combined & TestFlag::Execute) == TestFlag::None);
}

TEST(BitmaskOperators_And)
{
    TestFlag all = TestFlag::All;
    TestFlag readOnly = all & TestFlag::Read;
    EXPECT_TRUE(readOnly == TestFlag::Read);
}

TEST(BitmaskOperators_Xor)
{
    TestFlag rw = TestFlag::Read | TestFlag::Write;
    TestFlag toggled = rw ^ TestFlag::Read;
    EXPECT_TRUE((toggled & TestFlag::Read) == TestFlag::None);
    EXPECT_TRUE((toggled & TestFlag::Write) != TestFlag::None);
}

TEST(BitmaskOperators_Not)
{
    TestFlag notRead = ~TestFlag::Read;
    EXPECT_TRUE((notRead & TestFlag::Read) == TestFlag::None);
}

TEST(BitmaskOperators_CompoundAssign)
{
    TestFlag flags = TestFlag::None;
    flags |= TestFlag::Read;
    flags |= TestFlag::Write;
    EXPECT_TRUE((flags & TestFlag::Read) != TestFlag::None);
    EXPECT_TRUE((flags & TestFlag::Write) != TestFlag::None);
    flags &= ~TestFlag::Read;
    EXPECT_TRUE((flags & TestFlag::Read) == TestFlag::None);
}
