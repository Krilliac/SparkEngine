/**
 * @file TestPrefabCompatibility.cpp
 * @brief SAVE-230: editor prefab (.sparkprefab) N-1..N compatibility from committed fixtures.
 *
 * PrefabAsset reads SPARKPREFAB 1 (N-1) and 2 (N) and writes 2 only. The version 1 fixture
 * must load with every declared value intact and never be rewritten by the load; its re-save
 * is version 2 and reloads bit-identically. Version 2 must carry what version 1 could not
 * (whitespace and line breaks in names and strings) and detect a cut inside the last value.
 * A version 3 fixture must fail closed. Fixtures: Tests/Fixtures/Compatibility/Prefab.
 */

#include "TestFramework.h"
#include "Prefabs/PrefabAsset.h"

#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <variant>

namespace
{
    namespace fs = std::filesystem;

    /// The narrow SPARK_TEST_SOURCE_DIR literal would be decoded with the ANSI code
    /// page on Windows; the wide one keeps a non-ASCII checkout path intact.
    fs::path PrefabFixture(const char* name)
    {
#if defined(_WIN32) && defined(SPARK_TEST_SOURCE_DIR_WIDE)
        const fs::path sourceDir(SPARK_TEST_SOURCE_DIR_WIDE);
#else
        const fs::path sourceDir(SPARK_TEST_SOURCE_DIR);
#endif
        return sourceDir / "Tests" / "Fixtures" / "Compatibility" / "Prefab" / name;
    }

    /// PrefabAsset takes UTF-8 paths; path::string() is the ANSI code page on Windows.
    std::string Utf8(const fs::path& path)
    {
        const std::u8string utf8 = path.u8string();
        return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
    }

    fs::path WithSuffix(fs::path path, const char* suffix)
    {
        path += suffix;
        return path;
    }

    std::string ReadBytes(const fs::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    void WriteBytes(const fs::path& path, const std::string& bytes)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << bytes;
    }

    class PrefabScratch
    {
      public:
        PrefabScratch()
        {
            static std::atomic<unsigned int> sequence{0};
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            m_path = fs::temp_directory_path() /
                     ("spark-prefab-compat-" + std::to_string(stamp) + "-" + std::to_string(sequence++));
            fs::create_directories(m_path);
        }
        ~PrefabScratch()
        {
            std::error_code ec;
            fs::remove_all(m_path, ec);
        }
        PrefabScratch(const PrefabScratch&) = delete;
        PrefabScratch& operator=(const PrefabScratch&) = delete;

        fs::path File(const char* name) const { return m_path / name; }

      private:
        fs::path m_path;
    };

    const SparkEditor::PrefabPropertyValue* FindProperty(const SparkEditor::PrefabAsset& prefab, const char* component,
                                                         const char* property)
    {
        const SparkEditor::SerializedComponent* found = prefab.GetComponent(component);
        if (!found)
            return nullptr;
        const auto it = found->properties.find(property);
        return it == found->properties.end() ? nullptr : &it->second;
    }

    bool SameFloat(float a, float b)
    {
        return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
    }

    /// Exact equality: floats and doubles must match bit for bit, not within a tolerance.
    bool SameValue(const SparkEditor::PrefabPropertyValue& a, const SparkEditor::PrefabPropertyValue& b)
    {
        if (a.index() != b.index())
            return false;
        if (const auto* x = std::get_if<float>(&a))
            return SameFloat(*x, std::get<float>(b));
        if (const auto* x = std::get_if<double>(&a))
            return std::bit_cast<std::uint64_t>(*x) == std::bit_cast<std::uint64_t>(std::get<double>(b));
        if (const auto* x = std::get_if<XMFLOAT3>(&a))
        {
            const XMFLOAT3& y = std::get<XMFLOAT3>(b);
            return SameFloat(x->x, y.x) && SameFloat(x->y, y.y) && SameFloat(x->z, y.z);
        }
        if (const auto* x = std::get_if<XMFLOAT4>(&a))
        {
            const XMFLOAT4& y = std::get<XMFLOAT4>(b);
            return SameFloat(x->x, y.x) && SameFloat(x->y, y.y) && SameFloat(x->z, y.z) && SameFloat(x->w, y.w);
        }
        if (const auto* x = std::get_if<std::string>(&a))
            return *x == std::get<std::string>(b);
        if (const auto* x = std::get_if<int>(&a))
            return *x == std::get<int>(b);
        return std::get<bool>(a) == std::get<bool>(b);
    }

