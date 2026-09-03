/**
 * @file TestTFRegionLattice.cpp
 * @brief TERRAFRONT lattice rules (capturability + spawn connectivity) over
 *        the REAL Cindral Wastes topology in Assets/MMOFPS/Data/regions.json.
 *
 * Module sources are NOT compiled into SparkTests, so the two lattice rules
 * are reimplemented standalone, mirroring TFRegionSystem.cpp exactly:
 *
 *   IsCapturable(r, f):  r not owned by f, r not a skyanchor (captureSec > 0),
 *                        and r conduit-linked to at least one f-owned region.
 *   CanSpawnAt(r, f):    f owns r AND BFS from f's skyanchor(s) across
 *                        f-owned regions only reaches r (unbroken owned chain).
 *
 * The topology, home skyanchors and initialOwnership are read from the real
 * regions.json — if a designer edits the map into a state that breaks the
 * opening lattice (e.g. a faction spawn-island), these tests catch it.
 */

#include "TestFramework.h"

#include "Utils/JsonUtils.h"

#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Spark::Json::Value;

namespace
{
    enum Faction : int
    {
        None = 0,
        MRA = 1,
        AUC = 2,
        HLX = 3
    };

    struct Lattice
    {
        int regionCount = 0;
        std::vector<std::vector<int>> adj; // undirected conduit graph
        std::vector<std::string> tier;     // per region
        std::vector<float> captureSec;     // per region
        std::vector<int> home;             // skyanchor home faction (None otherwise)
        std::vector<int> owner;            // mutable current ownership
        bool loaded = false;
    };

    int FactionFromTag(const std::string& tag)
    {
        if (tag == "MRA")
            return MRA;
        if (tag == "AUC")
            return AUC;
        if (tag == "HLX")
            return HLX;
        return None;
    }

    Lattice LoadLattice()
    {
        Lattice lat;

        fs::path root = fs::current_path();
        for (int i = 0; i < 10 && !fs::exists(root / "Assets/MMOFPS/Data/regions.json"); ++i)
        {
            const fs::path parent = root.parent_path();
            if (parent.empty() || parent == root)
                return lat;
            root = parent;
        }
        std::ifstream f(root / "Assets/MMOFPS/Data/regions.json", std::ios::binary);
        if (!f.is_open())
            return lat;
        std::ostringstream ss;
        ss << f.rdbuf();
        const Value doc = Spark::Json::Parse(ss.str());
        if (!doc.IsObject() || !doc["regions"].IsArray())
            return lat;

        const Value& regions = doc["regions"];
        lat.regionCount = static_cast<int>(regions.Size());
        lat.adj.resize(lat.regionCount);
        lat.tier.resize(lat.regionCount);
        lat.captureSec.assign(lat.regionCount, 0.0f);
        lat.home.assign(lat.regionCount, None);
        lat.owner.assign(lat.regionCount, None);

        for (size_t i = 0; i < regions.Size(); ++i)
        {
            const Value& r = regions[i];
            const int id = r["id"].AsInt(-1);
            if (id < 0 || id >= lat.regionCount)
                return lat; // malformed — leave !loaded
            lat.tier[id] = r["tier"].AsString(std::string{});
            lat.captureSec[id] = static_cast<float>(r.HasKey("captureSec") ? r["captureSec"].AsNumber(0.0) : 0.0);
            if (r.HasKey("homeFaction"))
                lat.home[id] = FactionFromTag(r["homeFaction"].AsString(std::string{}));
        }

        const Value& conduits = doc["conduits"];
        for (size_t i = 0; i < conduits.Size(); ++i)
        {
            const int a = conduits[i][size_t{0}].AsInt(-1);
            const int b = conduits[i][size_t{1}].AsInt(-1);
            if (a < 0 || b < 0 || a >= lat.regionCount || b >= lat.regionCount)
                return lat;
            lat.adj[a].push_back(b);
            lat.adj[b].push_back(a); // undirected
        }

        const Value& own = doc["initialOwnership"];
        for (const char* tag : {"MRA", "AUC", "HLX"})
        {
            const Value& arr = own[tag];
            for (size_t i = 0; i < arr.Size(); ++i)
            {
                const int id = arr[i].AsInt(-1);
                if (id >= 0 && id < lat.regionCount)
                    lat.owner[id] = FactionFromTag(tag);
            }
        }

        lat.loaded = true;
        return lat;
    }

    /// Fresh copy of the shipped opening state for each test.
    const Lattice& InitialLattice()
    {
        static const Lattice lat = LoadLattice();
        return lat;
    }

    // --- rule mirrors (TFRegionSystem.cpp) -----------------------------------

    bool IsCapturable(const Lattice& lat, int region, int attacker)
    {
        if (region < 0 || region >= lat.regionCount || attacker == None)
            return false;
        if (lat.owner[region] == attacker)
            return false;
        if (lat.tier[region] == "skyanchor" || lat.captureSec[region] <= 0.0f)
            return false;
        for (int nb : lat.adj[region])
            if (lat.owner[nb] == attacker)
                return true;
        return false;
    }

