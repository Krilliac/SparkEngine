// TestSerializationHardening.cpp - SEC2 scene-serialization lane regressions.
//
// Each test pins one hardening fix at a document trust boundary:
//  - the vendored nlohmann::json stub bounds nesting depth, so a compact
//    deeply nested scene is a catchable parse error, not a stack overflow;
//  - reflected-scene persistence refuses an oversized file before reading it;
//  - a current-version scene with a damaged field is rejected (and LoadWorld
//    recovers the .bak) instead of loading defaults the next save would keep;
//  - Json::Value::AsInt never performs an out-of-range double->int conversion,
//    and mod manifests/configs validate loadOrder with TryAsInt;
//  - Json::ParseStrict rejects undefined escapes and raw control bytes;
//  - the stub parser bounds breadth (value count) as well as depth;
//  - SaveWorld refuses a world the reader would reject (NaN/Inf field, over the
//    byte cap) instead of writing a primary that LoadWorld rolls back to .bak,
//    while in-process undo/PIE snapshots still restore.

#include "TestFramework.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Components/CoreComponents.h"
#include "Engine/Modding/ModSystem.h"
#include "SceneManager/ReflectedSceneSerializer.h"
#include "Utils/JsonUtils.h"

#include <nlohmann_json.h>

#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>

using namespace Spark;

namespace
{
    std::string PathToUtf8(const std::filesystem::path& path)
    {
        const std::u8string utf8 = path.u8string();
        return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
    }

    /// A unique scratch directory under the temp directory, removed on scope exit.
    class ScratchDirectory
    {
      public:
        explicit ScratchDirectory(const char* tag)
        {
            const auto nonce = std::chrono::high_resolution_clock::now().time_since_epoch().count();
            m_path = std::filesystem::temp_directory_path() /
                     ("spark_serialization_hardening_" + std::string(tag) + "_" + std::to_string(nonce));
            std::filesystem::create_directories(m_path);
        }

        ~ScratchDirectory()
        {
            std::error_code ignored;
            std::filesystem::remove_all(m_path, ignored);
        }

        ScratchDirectory(const ScratchDirectory&) = delete;
        ScratchDirectory& operator=(const ScratchDirectory&) = delete;

        const std::filesystem::path& Path() const { return m_path; }

      private:
        std::filesystem::path m_path;
    };

    void WriteText(const std::filesystem::path& path, const std::string& text)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
    }

    std::string NestedArrays(size_t depth)
    {
        return std::string(depth, '[') + std::string(depth, ']');
    }

    std::string NestedObjects(size_t depth)
    {
        std::string text;
        text.reserve(depth * 6 + 1);
        for (size_t i = 0; i < depth; ++i)
            text += "{\"a\":";
        text += '1';
        text.append(depth, '}');
        return text;
    }

    std::string FirstEntityName(World& world)
    {
        const auto names = world.GetEntitiesWith<NameComponent>();
        if (names.begin() == names.end())
            return {};
        return names.get<NameComponent>(*names.begin()).name;
    }
} // namespace

// Finding 66: the recursive stub parser had no depth budget. 100k '[' overflowed
// the native stack, which the scene loader's C++ catch cannot intercept.
TEST(SerializationHardening_JsonStubRejectsDeepNestingAsParseError)
{
    constexpr size_t kLimit = nlohmann::json::max_parse_depth;

    // The limit itself still parses; one level deeper is a parse error.
    EXPECT_NO_THROW((void)nlohmann::json::parse(NestedArrays(kLimit)));
    EXPECT_NO_THROW((void)nlohmann::json::parse(NestedObjects(kLimit)));
    EXPECT_THROW((void)nlohmann::json::parse(NestedArrays(kLimit + 1)), std::runtime_error);
    EXPECT_THROW((void)nlohmann::json::parse(NestedObjects(kLimit + 1)), std::runtime_error);

    // A compact hostile document is rejected, not a crash.
    EXPECT_THROW((void)nlohmann::json::parse(NestedArrays(100000)), std::runtime_error);
    EXPECT_THROW((void)nlohmann::json::parse(NestedObjects(100000)), std::runtime_error);

    // The reflected-scene loader surfaces it as an ordinary rejection.
    const std::string hostileScene = "{\"version\":1,\"entities\":[" + NestedArrays(100000) + "]}";
    World world;
    world.CreateEntity("KeepMe");
    std::string error;
    EXPECT_FALSE(DeserializeInto(world, hostileScene, SceneDeserializeMode::Permissive, &error));
    EXPECT_STR_CONTAINS(error, "nesting too deep");
    EXPECT_EQ(world.GetEntityCount(), static_cast<size_t>(1));
}

