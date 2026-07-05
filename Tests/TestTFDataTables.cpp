/**
 * @file TestTFDataTables.cpp
 * @brief TERRAFRONT data-table validation against the REAL JSON files in
 *        Assets/MMOFPS/Data (weapons/vehicles/classes/regions/factions).
 *
 * Parses the shipped tables with the same JSON facility the module's loader
 * uses (Spark::Json, Utils/JsonUtils.h — see TFDataTables.cpp) and enforces
 * the loader's contract from the outside:
 *   - all five files parse, with the expected top-level arrays
 *   - unique ids/keys per table; vocabularies (slot/kind/tier/faction) closed
 *   - region conduit symmetry + valid endpoints, no self/duplicate links
 *   - initialOwnership partitions the full region set exactly once
 *   - every referenced model/audio/scene path exists on disk
 *
 * CI-standalone: pure file I/O + std::filesystem, no engine systems booted.
 * The repo root is found by walking up from the working directory, so the
 * suite passes whether it is run from the repo root or from build/bin.
 */

#include "TestFramework.h"

#include "Utils/JsonUtils.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <utility>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Spark::Json::Value;

namespace
{
    const fs::path& RepoRoot()
    {
        static const fs::path root = []() -> fs::path {
            fs::path p = fs::current_path();
            for (int i = 0; i < 10; ++i)
            {
                if (fs::exists(p / "Assets" / "MMOFPS" / "Data" / "regions.json"))
                    return p;
                const fs::path parent = p.parent_path();
                if (parent.empty() || parent == p)
                    break;
                p = parent;
            }
            return {};
        }();
        return root;
    }

    /// Parse one table from Assets/MMOFPS/Data. Null value on any failure.
    Value LoadTable(const char* file)
    {
        if (RepoRoot().empty())
            return {};
        std::ifstream f(RepoRoot() / "Assets" / "MMOFPS" / "Data" / file, std::ios::binary);
        if (!f.is_open())
            return {};
        std::ostringstream ss;
        ss << f.rdbuf();
        return Spark::Json::Parse(ss.str());
    }

    bool AssetExists(const std::string& relPath)
    {
        return fs::exists(RepoRoot() / "Assets" / relPath);
    }

    std::string Str(const Value& o, const char* key)
    {
        return o.HasKey(key) ? o[key].AsString(std::string{}) : std::string{};
    }

    double Num(const Value& o, const char* key, double def = 0.0)
    {
        return o.HasKey(key) ? o[key].AsNumber(def) : def;
    }

    bool InSet(const std::string& s, const std::set<std::string>& allowed)
    {
        return allowed.count(s) != 0;
    }

    // Vocabularies mirrored from TFDataTables.cpp's IsOneOf validation.
    const std::set<std::string> kWeaponSlots = {
        "rifle", "carbine", "lmg", "sniper", "pistol", "shotgun", "launcher",
        "melee", "tool", "colossus_autocannon", "vehicle_main", "vehicle_turret"};
    const std::set<std::string> kWeaponKinds = {"hitscan", "projectile", "melee", "beam"};
    const std::set<std::string> kRegionTiers = {"skyanchor", "outpost", "fort", "facility"};
    const std::set<std::string> kFactionTags = {"MRA", "AUC", "HLX"};
} // namespace

// ============================================================================
// Parse smoke — all five files, expected top-level shape
// ============================================================================

TEST(TFData_RepoRootFound)
{
    // Everything below depends on this; fail loudly if the data dir is gone.
    EXPECT_FALSE(RepoRoot().empty());
}

TEST(TFData_AllFiveTablesParse)
{
    EXPECT_TRUE(LoadTable("weapons.json")["weapons"].IsArray());
    EXPECT_TRUE(LoadTable("vehicles.json")["vehicles"].IsArray());
    EXPECT_TRUE(LoadTable("classes.json")["classes"].IsArray());
    EXPECT_TRUE(LoadTable("factions.json")["factions"].IsArray());
    const Value regions = LoadTable("regions.json");
    EXPECT_TRUE(regions["regions"].IsArray());
    EXPECT_TRUE(regions["conduits"].IsArray());
    EXPECT_TRUE(regions["continent"].IsObject());
    EXPECT_TRUE(regions["initialOwnership"].IsObject());
}

// ============================================================================
// weapons.json
// ============================================================================

