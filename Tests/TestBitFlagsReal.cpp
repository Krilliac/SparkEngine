/**
 * @file TestBitFlagsReal.cpp
 * @brief Real-class tests for Spark::BitFlags<E>
 *
 * Phase JJ deep-wire.
 */

#include "TestFramework.h"
#include "Utils/BitFlags.h"

namespace
{
    enum class TestFlag : uint32_t
    {
        None = 0,
        Red = 1 << 0,
        Green = 1 << 1,
        Blue = 1 << 2,
        Alpha = 1 << 3
    };
} // namespace

TEST(BitFlagsReal_DefaultEmpty)
{
    Spark::BitFlags<TestFlag> flags;
    EXPECT_FALSE(flags.Has(TestFlag::Red));
    EXPECT_FALSE(flags.Has(TestFlag::Green));
    EXPECT_FALSE(flags.Has(TestFlag::Blue));
    EXPECT_FALSE(flags.Has(TestFlag::Alpha));
}

TEST(BitFlagsReal_ConstructFromSingle)
{
    Spark::BitFlags<TestFlag> flags{TestFlag::Red};
    EXPECT_TRUE(flags.Has(TestFlag::Red));
    EXPECT_FALSE(flags.Has(TestFlag::Green));
}

TEST(BitFlagsReal_SetClearToggle)
{
    Spark::BitFlags<TestFlag> flags;
    flags.Set(TestFlag::Red);
    EXPECT_TRUE(flags.Has(TestFlag::Red));

    flags.Set(TestFlag::Blue);
    EXPECT_TRUE(flags.Has(TestFlag::Red));
    EXPECT_TRUE(flags.Has(TestFlag::Blue));

    flags.Clear(TestFlag::Red);
    EXPECT_FALSE(flags.Has(TestFlag::Red));
    EXPECT_TRUE(flags.Has(TestFlag::Blue));

    flags.Toggle(TestFlag::Blue);
    EXPECT_FALSE(flags.Has(TestFlag::Blue));

    flags.Toggle(TestFlag::Alpha);
    EXPECT_TRUE(flags.Has(TestFlag::Alpha));
}

TEST(BitFlagsReal_ExplicitRawConstructor)
{
    Spark::BitFlags<TestFlag> flags(static_cast<uint32_t>(0b1111));
    EXPECT_TRUE(flags.Has(TestFlag::Red));
    EXPECT_TRUE(flags.Has(TestFlag::Green));
    EXPECT_TRUE(flags.Has(TestFlag::Blue));
    EXPECT_TRUE(flags.Has(TestFlag::Alpha));
}