    bool CanSpawnAt(const Lattice& lat, int region, int faction)
    {
        if (region < 0 || region >= lat.regionCount || faction == None)
            return false;
        if (lat.owner[region] != faction)
            return false;

        // BFS from the faction's skyanchor(s) across owned regions only.
        std::vector<char> visited(lat.regionCount, 0);
        std::deque<int> open;
        for (int i = 0; i < lat.regionCount; ++i)
        {
            if (lat.tier[i] == "skyanchor" && lat.home[i] == faction)
            {
                open.push_back(i);
                visited[i] = 1;
            }
        }
        while (!open.empty())
        {
            const int cur = open.front();
            open.pop_front();
            if (cur == region)
                return true;
            for (int nb : lat.adj[cur])
            {
                if (visited[nb] || lat.owner[nb] != faction)
                    continue;
                visited[nb] = 1;
                open.push_back(nb);
            }
        }
        return false;
    }
} // namespace

// ============================================================================
// Topology sanity
// ============================================================================

TEST(TFLattice_TopologyLoads)
{
    const Lattice& lat = InitialLattice();
    EXPECT_TRUE(lat.loaded);
    EXPECT_GT(lat.regionCount, 3);

    // Exactly one skyanchor per faction, each owned by its home power.
    int anchors[4] = {0, 0, 0, 0};
    for (int i = 0; i < lat.regionCount; ++i)
    {
        if (lat.tier[i] != "skyanchor")
            continue;
        EXPECT_NE(lat.home[i], None);
        EXPECT_EQ(lat.owner[i], lat.home[i]);
        ++anchors[lat.home[i]];
    }
    EXPECT_EQ(anchors[MRA], 1);
    EXPECT_EQ(anchors[AUC], 1);
    EXPECT_EQ(anchors[HLX], 1);
}

TEST(TFLattice_GraphIsConnected_NoIsolatedRegions)
{
    const Lattice& lat = InitialLattice();
    ASSERT_TRUE(lat.loaded);
    // Ownership-blind BFS from region 0 must reach everything: an unreachable
    // region could never be fought over.
    std::vector<char> visited(lat.regionCount, 0);
    std::deque<int> open{0};
    visited[0] = 1;
    int reached = 0;
    while (!open.empty())
    {
        const int cur = open.front();
        open.pop_front();
        ++reached;
        for (int nb : lat.adj[cur])
            if (!visited[nb])
            {
                visited[nb] = 1;
                open.push_back(nb);
            }
    }
    EXPECT_EQ(reached, lat.regionCount);
}

// ============================================================================
// Spawn connectivity (BFS owned-chain rule)
// ============================================================================

TEST(TFLattice_OpeningState_EveryOwnedRegionIsSpawnable)
{
    // The shipped initialOwnership must not contain spawn-islands: every
    // region a faction starts with must trace an owned conduit chain back to
    // its skyanchor.
    const Lattice& lat = InitialLattice();
    ASSERT_TRUE(lat.loaded);
    for (int r = 0; r < lat.regionCount; ++r)
    {
        if (lat.owner[r] == None)
            continue;
        EXPECT_TRUE(CanSpawnAt(lat, r, lat.owner[r]));
    }
}

TEST(TFLattice_CannotSpawnAtEnemyOrNeutralRegions)
{
    const Lattice& lat = InitialLattice();
    ASSERT_TRUE(lat.loaded);
    for (int r = 0; r < lat.regionCount; ++r)
        for (int f : {MRA, AUC, HLX})
            if (lat.owner[r] != f)
                EXPECT_FALSE(CanSpawnAt(lat, r, f));
}

TEST(TFLattice_IslandOwnershipBlocksSpawn)
{
    // Give MRA a region with no MRA neighbors (deep in the south): owned but
    // chain-broken => not a legal spawn point.
    Lattice lat = InitialLattice();
    ASSERT_TRUE(lat.loaded);
    // Find a non-skyanchor region whose neighbors contain no MRA territory.
    int island = -1;
    for (int r = 0; r < lat.regionCount && island < 0; ++r)
    {
        if (lat.tier[r] == "skyanchor" || lat.owner[r] == MRA)
            continue;
        bool touchesMra = false;
        for (int nb : lat.adj[r])
            touchesMra = touchesMra || lat.owner[nb] == MRA;
        if (!touchesMra)
            island = r;
    }
    EXPECT_GE(island, 0); // the shipped map has such regions (e.g. AUC/HLX rear)
    if (island < 0)
        return;

    lat.owner[island] = MRA;
    EXPECT_FALSE(CanSpawnAt(lat, island, MRA));
}