TEST(TFData_Weapons_UniqueIdsAndKeys_ClosedVocabulary)
{
    const Value doc = LoadTable("weapons.json");
    const Value& list = doc["weapons"];
    EXPECT_GT(list.Size(), size_t{15}); // 3x5 faction weapons + common pool

    std::set<int> ids;
    std::set<std::string> keys;
    for (size_t i = 0; i < list.Size(); ++i)
    {
        const Value& w = list[i];
        const int id = w["id"].AsInt(-1);
        const std::string key = Str(w, "key");
        EXPECT_GT(id, 0);
        EXPECT_FALSE(key.empty());
        EXPECT_TRUE(ids.insert(id).second);    // unique id
        EXPECT_TRUE(keys.insert(key).second);  // unique key

        EXPECT_TRUE(InSet(Str(w, "slot"), kWeaponSlots));
        EXPECT_TRUE(InSet(Str(w, "kind"), kWeaponKinds));

        const std::string faction = Str(w, "faction");
        EXPECT_TRUE(faction == "ALL" || InSet(faction, kFactionTags));

        // Sanity on the combat numbers the damage model consumes.
        EXPECT_NE(Num(w, "damage"), 0.0); // tools are negative (heal), never 0
        EXPECT_GT(Num(w, "rofRpm"), 0.0);
        if (Str(w, "kind") == "projectile")
            EXPECT_GT(Num(w, "projSpeed"), 0.0);
        EXPECT_GE(Num(w, "falloffEndM"), Num(w, "falloffStartM"));
    }

    // The three faction rifles + the design's TTK reference gun exist.
    EXPECT_TRUE(keys.count("mra_rifle") && keys.count("auc_rifle") && keys.count("hlx_rifle"));
}

TEST(TFData_Weapons_ReferencedAssetPathsExist)
{
    const Value doc = LoadTable("weapons.json");
    const Value& list = doc["weapons"];
    for (size_t i = 0; i < list.Size(); ++i)
    {
        const Value& w = list[i];
        for (const char* field : {"model", "audioFire", "audioReload"})
        {
            const std::string path = Str(w, field);
            if (!path.empty()) // vehicle-mounted guns legitimately have no model
                EXPECT_TRUE(AssetExists(path));
        }
    }
}

// ============================================================================
// regions.json — lattice topology + ownership
// ============================================================================

TEST(TFData_Regions_UniqueDenseIds_TierRules)
{
    const Value doc = LoadTable("regions.json");
    const Value& list = doc["regions"];
    EXPECT_GT(list.Size(), size_t{3});

    std::set<int> ids;
    const double sizeM = Num(doc["continent"], "sizeM", 4096.0);
    for (size_t i = 0; i < list.Size(); ++i)
    {
        const Value& r = list[i];
        const int id = r["id"].AsInt(-1);
        EXPECT_GE(id, 0);
        EXPECT_LT(id, static_cast<int>(list.Size())); // dense: ids index the array
        EXPECT_TRUE(ids.insert(id).second);

        const std::string tier = Str(r, "tier");
        EXPECT_TRUE(InSet(tier, kRegionTiers));
        if (tier == "skyanchor")
        {
            EXPECT_TRUE(InSet(Str(r, "homeFaction"), kFactionTags)); // must have a home
            EXPECT_NEAR(Num(r, "captureSec", -1.0), 0.0, 0.001);     // never capturable
        }
        else
        {
            EXPECT_GT(Num(r, "captureSec"), 0.0);
            EXPECT_TRUE(r["capturePoints"].IsArray());
            EXPECT_GT(r["capturePoints"].Size(), size_t{0});
        }
        EXPECT_TRUE(r["spawns"].IsArray());
        EXPECT_GT(r["spawns"].Size(), size_t{0});

        // Centers stay inside the continent.
        EXPECT_TRUE(r["center"].IsArray());
        EXPECT_GE(r["center"][size_t{0}].AsNumber(-1.0), 0.0);
        EXPECT_LE(r["center"][size_t{0}].AsNumber(1e9), sizeM);
        EXPECT_GE(r["center"][size_t{1}].AsNumber(-1.0), 0.0);
        EXPECT_LE(r["center"][size_t{1}].AsNumber(1e9), sizeM);
    }
    EXPECT_EQ(ids.size(), list.Size());
}