    bool SamePrefab(const SparkEditor::PrefabAsset& a, const SparkEditor::PrefabAsset& b)
    {
        if (a.GetName() != b.GetName() || a.GetComponents().size() != b.GetComponents().size())
            return false;
        for (size_t i = 0; i < a.GetComponents().size(); ++i)
        {
            const SparkEditor::SerializedComponent& left = a.GetComponents()[i];
            const SparkEditor::SerializedComponent& right = b.GetComponents()[i];
            if (left.typeName != right.typeName || left.properties.size() != right.properties.size())
                return false;
            for (const auto& [name, value] : left.properties)
            {
                const auto it = right.properties.find(name);
                if (it == right.properties.end() || !SameValue(value, it->second))
                    return false;
            }
        }
        return true;
    }

    bool Holds(const SparkEditor::PrefabPropertyValue* actual, const SparkEditor::PrefabPropertyValue& expected)
    {
        return actual && SameValue(*actual, expected);
    }

    /// A prefab the loader must not overwrite on failure.
    SparkEditor::PrefabAsset Sentinel()
    {
        SparkEditor::PrefabAsset sentinel("Sentinel");
        SparkEditor::SerializedComponent marker;
        marker.typeName = "Marker";
        sentinel.AddComponent(marker);
        return sentinel;
    }
} // namespace

TEST(PrefabMigration_V1FixtureLoadsDeclaredStateWithoutRewritingSource)
{
    const fs::path fixture = PrefabFixture("v1-guard-tower.sparkprefab");
    const std::string before = ReadBytes(fixture);
    ASSERT_TRUE(before.starts_with("SPARKPREFAB 1"));

    SparkEditor::PrefabAsset tower;
    std::string error;
    ASSERT_TRUE(SparkEditor::PrefabAsset::TryLoad(Utf8(fixture), tower, error));
    EXPECT_TRUE(error.empty());
    EXPECT_FALSE(tower.IsModified());
    EXPECT_EQ(tower.GetName(), std::string("Guard Tower"));
    ASSERT_EQ(tower.GetComponents().size(), static_cast<size_t>(2));
    EXPECT_EQ(tower.GetComponents()[0].typeName, std::string("Transform"));
    EXPECT_EQ(tower.GetComponents()[1].typeName, std::string("GuardTower"));

    EXPECT_TRUE(Holds(FindProperty(tower, "Transform", "position"), XMFLOAT3{1.25f, -3.5f, 12.0625f}));
    EXPECT_TRUE(Holds(FindProperty(tower, "Transform", "rotation"), XMFLOAT4{0.0f, 0.38268343f, 0.0f, 0.9238795f}));
    EXPECT_TRUE(Holds(FindProperty(tower, "Transform", "scale"), XMFLOAT3{1.0f, 1.0f, 1.0f}));
    EXPECT_TRUE(Holds(FindProperty(tower, "GuardTower", "health"), 250));
    EXPECT_TRUE(Holds(FindProperty(tower, "GuardTower", "garrisoned"), true));
    EXPECT_TRUE(Holds(FindProperty(tower, "GuardTower", "fireInterval"), 1.35));
    EXPECT_TRUE(Holds(FindProperty(tower, "GuardTower", "banner"), std::string("Crimson Keep")));

    // The load migrated in memory only: the fixture is byte-identical and nothing was staged beside it.
    EXPECT_EQ(ReadBytes(fixture), before);
    EXPECT_FALSE(fs::exists(WithSuffix(fixture, ".tmp")));
    EXPECT_FALSE(fs::exists(WithSuffix(fixture, ".bak")));
}

TEST(PrefabMigration_V1ResavesAsV2AndRoundTripsFloatsExactly)
{
    PrefabScratch scratch;
    const fs::path copy = scratch.File("Guard Tower.sparkprefab");
    const std::string v1Bytes = ReadBytes(PrefabFixture("v1-guard-tower.sparkprefab"));
    WriteBytes(copy, v1Bytes);

    SparkEditor::PrefabAsset tower;
    std::string error;
    ASSERT_TRUE(SparkEditor::PrefabAsset::TryLoad(Utf8(copy), tower, error));
    // Values version 1 stored as 17 significant digits must survive the shortest-form writer too.
    tower.GetComponents()[1].properties["spread"] = 1.2345678f;
    tower.GetComponents()[1].properties["drift"] = 0.1;
    ASSERT_TRUE(tower.Save(Utf8(copy)));

    const std::string v2Bytes = ReadBytes(copy);
    EXPECT_TRUE(v2Bytes.starts_with("SPARKPREFAB 2\nname \"Guard Tower\"\n"));
    EXPECT_TRUE(v2Bytes.ends_with("\nend\n"));
    EXPECT_STR_CONTAINS(v2Bytes, "  \"spread\" float 1.2345678\n");
    // The version 1 bytes stay recoverable as the previous-good copy.
    EXPECT_EQ(ReadBytes(WithSuffix(copy, ".bak")), v1Bytes);

    SparkEditor::PrefabAsset reloaded;
    ASSERT_TRUE(SparkEditor::PrefabAsset::TryLoad(Utf8(copy), reloaded, error));
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(SamePrefab(tower, reloaded));

    // An unchanged prefab re-saves to identical bytes: properties are written in name order.
    ASSERT_TRUE(reloaded.Save(Utf8(copy)));
    EXPECT_EQ(ReadBytes(copy), v2Bytes);
}

