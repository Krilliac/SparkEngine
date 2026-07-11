/**
 * @file TestJsonStrict.cpp
 * @brief Spark::Json::ParseStrict (W9): strict acceptance/rejection contract vs
 *        the deliberately lenient Spark::Json::Parse.
 *
 * Background (W8 finding): Parse gives callers no reliable corruption signal —
 * depending on the active backend it returns Null OR a PARTIAL value for
 * malformed input (the vendored nlohmann stub returns '{ this is garbage' as a
 * valid-looking object). Persistence stores that quarantine-and-refuse on
 * corrupt files (TFDatabase, TFOutfitStore, TFSocialSystem's store) therefore
 * silently accepted truncated files and OVERWROTE them on the next save.
 * ParseStrict always uses the built-in recursive-descent parser, so its
 * verdicts below are deterministic across build configurations; the lenient
 * assertions are intentionally weak wherever Parse's result is
 * backend-dependent.
 */
#include "TestFramework.h"

#include "Utils/JsonUtils.h"

#include <string>

using Spark::Json::Parse;
using Spark::Json::ParseStrict;
using Spark::Json::Value;

namespace
{
    /// Assert the strict-rejection contract: false, Null out-value, and an
    /// error that carries a byte offset.
    void ExpectStrictReject(const std::string& input)
    {
        Value v;
        std::string err;
        EXPECT_FALSE(ParseStrict(input, &v, &err));
        EXPECT_TRUE(v.IsNull());
        EXPECT_STR_CONTAINS(err, "offset");
    }
} // namespace

// ---------------------------------------------------------------------------
// Valid input: ParseStrict and Parse must agree
// ---------------------------------------------------------------------------

TEST(JsonStrict_ValidObjectBothPaths)
{
    const std::string input = R"({"name": "Fireball", "damage": 50})";

    Value v;
    std::string err;
    EXPECT_TRUE(ParseStrict(input, &v, &err));
    EXPECT_TRUE(err.empty());
    EXPECT_TRUE(v.IsObject());
    EXPECT_TRUE(v["name"].AsString() == "Fireball");
    EXPECT_EQ(v["damage"].AsInt(), 50);

    const Value p = Parse(input);
    EXPECT_TRUE(p.IsObject());
    EXPECT_TRUE(p["name"].AsString() == "Fireball");
    EXPECT_EQ(p["damage"].AsInt(), 50);
}

TEST(JsonStrict_ValidNestedArrayBothPaths)
{
    const std::string input = R"([1, [2, 3], {"k": true}, null])";

    Value v;
    EXPECT_TRUE(ParseStrict(input, &v));
    EXPECT_TRUE(v.IsArray());
    EXPECT_EQ(v.Size(), size_t{4});
    EXPECT_EQ(v[0].AsInt(), 1);
    EXPECT_EQ(v[1][1].AsInt(), 3);
    EXPECT_TRUE(v[2]["k"].AsBool());
    EXPECT_TRUE(v[3].IsNull());

    const Value p = Parse(input);
    EXPECT_TRUE(p.IsArray());
    EXPECT_EQ(p.Size(), size_t{4});
}

TEST(JsonStrict_ValidScalarsAndEscapes)
{
    Value v;
    EXPECT_TRUE(ParseStrict("null", &v));
    EXPECT_TRUE(v.IsNull());

    EXPECT_TRUE(ParseStrict("true", &v));
    EXPECT_TRUE(v.AsBool());
    EXPECT_TRUE(ParseStrict("false", &v));
    EXPECT_FALSE(v.AsBool());

    EXPECT_TRUE(ParseStrict("42", &v));
    EXPECT_EQ(v.AsInt(), 42);
    EXPECT_TRUE(ParseStrict("-17.5", &v));
    EXPECT_NEAR(v.AsNumber(), -17.5, 1e-9);

    EXPECT_TRUE(ParseStrict("\"hi\"", &v));
    EXPECT_TRUE(v.AsString() == "hi");
    EXPECT_TRUE(ParseStrict(R"("a\nb")", &v));
    EXPECT_TRUE(v.AsString() == "a\nb");
    EXPECT_TRUE(ParseStrict(R"("A")", &v));
    EXPECT_TRUE(v.AsString() == "A");
}

TEST(JsonStrict_SurroundingWhitespaceAccepted)
{
    Value v;
    EXPECT_TRUE(ParseStrict("  \n\t {\"a\": 1} \r\n ", &v));
    EXPECT_TRUE(v.IsObject());
    EXPECT_EQ(v["a"].AsInt(), 1);
}

TEST(JsonStrict_LargeFiniteNumberAccepted)
{
    Value v;
    EXPECT_TRUE(ParseStrict("1e300", &v));
    EXPECT_NEAR(v.AsNumber(), 1e300, 1e285);

    EXPECT_TRUE(ParseStrict("123456789012345", &v));
    EXPECT_NEAR(v.AsNumber(), 123456789012345.0, 0.5);
}

// ---------------------------------------------------------------------------
// Corruption shapes the persistence stores must refuse
// ---------------------------------------------------------------------------

TEST(JsonStrict_TrailingJunkRejected)
{
    // A torn write over a longer old file leaves a valid JSON prefix + junk
    // tail. Strict must reject; lenient gives NO reliable signal (object on
    // the fallback/stub backends, Null on real nlohmann).
    const std::string input = R"({"a": 1}garbage)";

    Value v;
    std::string err;
    EXPECT_FALSE(ParseStrict(input, &v, &err));
    EXPECT_TRUE(v.IsNull());
    EXPECT_STR_CONTAINS(err, "trailing");

    const Value p = Parse(input);
    EXPECT_TRUE(p.IsObject() || p.IsNull());
}