TEST(TFData_Regions_ConduitSymmetryAndValidity)
{
    const Value doc = LoadTable("regions.json");
    const size_t regionCount = doc["regions"].Size();
    const Value& conduits = doc["conduits"];
    EXPECT_GT(conduits.Size(), size_t{0});

    std::set<std::pair<int, int>> seen;
    for (size_t i = 0; i < conduits.Size(); ++i)
    {
        const Value& link = conduits[i];
        EXPECT_TRUE(link.IsArray());
        EXPECT_EQ(link.Size(), size_t{2});
        const int a = link[size_t{0}].AsInt(-1);
        const int b = link[size_t{1}].AsInt(-1);
        EXPECT_GE(a, 0);
        EXPECT_GE(b, 0);
        EXPECT_LT(a, static_cast<int>(regionCount));
        EXPECT_LT(b, static_cast<int>(regionCount));
        EXPECT_NE(a, b); // no self-links

        // Conduits are undirected: the pair (min,max) must be unique — a
        // duplicate or a reversed re-listing is authoring error.
        const std::pair<int, int> norm{std::min(a, b), std::max(a, b)};
        EXPECT_TRUE(seen.insert(norm).second);
    }
}

TEST(TFData_Regions_InitialOwnershipPartitionsAllRegions)
{
    const Value doc = LoadTable("regions.json");
    const size_t regionCount = doc["regions"].Size();
    const Value& own = doc["initialOwnership"];

    std::set<int> covered;
    size_t listed = 0;
    for (const char* group : {"MRA", "AUC", "HLX", "neutral"})
    {
        EXPECT_TRUE(own.HasKey(group));
        const Value& arr = own[group];
        EXPECT_TRUE(arr.IsArray());
        for (size_t i = 0; i < arr.Size(); ++i)
        {
            const int id = arr[i].AsInt(-1);
            EXPECT_GE(id, 0);
            EXPECT_LT(id, static_cast<int>(regionCount));
            EXPECT_TRUE(covered.insert(id).second); // no region owned twice
            ++listed;
        }
    }
    EXPECT_EQ(covered.size(), regionCount); // every region assigned
    EXPECT_EQ(listed, regionCount);

    // Each skyanchor starts owned by its home faction.
    const Value& regions = doc["regions"];
    for (size_t i = 0; i < regions.Size(); ++i)
    {
        const Value& r = regions[i];
        if (Str(r, "tier") != "skyanchor")
            continue;
        const std::string home = Str(r, "homeFaction");
        const Value& homeList = own[home];
        bool found = false;
        for (size_t j = 0; j < homeList.Size(); ++j)
            found = found || homeList[j].AsInt(-1) == r["id"].AsInt(-2);
        EXPECT_TRUE(found);
    }
}

TEST(TFData_Regions_SceneFileExists)
{
    const Value doc = LoadTable("regions.json");
    const std::string scene = Str(doc["continent"], "scene");
    EXPECT_FALSE(scene.empty());
    EXPECT_TRUE(AssetExists(scene));
}

// ============================================================================
// classes.json
// ============================================================================

TEST(TFData_Classes_SixClasses_ValidLoadoutVocabulary)
{
    const Value doc = LoadTable("classes.json");
    const Value& list = doc["classes"];
    EXPECT_EQ(list.Size(), size_t{6});

    const std::set<std::string> expected = {"Ghost", "Striker", "Medtech",
                                            "Fabricator", "Bulwark", "Colossus"};
    std::set<std::string> ids;
    for (size_t i = 0; i < list.Size(); ++i)
    {
        const Value& c = list[i];
        const std::string id = Str(c, "id");
        EXPECT_TRUE(InSet(id, expected));
        EXPECT_TRUE(ids.insert(id).second);

        EXPECT_GT(Num(c, "health"), 0.0);
        EXPECT_GE(Num(c, "shield"), 0.0); // Colossus runs shieldless
        EXPECT_GT(Num(c, "sprintSpeed"), Num(c, "runSpeed") * 0.99);

        // Primary slots must come from the weapon-slot vocabulary.
        const Value& primaries = c["primaries"];
        EXPECT_TRUE(primaries.IsArray());
        EXPECT_GT(primaries.Size(), size_t{0});
        for (size_t j = 0; j < primaries.Size(); ++j)
            EXPECT_TRUE(InSet(primaries[j].AsString(std::string{}), kWeaponSlots));

        const std::string secondary = Str(c, "secondary");
        EXPECT_TRUE(secondary == "none" || InSet(secondary, kWeaponSlots));

        EXPECT_TRUE(c["ability"].IsObject());
        EXPECT_FALSE(Str(c["ability"], "key").empty());
    }
    EXPECT_EQ(ids.size(), size_t{6});
}