TEST(PrefabMigration_V2PreservesSpacesAndNewlinesInNamesAndStrings)
{
    PrefabScratch scratch;
    const std::string path = Utf8(scratch.File("Gate.sparkprefab"));

    // Every one of these names or values was refused by the version 1 writer.
    SparkEditor::PrefabAsset gate("North \"Gate\"\nWest Wing");
    SparkEditor::SerializedComponent siege;
    siege.typeName = "Siege Engine";
    siege.properties["firing arc"] = std::string("line one\nline two\r\n\t\"quoted\" C:\\path\\");
    siege.properties["crew count"] = 6;
    siege.properties["label"] = std::string(" padded ");
    siege.properties["empty"] = std::string();
    gate.AddComponent(siege);
    ASSERT_TRUE(gate.Save(path));

    SparkEditor::PrefabAsset reloaded;
    std::string error;
    ASSERT_TRUE(SparkEditor::PrefabAsset::TryLoad(path, reloaded, error));
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(SamePrefab(gate, reloaded));
    EXPECT_TRUE(Holds(FindProperty(reloaded, "Siege Engine", "firing arc"),
                      std::string("line one\nline two\r\n\t\"quoted\" C:\\path\\")));

    // An unknown escape is damage, not a character to guess at.
    std::string text = ReadBytes(scratch.File("Gate.sparkprefab"));
    const size_t escape = text.find("\\t");
    ASSERT_TRUE(escape != std::string::npos);
    text[escape + 1] = 'q';
    WriteBytes(scratch.File("Bad.sparkprefab"), text);
    SparkEditor::PrefabAsset out = Sentinel();
    EXPECT_FALSE(SparkEditor::PrefabAsset::TryLoad(Utf8(scratch.File("Bad.sparkprefab")), out, error));
    EXPECT_EQ(out.GetName(), std::string("Sentinel"));
    EXPECT_STR_CONTAINS(error, "property 'firing arc' of component 'Siege Engine' has a malformed string value");
}

TEST(PrefabMigration_V2CutInsideTheLastValueIsRejected)
{
    PrefabScratch scratch;
    const std::string path = Utf8(scratch.File("Cut.sparkprefab"));

    SparkEditor::PrefabAsset turret("Turret");
    SparkEditor::SerializedComponent stats;
    stats.typeName = "Stats";
    stats.properties["health"] = 250; // the only, so the last, property
    turret.AddComponent(stats);
    ASSERT_TRUE(turret.Save(path));

    // Version 1 accepted "  health int 25" here; the missing 'end' line now exposes the cut.
    const std::string whole = ReadBytes(scratch.File("Cut.sparkprefab"));
    const size_t valueEnd = whole.find("250\n");
    ASSERT_TRUE(valueEnd != std::string::npos);
    WriteBytes(scratch.File("Cut.sparkprefab"), whole.substr(0, valueEnd + 2));

    SparkEditor::PrefabAsset out = Sentinel();
    std::string error;
    EXPECT_FALSE(SparkEditor::PrefabAsset::TryLoad(path, out, error));
    EXPECT_EQ(out.GetName(), std::string("Sentinel"));
    EXPECT_STR_CONTAINS(error, "the closing 'end' line after the declared 1 components is missing");
}

TEST(PrefabMigration_FutureVersionFixtureFailsClosedWithVersionedError)
{
    const fs::path fixture = PrefabFixture("v3-future.sparkprefab");
    const std::string before = ReadBytes(fixture);
    ASSERT_TRUE(before.starts_with("SPARKPREFAB 3"));

    SparkEditor::PrefabAsset out = Sentinel();
    std::string error;
    EXPECT_FALSE(SparkEditor::PrefabAsset::TryLoad(Utf8(fixture), out, error));
    EXPECT_EQ(out.GetName(), std::string("Sentinel"));
    EXPECT_TRUE(out.HasComponent("Marker"));
    EXPECT_STR_CONTAINS(error, "v3-future.sparkprefab");
    EXPECT_STR_CONTAINS(error, "format version 3");
    EXPECT_STR_CONTAINS(error, "reads versions 1 to 2");
    EXPECT_STR_CONTAINS(error, "newer SparkEditor");
    EXPECT_EQ(ReadBytes(fixture), before);
}
