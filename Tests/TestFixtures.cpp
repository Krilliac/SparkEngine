// TestFixtures.cpp - Tests for the TEST_F fixture macro
// Validates SetUp/TearDown behavior

#include "TestFramework.h"
#include <string>
#include <vector>

// A simple fixture that tracks setup/teardown calls
struct CounterFixture
{
    static inline std::vector<std::string> callLog;

    void SetUp() { callLog.push_back("SetUp"); }
    void TearDown() { callLog.push_back("TearDown"); }
};

TEST_F(CounterFixture, SetUpCalled)
{
    // If we got here, SetUp was already called
    EXPECT_TRUE(callLog.size() >= 1u);
    // The last entry before Run should be "SetUp"
    bool foundSetUp = false;
    for (const auto& entry : callLog)
    {
        if (entry == "SetUp")
            foundSetUp = true;
    }
    EXPECT_TRUE(foundSetUp);
}

// A fixture with actual state
struct VectorFixture
{
    std::vector<int> data;

    void SetUp() { data = {1, 2, 3, 4, 5}; }

    void TearDown() { data.clear(); }
};

TEST_F(VectorFixture, DataIsInitialized)
{
    EXPECT_EQ(data.size(), 5u);
    EXPECT_EQ(data[0], 1);
    EXPECT_EQ(data[4], 5);
}

TEST_F(VectorFixture, DataIsIndependent)
{
    // Each test gets a fresh fixture, so data should be the original
    EXPECT_EQ(data.size(), 5u);
    data.push_back(6);
    EXPECT_EQ(data.size(), 6u);
    // Next test should not see this modification
}

TEST_F(VectorFixture, DataIsStillFresh)
{
    // Confirm independent from the previous test
    EXPECT_EQ(data.size(), 5u);
}