// Finding 67: LoadWorld read the primary (and then the .bak) whole with no byte
// limit, so an oversized file was allocated in full before any parse.
TEST(SerializationHardening_LoadWorldRejectsOversizedSceneBeforeReading)
{
    ScratchDirectory scratch("oversize");
    const std::filesystem::path scene = scratch.Path() / "Huge.sparkscene";
    std::filesystem::path backup = scene;
    backup += ".bak";

    // A sparse file one byte over the limit: the size check must reject it
    // without reading 64 MiB of zeros into memory.
    WriteText(scene, "{");
    std::filesystem::resize_file(scene, kMaxSceneDocumentBytes + 1);

    World world;
    world.CreateEntity("Unchanged");
    std::string error;
    EXPECT_FALSE(LoadWorld(world, PathToUtf8(scene), &error));
    EXPECT_STR_CONTAINS(error, "scene size limit");
    EXPECT_STR_CONTAINS(error, ".bak' was not usable: file does not exist");
    EXPECT_EQ(world.GetEntityCount(), static_cast<size_t>(1));

    // An oversized primary falls back to a valid previous-good backup.
    WriteText(backup, R"json({"version":1,"entities":[{"id":0,"name":"FromBackup"}]})json");
    World recovered;
    ASSERT_TRUE(LoadWorld(recovered, PathToUtf8(scene), &error));
    EXPECT_EQ(FirstEntityName(recovered), std::string("FromBackup"));

    // In-memory callers (editor recovery, undo snapshots) get the same bound,
    // checked before the text is parsed into a JSON tree.
    const std::string oversized(static_cast<size_t>(kMaxSceneDocumentBytes) + 1u, ' ');
    World inMemory;
    EXPECT_FALSE(DeserializeInto(inMemory, oversized, SceneDeserializeMode::Permissive, &error));
    EXPECT_STR_CONTAINS(error, "bytes; the limit is");
}

// Finding 72: a current-version scene with a wrong-typed or unparsable field
// loaded "successfully" with the component default, so LoadWorld never tried
// the .bak and the next save made the loss permanent.
TEST(SerializationHardening_CurrentSceneRejectsDamagedFieldsAndRecoversBackup)
{
    const char* numericField = R"json({"version":1,"entities":[{"id":1,"name":"Cam","parent":-1,
        "components":[{"type":"Camera","fields":{"fov":90}}]}]})json";
    const char* unparsableField = R"json({"version":1,"entities":[{"id":1,"name":"Cam","parent":-1,
        "components":[{"type":"Camera","fields":{"fov":"bad"}}]}]})json";

    std::string error;
    World numericWorld;
    EXPECT_FALSE(DeserializeInto(numericWorld, numericField, SceneDeserializeMode::Permissive, &error));
    EXPECT_STR_CONTAINS(error, "field 'Camera.fov' must be a string, found number");

    World unparsableWorld;
    EXPECT_FALSE(DeserializeInto(unparsableWorld, unparsableField, SceneDeserializeMode::Permissive, &error));
    EXPECT_STR_CONTAINS(error, "field 'Camera.fov' value \"bad\" could not be applied");

    // Schema drift stays tolerated in Permissive mode: a missing field keeps its
    // default and a component type this build does not register is skipped.
    const char* drift = R"json({"version":1,"entities":[{"id":1,"name":"Cam","parent":-1,
        "components":[{"type":"Camera","fields":{"fov":"60.000000"}},
                      {"type":"NoSuchComponent","fields":{"x":"1"}}]}]})json";
    World driftWorld;
    ASSERT_TRUE(DeserializeInto(driftWorld, drift, SceneDeserializeMode::Permissive, &error));
    const auto cameras = driftWorld.GetEntitiesWith<Camera>();
    ASSERT_TRUE(cameras.begin() != cameras.end());
    EXPECT_NEAR(cameras.get<Camera>(*cameras.begin()).fov, 60.0f, 0.001f);

    // LoadWorld recovers the previous-good image instead of installing defaults.
    ScratchDirectory scratch("damagedfield");
    const std::filesystem::path scene = scratch.Path() / "Level.sparkscene";
    World first;
    first.CreateEntity("First");
    ASSERT_TRUE(SaveWorld(first, PathToUtf8(scene)));
    World second;
    second.CreateEntity("Second");
    ASSERT_TRUE(SaveWorld(second, PathToUtf8(scene)));
    WriteText(scene, numericField);

    World recovered;
    ASSERT_TRUE(LoadWorld(recovered, PathToUtf8(scene), &error));
    EXPECT_EQ(recovered.GetEntityCount(), static_cast<size_t>(1));
    EXPECT_EQ(FirstEntityName(recovered), std::string("First"));
}

