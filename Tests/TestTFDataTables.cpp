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

#include "Data/TFDataTablesInternal.h"
#include "Persistence/TFSavePaths.h"
#include "World/TFAssetPaths.h"
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
        static const fs::path root = []() -> fs::path
        {
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
        "rifle",        "carbine",       "lmg",   "sniper", "pistol",
        "shotgun",      "launcher",      "melee", "tool",   "colossus_autocannon",
        "vehicle_main", "vehicle_turret"};
    const std::set<std::string> kWeaponKinds = {"hitscan", "projectile", "melee", "beam"};
    const std::set<std::string> kRegionTiers = {"skyanchor", "outpost", "fort", "facility"};
    const std::set<std::string> kFactionTags = {"MRA", "AUC", "HLX"};
} // namespace

// ============================================================================
// Parse smoke — every JSON data file, expected top-level shape
// ============================================================================

TEST(TFData_RepoRootFound)
{
    // Everything below depends on this; fail loudly if the data dir is gone.
    EXPECT_FALSE(RepoRoot().empty());
}

TEST(TFData_AllDataFilesParse)
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

    const Value highlands = LoadTable("regions_highlands.json");
    EXPECT_TRUE(highlands["regions"].IsArray());
    EXPECT_TRUE(highlands["conduits"].IsArray());
    EXPECT_TRUE(highlands["continent"].IsObject());
    EXPECT_TRUE(highlands["initialOwnership"].IsObject());
    EXPECT_TRUE(LoadTable("continents.json")["continents"].IsArray());
    EXPECT_TRUE(LoadTable("presentation.json")["presentation"].IsObject());
    EXPECT_TRUE(LoadTable("deployables.json")["deployables"].IsArray());
    EXPECT_TRUE(LoadTable("decor.json")["templates"].IsObject());
    EXPECT_TRUE(LoadTable("suits.json")["suits"].IsArray());
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
        EXPECT_TRUE(ids.insert(id).second);   // unique id
        EXPECT_TRUE(keys.insert(key).second); // unique key

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

    const std::set<std::string> expected = {"Ghost", "Striker", "Medtech", "Fabricator", "Bulwark", "Colossus"};
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

// ============================================================================
// presentation.json / deployables.json (sp4 de-hardcode)
// ============================================================================

TEST(TFData_Presentation_ParsesAndAssetsExist)
{
    const Value doc = LoadTable("presentation.json");
    EXPECT_TRUE(doc["presentation"].IsObject());
    EXPECT_TRUE(doc["presentation"]["skybox"]["faceTex"].IsArray());
    EXPECT_EQ(doc["presentation"]["skybox"]["faceTex"].Size(), size_t{6});
    for (size_t i = 0; i < doc["presentation"]["skybox"]["faceTex"].Size(); ++i)
    {
        const std::string texPath = doc["presentation"]["skybox"]["faceTex"][i].AsString(std::string{});
        EXPECT_FALSE(texPath.empty());
        EXPECT_TRUE(AssetExists(texPath));
    }
    EXPECT_TRUE(AssetExists(Str(doc["presentation"]["terrain"], "texture")));
    EXPECT_TRUE(AssetExists(Str(doc["presentation"]["ambient"], "path")));
    EXPECT_TRUE(AssetExists(Str(doc["presentation"], "pawnMesh")));
    EXPECT_GT(Num(doc["presentation"]["skybox"], "scale"), 0.0);
    EXPECT_GT(Num(doc["presentation"]["terrain"], "uvTiles"), 0.0);
    EXPECT_GE(Num(doc["presentation"]["ambient"], "volume"), 0.0);
    EXPECT_LE(Num(doc["presentation"]["ambient"], "volume"), 1.0);
}