// ============================================================================
// vehicles.json
// ============================================================================

TEST(TFData_Vehicles_ValidIdsSeatsAndWeaponRefs)
{
    const Value weaponsDoc = LoadTable("weapons.json");
    std::set<std::string> weaponKeys;
    for (size_t i = 0; i < weaponsDoc["weapons"].Size(); ++i)
        weaponKeys.insert(Str(weaponsDoc["weapons"][i], "key"));

    const Value doc = LoadTable("vehicles.json");
    const Value& list = doc["vehicles"];
    EXPECT_GE(list.Size(), size_t{3}); // Drifter, Aegis, Ravager (+Vulture stretch)

    const std::set<std::string> knownIds = {"Drifter", "Aegis", "Ravager", "Vulture"};
    const std::set<std::string> seatRoles = {"driver", "gunner", "passenger"};
    std::set<std::string> ids;
    bool sawAegisDeploySpawn = false;

    for (size_t i = 0; i < list.Size(); ++i)
    {
        const Value& v = list[i];
        const std::string id = Str(v, "id");
        EXPECT_TRUE(InSet(id, knownIds));
        EXPECT_TRUE(ids.insert(id).second);

        EXPECT_GT(Num(v, "health"), 0.0);
        EXPECT_GT(Num(v, "topSpeed"), 0.0);

        const Value& seats = v["seats"];
        EXPECT_TRUE(seats.IsArray());
        EXPECT_GT(seats.Size(), size_t{0});
        EXPECT_LE(seats.Size(), size_t{8}); // TF_RepVehicleSeats.seats[8]
        EXPECT_EQ(Str(seats[size_t{0}], "role"), std::string("driver"));
        for (size_t j = 0; j < seats.Size(); ++j)
        {
            EXPECT_TRUE(InSet(Str(seats[j], "role"), seatRoles));
            const std::string wkey = Str(seats[j], "weapon");
            if (!wkey.empty())
                EXPECT_TRUE(weaponKeys.count(wkey) != 0); // must reference weapons.json
        }

        if (id == "Aegis")
        {
            EXPECT_TRUE(v.HasKey("deploySpawn"));
            EXPECT_GT(Num(v["deploySpawn"], "radiusM"), 0.0);
            sawAegisDeploySpawn = true;
        }

        for (const char* field : {"model", "audioEngine", "explodeAudio"})
        {
            const std::string path = Str(v, field);
            if (!path.empty())
                EXPECT_TRUE(AssetExists(path));
        }
    }
    EXPECT_TRUE(sawAegisDeploySpawn); // the core logistics unit must exist
}

// ============================================================================
// factions.json
// ============================================================================

TEST(TFData_Factions_ThreePowers_CompleteTraits)
{
    const Value doc = LoadTable("factions.json");
    const Value& list = doc["factions"];
    EXPECT_EQ(list.Size(), size_t{3});

    std::set<int> ids;
    std::set<std::string> tags;
    for (size_t i = 0; i < list.Size(); ++i)
    {
        const Value& f = list[i];
        const int id = f["id"].AsInt(-1);
        EXPECT_GE(id, 1);
        EXPECT_LE(id, 3); // FactionId::MRA..HLX
        EXPECT_TRUE(ids.insert(id).second);
        EXPECT_TRUE(tags.insert(Str(f, "tag")).second);
        EXPECT_TRUE(InSet(Str(f, "tag"), kFactionTags));
        EXPECT_FALSE(Str(f, "name").empty());

        EXPECT_TRUE(f["color"].IsArray());
        EXPECT_EQ(f["color"].Size(), size_t{3});

        const Value& traits = f["traits"];
        EXPECT_TRUE(traits.IsObject());
        EXPECT_GT(Num(traits, "rofMult"), 0.0);
        EXPECT_GT(Num(traits, "damageMult"), 0.0);
        EXPECT_GT(Num(traits, "reloadMult"), 0.0);
        EXPECT_GE(Num(traits, "projGravityMult", -1.0), 0.0); // HLX: 0 = no drop
        EXPECT_GT(Num(traits, "shieldRegenDelaySec"), 0.0);
    }
    EXPECT_TRUE(tags == kFactionTags); // exactly MRA/AUC/HLX, no strangers
}
