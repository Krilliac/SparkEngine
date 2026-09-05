/**
 * @file TestJsonUtils.cpp
 * @brief Parser/serializer contract tests against the SHIPPED Spark::Json.
 *
 * This file used to carry a "simplified JSON implementation for testing (mirrors
 * engine implementation)" and included no engine header at all, so every
 * assertion below described a copy rather than the code that ships. A test only
 * counts if it executes production source, so the mock is gone and these
 * assertions now run Utils/JsonUtils.h.
 *
 * Parsing is asserted through ParseStrict because it always uses the built-in
 * recursive-descent parser, making acceptance deterministic across build
 * configurations; the lenient Parse entry point is backend-dependent and is
 * covered by TestJsonStrict.cpp.
 */

#include "TestFramework.h"

#include "Utils/JsonUtils.h"

#include <string>

using Spark::Json::ParseStrict;
using Spark::Json::Value;

namespace
{
    /// Parse @p text with the deterministic strict parser, asserting it is accepted.
    Value ParseOk(const std::string& text)
    {
        Value value;
        std::string error;
        const bool ok = ParseStrict(text, &value, &error);
        EXPECT_TRUE(ok);
        return value;
    }
} // namespace

// ===========================================================================
// Tests
// ===========================================================================

TEST(Json_ParseNull)
{
    auto v = ParseOk("null");
    EXPECT_TRUE(v.IsNull());
}

TEST(Json_ParseTrue)
{
    auto v = ParseOk("true");
    EXPECT_TRUE(v.IsBool());
    EXPECT_TRUE(v.AsBool());
}

TEST(Json_ParseFalse)
{
    auto v = ParseOk("false");
    EXPECT_TRUE(v.IsBool());
    EXPECT_FALSE(v.AsBool());
}

TEST(Json_ParseInteger)
{
    auto v = ParseOk("42");
    EXPECT_TRUE(v.IsNumber());
    EXPECT_NEAR(v.AsNumber(), 42.0, 0.001);
}

TEST(Json_ParseNegative)
{
    auto v = ParseOk("-17");
    EXPECT_TRUE(v.IsNumber());
    EXPECT_NEAR(v.AsNumber(), -17.0, 0.001);
}

TEST(Json_ParseFloat)
{
    auto v = ParseOk("3.14");
    EXPECT_TRUE(v.IsNumber());
    EXPECT_NEAR(v.AsNumber(), 3.14, 0.001);
}

TEST(Json_ParseScientific)
{
    auto v = ParseOk("1.5e3");
    EXPECT_TRUE(v.IsNumber());
    EXPECT_NEAR(v.AsNumber(), 1500.0, 0.001);
}

TEST(Json_ParseString)
{
    auto v = ParseOk("\"hello world\"");
    EXPECT_TRUE(v.IsString());
    EXPECT_EQ(v.AsString(), std::string("hello world"));
}

TEST(Json_ParseStringEscapes)
{
    auto v = ParseOk("\"line1\\nline2\\ttab\"");
    EXPECT_TRUE(v.IsString());
    EXPECT_EQ(v.AsString(), std::string("line1\nline2\ttab"));
}

TEST(Json_ParseEmptyArray)
{
    auto v = ParseOk("[]");
    EXPECT_TRUE(v.IsArray());
    EXPECT_EQ(v.Size(), 0u);
}

TEST(Json_ParseArray)
{
    auto v = ParseOk("[1, 2, 3]");
    EXPECT_TRUE(v.IsArray());
    EXPECT_EQ(v.Size(), 3u);
    EXPECT_NEAR(v[0].AsNumber(), 1.0, 0.001);
    EXPECT_NEAR(v[1].AsNumber(), 2.0, 0.001);
    EXPECT_NEAR(v[2].AsNumber(), 3.0, 0.001);
}