TEST(TFData_Presentation_ProductionParserHonorsRootSkyboxOverrides)
{
    const Value doc = LoadTable("presentation.json");
    Terrafront::WorldPresentationDef parsed;
    std::string error;
    EXPECT_TRUE(Terrafront::DataTablesDetail::ParsePresentation(doc, parsed, error));
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(parsed.skybox.faceTex[0], std::string("Textures/MMOFPS/sky/veyra_px.png"));
    EXPECT_EQ(parsed.sanctuarySkybox.faceTex[0], std::string("Textures/MMOFPS/sky/sanctuary_px.png"));
    for (const std::string& face : parsed.skybox.faceTex)
        EXPECT_TRUE(AssetExists(face));
    for (const std::string& face : parsed.sanctuarySkybox.faceTex)
        EXPECT_TRUE(AssetExists(face));
}

TEST(TFData_Presentation_ProductionParserAcceptsNestedOnlySkyboxes)
{
    const Value doc = Spark::Json::Parse(R"json(
        {
          "presentation": {
            "skybox": { "faceTex": ["a", "b", "c", "d", "e", "f"], "scale": 100.0 },
            "sanctuarySkybox": { "faceTex": ["g", "h", "i", "j", "k", "l"], "scale": 80.0 }
          }
        }
    )json");
    Terrafront::WorldPresentationDef parsed;
    std::string error;
    EXPECT_TRUE(Terrafront::DataTablesDetail::ParsePresentation(doc, parsed, error));
    EXPECT_EQ(parsed.skybox.faceTex[0], std::string("a"));
    EXPECT_EQ(parsed.sanctuarySkybox.faceTex[5], std::string("l"));
}

TEST(TFData_ContinentsHaveUniquePersistenceSafeKeys)
{
    const Value document = LoadTable("continents.json");
    const Value& continents = document["continents"];
    std::set<std::string> keys;
    for (size_t i = 0; i < continents.Size(); ++i)
    {
        const Value& row = continents[i];
        const std::string key = Str(row, "key");
        EXPECT_TRUE(Terrafront::SavePaths::IsValidContinentKey(key));
        EXPECT_TRUE(keys.insert(key).second);
        if (Str(row, "kind") == "continent")
            EXPECT_FALSE(Str(row, "regions").empty());
    }
    EXPECT_TRUE(keys.count("cindral_wastes") == 1);
    EXPECT_TRUE(keys.count("veyra_highlands") == 1);
}

TEST(TFData_Presentation_LegacyNestedSanctuaryAllowsPartialOverride)
{
    const Value doc = Spark::Json::Parse(R"json(
        {
          "presentation": {
            "skybox": { "faceTex": ["a", "b", "c", "d", "e", "f"], "scale": 100.0 },
            "sanctuarySkybox": { "faceTex": ["ignored-partial"], "scale": 77.0,
                                  "tint": [0.1, 0.2, 0.3, 1.0] }
          }
        }
    )json");
    Terrafront::WorldPresentationDef parsed;
    std::string error;
    EXPECT_TRUE(Terrafront::DataTablesDetail::ParsePresentation(doc, parsed, error));
    EXPECT_EQ(parsed.sanctuarySkybox.faceTex[0], std::string("a"));
    EXPECT_NEAR(parsed.sanctuarySkybox.scale, 77.0f, 0.001f);
    EXPECT_NEAR(parsed.sanctuarySkybox.tint[2], 0.3f, 0.001f);
}

TEST(TFData_Presentation_RootSanctuaryPrecedenceRemainsStrict)
{
    const Value doc = Spark::Json::Parse(R"json(
        {
          "presentation": {
            "skybox": { "faceTex": ["a", "b", "c", "d", "e", "f"] },
            "sanctuarySkybox": { "scale": 77.0 }
          },
          "sanctuarySkybox": { "scale": 88.0 }
        }
    )json");
    Terrafront::WorldPresentationDef parsed;
    std::string error;
    EXPECT_FALSE(Terrafront::DataTablesDetail::ParsePresentation(doc, parsed, error));
    EXPECT_TRUE(error.find("root sanctuarySkybox") != std::string::npos);
}