TEST(JsonStrict_DoubleRootRejected)
{
    ExpectStrictReject("{} {}");
    ExpectStrictReject("{\"a\": 1} null");
}

TEST(JsonStrict_TruncatedObjectRejected)
{
    // File cut mid-write (the TFOutfitStore data-loss shape from W8).
    const std::string input = R"({"outfits": [{"id": 1, "name": "x")";
    ExpectStrictReject(input);

    // Lenient: backend-dependent (partial object on the stub, Null otherwise).
    const Value p = Parse(input);
    EXPECT_TRUE(p.IsObject() || p.IsNull());
}

TEST(JsonStrict_TruncatedStringRejected)
{
    ExpectStrictReject("{\"a\": \"unterminated");
    ExpectStrictReject("\"unterminated");
}

TEST(JsonStrict_W8GarbageObjectPrefixRejected)
{
    // The W8 reproducer: the lenient stub backend returns a valid-looking
    // (empty) OBJECT for this, which sailed straight past IsObject() checks.
    const std::string input = "{ this is garbage";
    ExpectStrictReject(input);

    const Value p = Parse(input);
    EXPECT_TRUE(p.IsObject() || p.IsNull()); // no reliable corruption signal
}

TEST(JsonStrict_BinaryGarbageRejected)
{
    ExpectStrictReject(std::string("\x01\x02\xFF\xFE"
                                   "binary garbage, not JSON"));
    // Embedded NUL must not truncate the scan or crash.
    ExpectStrictReject(std::string("\x00\x00\x7F", 3));
}

TEST(JsonStrict_EmptyAndWhitespaceOnlyRejected)
{
    Value v;
    std::string err;
    EXPECT_FALSE(ParseStrict("", &v, &err));
    EXPECT_TRUE(v.IsNull());
    EXPECT_STR_CONTAINS(err, "empty");

    err.clear();
    EXPECT_FALSE(ParseStrict(" \n\t ", &v, &err));
    EXPECT_STR_CONTAINS(err, "empty");

    // Lenient Parse of empty input is Null on every backend.
    EXPECT_TRUE(Parse("").IsNull());
    EXPECT_TRUE(Parse(" \n\t ").IsNull());
}

TEST(JsonStrict_HugeNumberOverflowRejected)
{
    Value v;
    std::string err;
    EXPECT_FALSE(ParseStrict("1e999", &v, &err));
    EXPECT_TRUE(v.IsNull());
    EXPECT_STR_CONTAINS(err, "range");

    ExpectStrictReject(R"({"n": 1e999})");

    // Lenient Parse swallows the overflow as Null on every backend.
    EXPECT_TRUE(Parse("1e999").IsNull());
}

TEST(JsonStrict_MalformedNumbersRejected)
{
    ExpectStrictReject(R"({"a": 12.})"); // no digits after '.'
    ExpectStrictReject("[1e]");          // no digits in exponent
    ExpectStrictReject("-");             // sign without digits
    ExpectStrictReject("01x");           // leading zero leaves trailing junk
}

TEST(JsonStrict_MissingSeparatorsRejected)
{
    ExpectStrictReject(R"({"a" 1})");       // missing ':'
    ExpectStrictReject("[1 2]");            // missing ','
    ExpectStrictReject(R"({"a":1 "b":2})"); // missing ',' between members
}

TEST(JsonStrict_InvalidUnicodeEscapeRejected)
{
    ExpectStrictReject(R"("\uZZZZ")");
    // Lenient keeps the historical '?' replacement (fallback) or another
    // backend-specific string — but never an object.
    const Value p = Parse(R"("\uZZZZ")");
    EXPECT_TRUE(p.IsString() || p.IsNull());
}

TEST(JsonStrict_NonJsonPrefixRejected)
{
    ExpectStrictReject(R"(garbage{"a":1})");
    // A non-JSON first byte yields Null on every backend.
    EXPECT_TRUE(Parse(R"(garbage{"a":1})").IsNull());
}

// ---------------------------------------------------------------------------
// API surface details
// ---------------------------------------------------------------------------

TEST(JsonStrict_NullOutParamsTolerated)
{
    // Validation-only use: both out-params optional.
    EXPECT_TRUE(ParseStrict("{}", nullptr, nullptr));
    EXPECT_FALSE(ParseStrict("junk", nullptr)); // outError defaulted
}

TEST(JsonStrict_ErrorOffsetIsReported)
{
    Value v;
    std::string err;
    EXPECT_FALSE(ParseStrict("xyz", &v, &err));
    EXPECT_STR_CONTAINS(err, "(offset 0)");

    err.clear();
    EXPECT_FALSE(ParseStrict("[1, 2, @]", &v, &err));
    EXPECT_STR_CONTAINS(err, "(offset 7)"); // the '@'
}

TEST(JsonStrict_StrictValueRoundTrips)
{
    Value v;
    EXPECT_TRUE(ParseStrict(R"({"a": [1, 2], "s": "x"})", &v));

    const std::string text = Spark::Json::Stringify(v);
    Value v2;
    EXPECT_TRUE(ParseStrict(text, &v2));
    EXPECT_EQ(v2["a"][1].AsInt(), 2);
    EXPECT_TRUE(v2["s"].AsString() == "x");
}