// Finding 71: AsInt did static_cast<int> on any finite double, which is
// undefined behaviour outside int (x86 yields INT_MIN).
TEST(SerializationHardening_JsonAsIntNeverConvertsOutOfRangeNumbers)
{
    const auto parse = [](const char* text)
    {
        Json::Value value;
        std::string error;
        const bool ok = Json::ParseStrict(text, &value, &error);
        EXPECT_TRUE(ok);
        return value;
    };

    EXPECT_EQ(parse("2147483648").AsInt(7), 7);
    EXPECT_EQ(parse("-2147483649").AsInt(7), 7);
    EXPECT_EQ(parse("1e300").AsInt(7), 7);
    EXPECT_EQ(parse("-1e300").AsInt(7), 7);
    EXPECT_EQ(parse("2147483647").AsInt(7), INT_MAX);
    EXPECT_EQ(parse("-2147483648").AsInt(7), INT_MIN);
    // In-range fractions keep their historical truncation toward zero.
    EXPECT_EQ(parse("2147483647.5").AsInt(7), INT_MAX);
    EXPECT_EQ(parse("-2147483648.5").AsInt(7), INT_MIN);
    EXPECT_EQ(parse("1.5").AsInt(7), 1);

    EXPECT_TRUE(parse("42").TryAsInt() == std::optional<int>(42));
    EXPECT_TRUE(parse("-2147483648").TryAsInt() == std::optional<int>(INT_MIN));
    EXPECT_TRUE(parse("2147483647").TryAsInt() == std::optional<int>(INT_MAX));
    EXPECT_FALSE(parse("2147483648").TryAsInt().has_value());
    EXPECT_FALSE(parse("-2147483649").TryAsInt().has_value());
    EXPECT_FALSE(parse("1.5").TryAsInt().has_value());
    EXPECT_FALSE(parse("\"5\"").TryAsInt().has_value());
}

// Finding 71 (consumer): mod loadOrder went through the unchecked AsInt.
TEST(SerializationHardening_ModLoadOrderMustBeAnExactInt)
{
    ScratchDirectory scratch("modorder");
    const std::filesystem::path mods = scratch.Path() / "mods";
    WriteText(mods / "Good" / "mod.json", R"({"id":"Good","name":"Good","version":"1.0","loadOrder":5})");
    WriteText(mods / "Huge" / "mod.json", R"({"id":"Huge","name":"Huge","version":"1.0","loadOrder":2147483648})");
    WriteText(mods / "Frac" / "mod.json", R"({"id":"Frac","name":"Frac","version":"1.0","loadOrder":1.5})");

    ModSystem system;
    EXPECT_EQ(system.ScanForMods(PathToUtf8(mods)), static_cast<size_t>(1));
    const ModInfo* good = system.GetModInfo("Good");
    ASSERT_TRUE(good != nullptr);
    EXPECT_EQ(good->loadOrder, 5);
    EXPECT_TRUE(system.GetModInfo("Huge") == nullptr);
    EXPECT_TRUE(system.GetModInfo("Frac") == nullptr);

    // The mod-state config falls back to the default priority.
    const std::filesystem::path config = scratch.Path() / "mods.json";
    WriteText(config, R"({"mods":[{"id":"Good","enabled":true,"loadOrder":1e300}]})");
    ASSERT_TRUE(system.LoadConfig(PathToUtf8(config)));
    good = system.GetModInfo("Good");
    ASSERT_TRUE(good != nullptr);
    EXPECT_EQ(good->loadOrder, 0);
    EXPECT_TRUE(good->enabled);
}