TEST(TFData_Presentation_MalformedRootOverrideFailsClosed)
{
    const Value doc = Spark::Json::Parse(R"json(
        {
          "presentation": {
            "skybox": { "faceTex": ["a", "b", "c", "d", "e", "f"] }
          },
          "skybox": { "faceTex": ["incomplete"] }
        }
    )json");
    Terrafront::WorldPresentationDef parsed;
    std::string error;
    EXPECT_FALSE(Terrafront::DataTablesDetail::ParsePresentation(doc, parsed, error));
    EXPECT_TRUE(error.find("exactly 6") != std::string::npos);
}

TEST(TFSavePaths_OneRootAndLeafOnlyFiles)
{
    const fs::path cwd = fs::path("server-root");
    EXPECT_TRUE(Terrafront::SavePaths::ResolveRoot({}, cwd) == (cwd / "Saves").lexically_normal());
    EXPECT_TRUE(Terrafront::SavePaths::ResolveRoot("shared/terrafront", cwd) ==
                (cwd / "shared" / "terrafront").lexically_normal());
    EXPECT_TRUE(Terrafront::SavePaths::ResolveRoot({}, {}).empty());
    EXPECT_TRUE(Terrafront::SavePaths::ResolveRoot("relative", {}).empty());

    const fs::path account = Terrafront::SavePaths::File("terrafront.db");
    const fs::path territory = Terrafront::SavePaths::File("terrafront_territory.json");
    EXPECT_FALSE(account.empty());
    EXPECT_TRUE(account.parent_path() == Terrafront::SavePaths::Root());
    EXPECT_TRUE(territory.parent_path() == Terrafront::SavePaths::Root());
    EXPECT_TRUE(Terrafront::SavePaths::File("../escape.json").empty());
    EXPECT_TRUE(Terrafront::SavePaths::File("nested/escape.json").empty());
    EXPECT_TRUE(Terrafront::SavePaths::File(".").empty());
    EXPECT_TRUE(Terrafront::SavePaths::IsValidContinentKey("cindral_wastes"));
    EXPECT_FALSE(Terrafront::SavePaths::IsValidContinentKey("../escape"));
    const fs::path scoped = Terrafront::SavePaths::ContinentFile("terrafront_state", "cindral_wastes");
    EXPECT_EQ(scoped.filename(), fs::path("terrafront_state.cindral_wastes.json"));

#ifdef _WIN32
    const fs::path unicodeRoot = Terrafront::SavePaths::ResolveRootWide(L"保存/данные", fs::path(L"D:/服务器"));
    EXPECT_TRUE(unicodeRoot == (fs::path(L"D:/服务器") / L"保存/данные").lexically_normal());
    const fs::path unc = Terrafront::SavePaths::ResolveRootWide(LR"(\\server\share\保存)", cwd);
#if defined(__MINGW32__)
    // MinGW libstdc++ does not classify UNC paths as absolute, but the native
    // Windows path must remain UNC-rooted and must never be prefixed by cwd.
    EXPECT_TRUE(unc.native().find(LR"(\\server\share)") == 0);
#else
    EXPECT_TRUE(unc.is_absolute());
    EXPECT_TRUE(unc.root_name() == fs::path(LR"(\\server\share)").root_name());
#endif
    EXPECT_FALSE(unc.native().find(cwd.native()) == 0);
#endif
}

