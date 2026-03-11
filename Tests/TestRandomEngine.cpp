// TestRandomEngine.cpp - Tests for Spark::RandomEngine
#include "TestFramework.h"
#include "Utils/RandomEngine.h"
#include <set>

TEST(RandomEngine_FloatInRange)
{
    Spark::RandomEngine rng(42);
    for (int i = 0; i < 1000; ++i)
    {
        float val = rng.Float(5.0f, 10.0f);
        EXPECT_GE(val, 5.0f);
        EXPECT_LE(val, 10.0f);
    }
}

TEST(RandomEngine_IntInRange)
{
    Spark::RandomEngine rng(42);
    for (int i = 0; i < 1000; ++i)
    {
        int val = rng.Int(1, 6);
        EXPECT_GE(val, 1);
        EXPECT_LE(val, 6);
    }
}

TEST(RandomEngine_BoolProbability)
{
    Spark::RandomEngine rng(42);
    int trueCount = 0;
    constexpr int TRIALS = 10000;
    for (int i = 0; i < TRIALS; ++i)
    {
        if (rng.Bool(0.3f))
            ++trueCount;
    }
    // Expect roughly 30% true, allow wide margin
    float ratio = static_cast<float>(trueCount) / TRIALS;
    EXPECT_GT(ratio, 0.2f);
    EXPECT_LT(ratio, 0.4f);
}

TEST(RandomEngine_DeterministicSeed)
{
    Spark::RandomEngine rng1(12345);
    Spark::RandomEngine rng2(12345);
    for (int i = 0; i < 100; ++i)
    {
        EXPECT_EQ(rng1.Int(0, 1000), rng2.Int(0, 1000));
    }
}

TEST(RandomEngine_Gaussian)
{
    Spark::RandomEngine rng(42);
    float sum = 0.0f;
    constexpr int N = 10000;
    for (int i = 0; i < N; ++i)
    {
        sum += rng.Gaussian(5.0f, 1.0f);
    }
    float mean = sum / N;
    EXPECT_NEAR(mean, 5.0f, 0.2f);
}

TEST(RandomEngine_WeightedIndex)
{
    Spark::RandomEngine rng(42);
    std::vector<float> weights = {0.0f, 1.0f, 0.0f};
    for (int i = 0; i < 100; ++i)
    {
        EXPECT_EQ(rng.WeightedIndex(weights), 1);
    }
}

TEST(RandomEngine_WeightedIndexEmpty)
{
    Spark::RandomEngine rng(42);
    std::vector<float> weights;
    EXPECT_EQ(rng.WeightedIndex(weights), 0);
}

TEST(RandomEngine_UnitFloat)
{
    Spark::RandomEngine rng(42);
    for (int i = 0; i < 100; ++i)
    {
        float val = rng.UnitFloat();
        EXPECT_GE(val, 0.0f);
        EXPECT_LE(val, 1.0f);
    }
}

TEST(RandomEngine_Shuffle)
{
    Spark::RandomEngine rng(42);
    std::vector<int> v = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    std::vector<int> original = v;
    rng.Shuffle(v);
    // After shuffle, should still contain same elements
    std::set<int> origSet(original.begin(), original.end());
    std::set<int> shuffledSet(v.begin(), v.end());
    EXPECT_EQ(origSet.size(), shuffledSet.size());
    EXPECT_TRUE(origSet == shuffledSet);
}

TEST(RandomEngine_Pick)
{
    Spark::RandomEngine rng(42);
    std::vector<std::string> items = {"sword", "shield", "potion"};
    for (int i = 0; i < 100; ++i)
    {
        const auto& pick = rng.Pick(items);
        EXPECT_TRUE(pick == "sword" || pick == "shield" || pick == "potion");
    }
}

TEST(ThreadSafeRandomEngine_Basic)
{
    Spark::ThreadSafeRandomEngine rng(42);
    float f = rng.Float(0.0f, 1.0f);
    EXPECT_GE(f, 0.0f);
    EXPECT_LE(f, 1.0f);
    int i = rng.Int(1, 10);
    EXPECT_GE(i, 1);
    EXPECT_LE(i, 10);
}