// Finding 73: ParseStrict accepted "\q" (dropping the backslash) and raw bytes
// below 0x20 inside strings, although it promises to reject malformed JSON.
TEST(SerializationHardening_JsonStrictRejectsUndefinedEscapesAndControlBytes)
{
    Json::Value value;
    std::string error;

    EXPECT_FALSE(Json::ParseStrict(R"({"path":"a\q"})", &value, &error));
    EXPECT_STR_CONTAINS(error, "invalid escape character");

    EXPECT_FALSE(Json::ParseStrict("{\"a\":\"x\x01y\"}", &value, &error));
    EXPECT_STR_CONTAINS(error, "unescaped control character");

    EXPECT_FALSE(Json::ParseStrict("{\"a\":\"line\nbreak\"}", &value, &error));
    EXPECT_STR_CONTAINS(error, "unescaped control character");

    const std::string rawNul("{\"a\":\"x\0y\"}", 11);
    EXPECT_FALSE(Json::ParseStrict(rawNul, &value, &error));
    EXPECT_STR_CONTAINS(error, "unescaped control character");

    // The escaped forms are valid JSON and still parse.
    ASSERT_TRUE(Json::ParseStrict(R"({"a":"x\u0000y","b":"\t\n\/"})", &value, &error));
    EXPECT_EQ(value[std::string("a")].AsString(), std::string("x\0y", 3));
    EXPECT_EQ(value[std::string("b")].AsString(), std::string("\t\n/"));

    // Bounded parsing shares the rule.
    EXPECT_FALSE(Json::ParseBounded(R"(["\q"])", Json::JsonLimits{}, &value, &error));
}