TEST(TFAssetPaths_ConfinesRelativeAndLoaderResolvedAbsolutePaths)
{
    const fs::path canonicalRoot = fs::canonical(RepoRoot());
    const auto rootUtf8Raw = canonicalRoot.generic_u8string();
    const std::string rootUtf8(rootUtf8Raw.begin(), rootUtf8Raw.end());

    const auto relative = Terrafront::ResolveContentAssetPath(rootUtf8, "Assets/MMOFPS/Data/presentation.json");
    EXPECT_TRUE(relative.has_value());
    if (!relative)
        return;
    EXPECT_TRUE(relative->nativePath == canonicalRoot / "Assets/MMOFPS/Data/presentation.json");

    const auto absoluteRaw = (canonicalRoot / "Assets/MMOFPS/Data/presentation.json").generic_u8string();
    const std::string absolute(absoluteRaw.begin(), absoluteRaw.end());
    const auto loaderResolved = Terrafront::ResolveContentAssetPath(rootUtf8, absolute);
    EXPECT_TRUE(loaderResolved.has_value());
    if (!loaderResolved)
        return;
    EXPECT_TRUE(loaderResolved->cacheKey == relative->cacheKey);

    EXPECT_FALSE(Terrafront::ResolveContentAssetPath(rootUtf8, "../outside.png").has_value());
    const auto outsideRaw = (canonicalRoot.parent_path() / "outside.png").generic_u8string();
    EXPECT_FALSE(
        Terrafront::ResolveContentAssetPath(rootUtf8, std::string(outsideRaw.begin(), outsideRaw.end())).has_value());
}

TEST(TFData_Presentation_MuzzleFxSaneRanges)
{
    const Value doc = LoadTable("presentation.json");
    const Value& muzzleFx = doc["presentation"]["muzzleFx"];
    EXPECT_GT(Num(muzzleFx, "tracerLenM"), 0.0);
    EXPECT_GT(Num(muzzleFx, "tracerThickM"), 0.0);
    EXPECT_GT(Num(muzzleFx, "tracerLifeSec"), 0.0);
    EXPECT_GT(Num(muzzleFx, "flashLifeSec"), 0.0);
    EXPECT_EQ(muzzleFx["tracerColor"].Size(), size_t{4});
    EXPECT_EQ(muzzleFx["flashColor"].Size(), size_t{4});
}

TEST(TFData_Deployables_ClosedVocabularyAndAssetsExist)
{
    const Value doc = LoadTable("deployables.json");
    EXPECT_TRUE(doc["deployables"].IsArray());
    // W6 extended kinds (ResupplyStation/AVTurret/ShieldWall, values 3-5 in
    // Game/TFDeployableTypes.h) joined the table's documented vocabulary.
    std::set<std::string> expectedIds = {"FabTurret",       "FabAmmoPack", "MedBeacon",
                                         "ResupplyStation", "AVTurret",    "ShieldWall"};
    for (size_t i = 0; i < doc["deployables"].Size(); ++i)
    {
        const Value& deployable = doc["deployables"][i];
        const std::string id = Str(deployable, "id");
        EXPECT_TRUE(InSet(id, expectedIds));
        EXPECT_EQ(expectedIds.erase(id), size_t{1}); // also catches duplicates
        EXPECT_FALSE(Str(deployable, "model").empty());
        EXPECT_TRUE(AssetExists(Str(deployable, "model")));
        EXPECT_EQ(deployable["scale"].Size(), size_t{3});
    }
    EXPECT_EQ(expectedIds.size(), size_t{0}); // every documented kind present
}

TEST(TFData_Factions_StructureMaterialAssetsExist)
{
    const Value doc = LoadTable("factions.json");
    const Value& list = doc["factions"];
    for (size_t i = 0; i < list.Size(); ++i)
    {
        // Unlike model/texture/audio paths (relative, AssetExists() prepends
        // "Assets/"), structureMaterial is stored WITH the "Assets/" prefix
        // baked in - it's used directly as MeshRenderer::materialPath at
        // runtime (see Game/TFVisualUtils.h), matching every other faction
        // tint-material literal already in this codebase. Check as-is.
        const std::string structureMaterial = Str(list[i], "structureMaterial");
        EXPECT_FALSE(structureMaterial.empty());
        EXPECT_TRUE(fs::exists(RepoRoot() / structureMaterial));
    }
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