TEST(TFLattice_CuttingTheChainRevokesSpawn)
{
    // Extend an owned chain by one region, verify spawnable, then flip the
    // middle link to the enemy: the tip must stop being spawnable while the
    // regions still linked home stay valid.
    Lattice lat = InitialLattice();
    ASSERT_TRUE(lat.loaded);

    // Find (anchorSide, mid, tip): mid owned by MRA and conduit-linked to a
    // neutral/enemy region 'tip' that touches no other MRA territory.
    int mid = -1, tip = -1;
    for (int m = 0; m < lat.regionCount && tip < 0; ++m)
    {
        if (lat.owner[m] != MRA || lat.tier[m] == "skyanchor")
            continue;
        for (int t : lat.adj[m])
        {
            if (lat.owner[t] == MRA || lat.tier[t] == "skyanchor")
                continue;
            bool otherMra = false;
            for (int nb : lat.adj[t])
                otherMra = otherMra || (nb != m && lat.owner[nb] == MRA);
            if (!otherMra)
            {
                mid = m;
                tip = t;
                break;
            }
        }
    }
    EXPECT_GE(mid, 0);
    EXPECT_GE(tip, 0);
    if (mid < 0 || tip < 0)
        return;

    lat.owner[tip] = MRA; // captured the tip through mid
    EXPECT_TRUE(CanSpawnAt(lat, tip, MRA));

    lat.owner[mid] = AUC;                    // enemy cuts the chain
    EXPECT_FALSE(CanSpawnAt(lat, tip, MRA)); // tip is now an island
}

// ============================================================================
// Capturability (conduit-link rule)
// ============================================================================

TEST(TFLattice_SkyanchorsAreNeverCapturable)
{
    const Lattice& lat = InitialLattice();
    ASSERT_TRUE(lat.loaded);
    for (int r = 0; r < lat.regionCount; ++r)
    {
        if (lat.tier[r] != "skyanchor")
            continue;
        for (int f : {MRA, AUC, HLX})
            EXPECT_FALSE(IsCapturable(lat, r, f));
    }
}

TEST(TFLattice_OwnRegionsAreNotCapturable)
{
    const Lattice& lat = InitialLattice();
    ASSERT_TRUE(lat.loaded);
    for (int r = 0; r < lat.regionCount; ++r)
        if (lat.owner[r] != None)
            EXPECT_FALSE(IsCapturable(lat, r, lat.owner[r]));
}

TEST(TFLattice_CapturableExactlyWhenConduitLinked)
{
    // Exhaustive: for every region/faction pair, the rule must equal the
    // brute-force definition "some neighbor owned by attacker".
    const Lattice& lat = InitialLattice();
    ASSERT_TRUE(lat.loaded);
    for (int r = 0; r < lat.regionCount; ++r)
    {
        for (int f : {MRA, AUC, HLX})
        {
            bool linked = false;
            for (int nb : lat.adj[r])
                linked = linked || lat.owner[nb] == f;
            const bool expected = linked && lat.owner[r] != f && lat.tier[r] != "skyanchor" && lat.captureSec[r] > 0.0f;
            EXPECT_EQ(IsCapturable(lat, r, f), expected);
        }
    }
}

TEST(TFLattice_OpeningState_EveryFactionHasAnAttackFrontier)
{
    // Day-one map must give all three powers something to fight for.
    const Lattice& lat = InitialLattice();
    ASSERT_TRUE(lat.loaded);
    for (int f : {MRA, AUC, HLX})
    {
        int capturable = 0;
        for (int r = 0; r < lat.regionCount; ++r)
            capturable += IsCapturable(lat, r, f) ? 1 : 0;
        EXPECT_GT(capturable, 0);
    }
}

TEST(TFLattice_DominionShape_LoserStillHasAFoothold)
{
    // Push AUC to total map control (all non-skyanchor regions). The losing
    // factions must still be able to counterattack out of their skyanchors —
    // the lattice may never deadlock the war.
    Lattice lat = InitialLattice();
    ASSERT_TRUE(lat.loaded);
    for (int r = 0; r < lat.regionCount; ++r)
        if (lat.tier[r] != "skyanchor")
            lat.owner[r] = AUC;

    // Nothing left for the dominant faction to take...
    for (int r = 0; r < lat.regionCount; ++r)
        EXPECT_FALSE(IsCapturable(lat, r, AUC));

    // ...but MRA and HLX can each break out next to their own skyanchor.
    for (int f : {MRA, HLX})
    {
        int frontier = 0;
        for (int r = 0; r < lat.regionCount; ++r)
            frontier += IsCapturable(lat, r, f) ? 1 : 0;
        EXPECT_GT(frontier, 0);
    }

    // And every AUC region is spawnable (fully-owned map is fully linked).
    for (int r = 0; r < lat.regionCount; ++r)
        if (lat.owner[r] == AUC && lat.tier[r] != "skyanchor")
            EXPECT_TRUE(CanSpawnAt(lat, r, AUC));
}