// Finding 66 (breadth): the byte cap alone admits ~33M values ("[0,0,..." is two
// bytes each), and every value is a full JSON node, so a file under the cap could
// expand to gigabytes. parse() now refuses a document over its value budget with
// an allocation-free pre-scan.
TEST(SerializationHardening_JsonStubBoundsValueCountBeforeAllocating)
{
    using nlohmann::json;
    EXPECT_EQ(json::count_values_upper_bound("[0,0,0]"), static_cast<size_t>(4));
    // Brackets and commas inside strings (including after an escaped quote) do not count.
    EXPECT_EQ(json::count_values_upper_bound(R"(["a,[{b\",c"])"), static_cast<size_t>(2));

    EXPECT_NO_THROW((void)json::parse("[0,0,0]", 4));
    EXPECT_THROW((void)json::parse("[0,0,0]", 3), std::runtime_error);

    // A wide flat array one value over the default budget is rejected by the scan.
    std::string wide;
    wide.reserve(json::max_parse_values * 2 + 2);
    wide += '[';
    for (size_t i = 0; i < json::max_parse_values; ++i)
        wide += "0,";
    wide.back() = ']';
    EXPECT_EQ(json::count_values_upper_bound(wide), json::max_parse_values + 1);
    std::string parseError;
    try
    {
        (void)json::parse(wide);
    }
    catch (const std::runtime_error& ex)
    {
        parseError = ex.what();
    }
    EXPECT_STR_CONTAINS(parseError, "too many values");

    // The scene loader applies its own budget to a document well under the byte cap.
    std::string hostileScene = "{\"version\":1,\"entities\":[";
    hostileScene.reserve(static_cast<size_t>(kMaxSceneDocumentValues) * 2 + 64);
    for (uint64_t i = 0; i < kMaxSceneDocumentValues; ++i)
        hostileScene += "0,";
    hostileScene += "0]}";
    ASSERT_TRUE(static_cast<uint64_t>(hostileScene.size()) < kMaxSceneDocumentBytes);
    World world;
    world.CreateEntity("KeepMe");
    std::string error;
    EXPECT_FALSE(DeserializeInto(world, hostileScene, SceneDeserializeMode::Permissive, &error));
    EXPECT_STR_CONTAINS(error, "too many values");
    EXPECT_EQ(world.GetEntityCount(), static_cast<size_t>(1));
}

// Finding 72 (writer side): GetFieldAsString prints NaN/Inf as "nan"/"inf", which
// the reader refuses. SaveWorld used to write such a world, and the next LoadWorld
// rejected it and silently recovered the older .bak. SaveWorld now fails closed;
// in-process undo/PIE snapshots of the same world still restore.
TEST(SerializationHardening_SaveWorldRefusesNonFiniteFields)
{
    ScratchDirectory scratch("nonfinite");
    const std::filesystem::path scene = scratch.Path() / "Level.sparkscene";

    World world;
    const EntityID mover = world.CreateEntity("Mover");
    Transform& transform = world.AddComponent<Transform>(mover);
    transform.position = DirectX::XMFLOAT3(1.0f, 2.0f, 3.0f);
    transform.rotation = DirectX::XMFLOAT3(0.0f, 45.0f, 0.0f);
    std::string error;
    ASSERT_TRUE(SaveWorld(world, PathToUtf8(scene), &error));

    // Physics blew up: the position is now NaN.
    world.GetComponent<Transform>(mover)->position.x = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(SaveWorld(world, PathToUtf8(scene), &error));
    EXPECT_STR_CONTAINS(error, "'Transform.position'");
    EXPECT_STR_CONTAINS(error, "finite");

    // The previous save is untouched and loads as the primary (no .bak exists).
    std::filesystem::path backup = scene;
    backup += ".bak";
    EXPECT_FALSE(std::filesystem::exists(backup));
    World reopened;
    ASSERT_TRUE(LoadWorld(reopened, PathToUtf8(scene), &error));
    EXPECT_EQ(FirstEntityName(reopened), std::string("Mover"));
    const auto transforms = reopened.GetEntitiesWith<Transform>();
    ASSERT_TRUE(transforms.begin() != transforms.end());
    EXPECT_NEAR(transforms.get<Transform>(*transforms.begin()).position.x, 1.0f, 0.001f);

    // Infinity in a scalar Float field is refused the same way.
    World infinite;
    const EntityID camera = infinite.CreateEntity("Cam");
    infinite.AddComponent<Camera>(camera).fov = std::numeric_limits<float>::infinity();
    std::string text;
    EXPECT_FALSE(TrySerializeWorld(infinite, text, &error));
    EXPECT_STR_CONTAINS(error, "'Camera.fov'");
    EXPECT_TRUE(text.empty());

    // Undo/redo and play-in-editor restore their own in-process snapshot: the
    // unreadable field keeps its default instead of failing the whole restore,
    // and every other field is restored.
    const std::string snapshot = SerializeWorld(world);
    World restored;
    ASSERT_TRUE(DeserializeInto(restored, snapshot, SceneDeserializeMode::TrustedSnapshot, &error));
    const auto restoredTransforms = restored.GetEntitiesWith<Transform>();
    ASSERT_TRUE(restoredTransforms.begin() != restoredTransforms.end());
    const Transform& restoredTransform = restoredTransforms.get<Transform>(*restoredTransforms.begin());
    EXPECT_TRUE(std::isfinite(restoredTransform.position.x));
    EXPECT_NEAR(restoredTransform.rotation.y, 45.0f, 0.001f);
    // The same text read as a file is still damage.
    World fromDisk;
    EXPECT_FALSE(DeserializeInto(fromDisk, snapshot, SceneDeserializeMode::Permissive, &error));
}

// Finding 67 (writer side): LoadWorld caps a scene at kMaxSceneDocumentBytes but
// SaveWorld wrote any size, so an over-cap world saved "successfully" and the next
// load silently rolled back to the .bak. SaveWorld now refuses it; in-process
// snapshots of the same world are exempt from the cap.
TEST(SerializationHardening_SaveWorldRefusesOverCapDocument)
{
    ScratchDirectory scratch("overcapsave");
    const std::filesystem::path scene = scratch.Path() / "Big.sparkscene";

    World world;
    world.CreateEntity("Small");
    std::string error;
    ASSERT_TRUE(SaveWorld(world, PathToUtf8(scene), &error));

    // One entity whose name alone exceeds the byte cap.
    const size_t hugeNameBytes = static_cast<size_t>(kMaxSceneDocumentBytes) + 1u;
    World huge;
    huge.CreateEntity(std::string(hugeNameBytes, 'n'));
    EXPECT_FALSE(SaveWorld(huge, PathToUtf8(scene), &error));
    EXPECT_STR_CONTAINS(error, "bytes; the limit is");

    // The earlier image is still the primary; nothing rolled back to a .bak.
    std::filesystem::path backup = scene;
    backup += ".bak";
    EXPECT_FALSE(std::filesystem::exists(backup));
    World reopened;
    ASSERT_TRUE(LoadWorld(reopened, PathToUtf8(scene), &error));
    EXPECT_EQ(FirstEntityName(reopened), std::string("Small"));

    // An undo or PIE snapshot of the big world still restores in-process.
    const std::string snapshot = SerializeWorld(huge);
    World restored;
    ASSERT_TRUE(DeserializeInto(restored, snapshot, SceneDeserializeMode::TrustedSnapshot, &error));
    EXPECT_EQ(FirstEntityName(restored).size(), hugeNameBytes);
}
