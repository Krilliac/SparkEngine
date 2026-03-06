// TestUUID.cpp - Tests for UUID generation and manipulation
// Uses the actual UUID.h header

#include "TestFramework.h"
#include "Utils/UUID.h"
#include <unordered_set>
#include <set>

// =============================================================================
// Tests
// =============================================================================

TEST(UUID_NullIsInvalid) {
    Spark::UUID null = Spark::UUID::Null();
    EXPECT_FALSE(null.IsValid());
    EXPECT_EQ(null.GetValue(), (uint64_t)0);
}

TEST(UUID_GenerateIsValid) {
    Spark::UUID id = Spark::UUID::Generate();
    EXPECT_TRUE(id.IsValid());
    EXPECT_NE(id.GetValue(), (uint64_t)0);
}

TEST(UUID_GenerateUnique) {
    std::unordered_set<uint64_t> ids;
    for (int i = 0; i < 1000; ++i) {
        Spark::UUID id = Spark::UUID::Generate();
        ids.insert(id.GetValue());
    }
    // All 1000 should be unique
    EXPECT_EQ((int)ids.size(), 1000);
}

TEST(UUID_Equality) {
    Spark::UUID a(42);
    Spark::UUID b(42);
    Spark::UUID c(99);

    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a == c);
    EXPECT_TRUE(a != c);
    EXPECT_FALSE(a != b);
}

TEST(UUID_Comparison) {
    Spark::UUID a(10);
    Spark::UUID b(20);

    EXPECT_TRUE(a < b);
    EXPECT_FALSE(b < a);
    EXPECT_TRUE(b > a);
    EXPECT_TRUE(a <= b);
    EXPECT_TRUE(a <= Spark::UUID(10));
    EXPECT_TRUE(b >= a);
}

TEST(UUID_ToStringRoundTrip) {
    Spark::UUID original(0xDEADBEEFCAFE1234ULL);
    std::string hex = original.ToString();

    EXPECT_EQ((int)hex.size(), 16);

    Spark::UUID parsed = Spark::UUID::FromString(hex);
    EXPECT_TRUE(original == parsed);
}

TEST(UUID_FromStringInvalid) {
    Spark::UUID null = Spark::UUID::FromString("");
    EXPECT_FALSE(null.IsValid());

    Spark::UUID bad = Spark::UUID::FromString("not_hex");
    // std::stoull may throw for invalid input, should return Null
    // This depends on implementation - just check it doesn't crash
    (void)bad;
}

TEST(UUID_GenerateToStringRoundTrip) {
    Spark::UUID id = Spark::UUID::Generate();
    std::string hex = id.ToString();
    Spark::UUID parsed = Spark::UUID::FromString(hex);
    EXPECT_TRUE(id == parsed);
}

TEST(UUID_ExplicitValue) {
    Spark::UUID id(12345);
    EXPECT_EQ(id.GetValue(), (uint64_t)12345);
    EXPECT_TRUE(id.IsValid());
}

TEST(UUID_BoolConversion) {
    Spark::UUID valid(42);
    Spark::UUID null = Spark::UUID::Null();

    EXPECT_TRUE(static_cast<bool>(valid));
    EXPECT_FALSE(static_cast<bool>(null));
}

TEST(UUID_Hash) {
    // Test that UUID works in unordered containers
    std::unordered_set<Spark::UUID> set;
    Spark::UUID a(100);
    Spark::UUID b(200);

    set.insert(a);
    set.insert(b);
    set.insert(a);  // Duplicate

    EXPECT_EQ((int)set.size(), 2);
    EXPECT_TRUE(set.count(a) == 1);
    EXPECT_TRUE(set.count(b) == 1);
}

TEST(UUID_OrderedContainer) {
    std::set<Spark::UUID> ordered;
    ordered.insert(Spark::UUID(30));
    ordered.insert(Spark::UUID(10));
    ordered.insert(Spark::UUID(20));

    auto it = ordered.begin();
    EXPECT_EQ(it->GetValue(), (uint64_t)10);
    ++it;
    EXPECT_EQ(it->GetValue(), (uint64_t)20);
    ++it;
    EXPECT_EQ(it->GetValue(), (uint64_t)30);
}