TEST(Json_ParseNestedArray)
{
    auto v = ParseOk("[[1, 2], [3]]");
    EXPECT_TRUE(v.IsArray());
    EXPECT_EQ(v.Size(), 2u);
    EXPECT_EQ(v[0].Size(), 2u);
    EXPECT_EQ(v[1].Size(), 1u);
}

TEST(Json_ParseEmptyObject)
{
    auto v = ParseOk("{}");
    EXPECT_TRUE(v.IsObject());
    EXPECT_EQ(v.Size(), 0u);
}

TEST(Json_ParseObject)
{
    auto v = ParseOk("{\"name\": \"Fireball\", \"damage\": 50}");
    EXPECT_TRUE(v.IsObject());
    EXPECT_EQ(v[std::string("name")].AsString(), std::string("Fireball"));
    EXPECT_NEAR(v[std::string("damage")].AsNumber(), 50.0, 0.001);
}

TEST(Json_ParseNestedObject)
{
    auto v = ParseOk("{\"a\": {\"b\": 42}}");
    EXPECT_TRUE(v.IsObject());
    EXPECT_TRUE(v[std::string("a")].IsObject());
    EXPECT_NEAR(v[std::string("a")][std::string("b")].AsNumber(), 42.0, 0.001);
}

TEST(Json_ParseWithWhitespace)
{
    auto v = ParseOk("  { \"key\" :  \"value\"  } ");
    EXPECT_TRUE(v.IsObject());
    EXPECT_EQ(v[std::string("key")].AsString(), std::string("value"));
}

TEST(Json_ParseMalformedRejected)
{
    Value value;
    std::string error;
    EXPECT_FALSE(ParseStrict("{invalid json}", &value, &error));
    EXPECT_TRUE(value.IsNull());
    EXPECT_FALSE(error.empty());
}

TEST(Json_ParseEmptyRejected)
{
    Value value;
    std::string error;
    EXPECT_FALSE(ParseStrict("", &value, &error));
    EXPECT_TRUE(value.IsNull());
    EXPECT_FALSE(error.empty());
}

TEST(Json_HasKey)
{
    auto v = ParseOk("{\"x\": 1}");
    EXPECT_TRUE(v.HasKey("x"));
    EXPECT_FALSE(v.HasKey("y"));
}

TEST(Json_AsInt)
{
    auto v = ParseOk("42");
    EXPECT_EQ(v.AsInt(), 42);
}

TEST(Json_BuildObject)
{
    Value obj;
    obj[std::string("level")] = Value(5);
    obj[std::string("name")] = Value("test");
    EXPECT_TRUE(obj.IsObject());
    EXPECT_EQ(obj[std::string("level")].AsInt(), 5);
    EXPECT_EQ(obj[std::string("name")].AsString(), std::string("test"));
}

TEST(Json_BuildArray)
{
    auto arr = Value::MakeArray();
    arr.PushBack(Value(1));
    arr.PushBack(Value(2));
    arr.PushBack(Value("three"));
    EXPECT_EQ(arr.Size(), 3u);
    EXPECT_EQ(arr[2].AsString(), std::string("three"));
}

TEST(Json_MixedArray)
{
    auto v = ParseOk("[1, \"two\", true, null]");
    EXPECT_EQ(v.Size(), 4u);
    EXPECT_TRUE(v[0].IsNumber());
    EXPECT_TRUE(v[1].IsString());
    EXPECT_TRUE(v[2].IsBool());
    EXPECT_TRUE(v[3].IsNull());
}

TEST(Json_RoundTripThroughStringify)
{
    auto v = ParseOk("{\"a\":[1,2],\"b\":\"x\"}");
    const std::string text = Spark::Json::Stringify(v);

    Value reparsed;
    std::string error;
    EXPECT_TRUE(ParseStrict(text, &reparsed, &error));
    EXPECT_EQ(reparsed[std::string("a")].Size(), 2u);
    EXPECT_EQ(reparsed[std::string("b")].AsString(), std::string("x"));
}
