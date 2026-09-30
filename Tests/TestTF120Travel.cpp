/**
 * @file TestTF120Travel.cpp
 * @brief TF-120: a pawn carried from one continent authority process to another keeps its physical state.
 *
 * Each continent authority simulates its own analytic ground. These tests load the REAL Cindral Wastes and
 * Veyra Highlands terrain (the scenes' [Terrain] keys and region tables, through the production
 * TFTerrainModel), move the pawn with the production movement step (TFMovementModel.h) and terrain backstop,
 * and carry it through the production TFHandoffParticipant pair over a real TFDatabase file. The source
 * continent is a separate SparkTests process (TF120PeerProcess.h), so the checkpoint crosses a process boundary
 * exactly as it does between two SparkServer processes.
 *
 * Test doubles and limits: the pawn store is a plain move state in each process (TFServerSim is not linked into
 * SparkTests); its capture/arrival rules are the production TFHandoffContinuity.h helpers TFServerSim calls.
 * Jolt static bodies are not built here; the sanctuary's bodies come from the same sanctuary_haven.scene on
 * every continent. No renderer, client, gateway or SparkServer process is involved, so this proves the
 * authoritative physical continuity only, not client presentation.
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "TestFramework.h"

#include "Data/TFDataTables.h"
#include "Data/TFDataTablesInternal.h"
#include "Game/TFMovementModel.h"
#include "Net/TFHandoffContinuity.h"
#include "Net/TFHandoffParticipant.h"
#include "Persistence/TFDatabase.h"
#include "TF120PeerProcess.h"
#include "Utils/JsonUtils.h"
#include "World/TFSanctuaryZone.h"
#include "World/TFTerrainModel.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace Terrafront;
using Spark::Net::AreaID;
using Spark::Net::HandoffRequest;
using Spark::Net::HandoffResult;

namespace
{
    namespace fs = std::filesystem;

    constexpr const char* kCindral = "cindral_wastes";
    constexpr const char* kVeyra = "veyra_highlands";
    constexpr const char* kPeerRoleSource = "travel-source";
    constexpr const char* kPeerPrepared = "TF120_TRAVEL_PREPARED=";
    constexpr const char* kPeerReady = "TF120_TRAVEL_READY=";
    constexpr const char* kPeerRef = "TF120_TRAVEL_REF ";

    constexpr float kDt = 1.0f / 60.0f; // the fixed step both roles run
    constexpr float kRunSpeed = 6.0f;
    constexpr float kSprintSpeed = 7.8f;
    constexpr int kJumpTick = 80;      // leaves the pad ...
    constexpr int kCaptureTick = 84;   // ... and is carried while still rising
    constexpr int kContinueTicks = 90; // lands on the destination and keeps running
    constexpr float kTolerance = 1.0e-4f;

    /// Start on spawn pad 0, sprinting toward the travel terminal.
    constexpr float kStartX = kTFSanctuarySpawnPads[0][0];
    constexpr float kStartZ = kTFSanctuarySpawnPads[0][1];
    const float kRunYaw = std::atan2(kTFSanctuaryTerminalX - kStartX, kTFSanctuaryTerminalZ - kStartZ);

    const fs::path& RepoRoot()
    {
        static const fs::path root = []() -> fs::path
        {
            fs::path p = fs::current_path();
            for (int i = 0; i < 10; ++i)
            {
                if (fs::exists(p / "Assets" / "MMOFPS" / "Data" / "continents.json"))
                {
                    return p;
                }
                const fs::path parent = p.parent_path();
                if (parent.empty() || parent == p)
                {
                    break;
                }
                p = parent;
            }
            return {};
        }();
        return root;
    }

    /// One continent authority's ground, loaded from the shipped data exactly as TFWorldSetup loads it.
    struct ContinentGround
    {
        TFTerrainParams terrain;
        ContinentDef continent;
        bool loaded = false;

        float Height(float x, float z) const { return TFTerrainHeightAt(terrain, &continent.regions, x, z); }

        /// The destination's post-move hook minus Jolt bodies: TFWorldSetup::ResolveMoveCollision step 2.
        void Resolve(const float* /*prev*/, float pos[3], float vel[3], bool* /*grounded*/) const
        {
            TFApplyTerrainBackstop(Height(pos[0], pos[2]), pos, vel);
        }
    };

    ContinentGround LoadGround(const char* regionsFile)
    {
        ContinentGround ground;
        std::ifstream file(RepoRoot() / "Assets" / "MMOFPS" / "Data" / regionsFile, std::ios::binary);
        std::ostringstream text;
        text << file.rdbuf();
        std::string error;
        if (!DataTablesDetail::ParseRegions(Spark::Json::Parse(text.str()), ground.continent, error) ||
            ground.continent.scene.empty())
        {
            return ground;
        }
        const fs::path scene = RepoRoot() / "Assets" / ground.continent.scene;
        ground.loaded = TFLoadTerrainParams(scene.string(), ground.terrain);
        return ground;
    }

    const ContinentGround& Cindral()
    {
        static const ContinentGround ground = LoadGround("regions.json");
        return ground;
    }

    const ContinentGround& Veyra()
    {
        static const ContinentGround ground = LoadGround("regions_highlands.json");
        return ground;
    }

    TFMoveInput InputAt(int tick)
    {
        TFMoveInput input;
        input.moveY = 1.0f;
        input.yaw = kRunYaw;
        input.sprint = true;
        input.jump = tick == kJumpTick;
        return input;
    }

    /// One authoritative tick: the shared movement step, then the world's post-move resolve.
    void Tick(const ContinentGround& ground, TFMoveState& state, int tick)
    {
        const float prev[3] = {state.pos[0], state.pos[1], state.pos[2]};
        TFMoveStep(state, InputAt(tick), kRunSpeed, kSprintSpeed, kDt,
                   [&ground](float x, float z) { return ground.Height(x, z); });
        ground.Resolve(prev, state.pos, state.vel, &state.grounded);
    }

    TFMoveState StandOnSpawnPad(const ContinentGround& ground)
    {
        TFMoveState state;
        state.pos[0] = kStartX;
        state.pos[2] = kStartZ;
        state.pos[1] = ground.Height(kStartX, kStartZ);
        state.grounded = true;
        return state;
    }

    bool ResolveArea(AreaID area, std::string& key)
    {
        if (area == 1)
        {
            key = kCindral;
            return true;
        }
        if (area == 2)
        {
            key = kVeyra;
            return true;
        }
        return false;
    }

    HandoffRequest TravelRequest(uint64_t character)
    {
        HandoffRequest request;
        request.sessionId = TFHandoffParticipant::SessionId(character);
        request.epoch = 1;
        request.sourceArea = 1;
        request.targetArea = 2;
        return request;
    }

    /// Source authority: its live pawn is @p pawn; capture applies the production departure gate.
    struct SourceAuthority final : TFHandoffParticipant::IAuthority
    {
        explicit SourceAuthority(const TFMoveState& pawn, uint32_t sequence) : m_pawn(pawn), m_sequence(sequence) {}

        bool ResolveContinent(AreaID area, std::string& key) const override { return ResolveArea(area, key); }

        bool Capture(uint64_t /*character*/, TFHandoffState& state) override
        {
            state.player = 7;
            state.cls = ClassId::Striker;
            std::copy_n(m_pawn.pos, 3, state.position);
            std::copy_n(m_pawn.vel, 3, state.velocity);
            state.yaw = kRunYaw;
            state.pitch = -0.125f;
            state.health = 87.5f;
            state.shield = 12.25f;
            state.lastSequence = m_sequence;
            state.grounded = m_pawn.grounded;
            ++captures;
            return TFHandoff_CanCarry(state);
        }

        bool CanInstall(uint64_t, const TFHandoffState& state) const override { return TFHandoff_CanCarry(state); }

        bool Suspend(uint64_t) override
        {
            ++suspends;
            return true;
        }

        bool Install(const TFCharacterRecord&, const TFHandoffState&) override { return false; }

        void Retire(uint64_t) override {}

        int captures = 0;
        int suspends = 0;

      private:
        TFMoveState m_pawn;
        uint32_t m_sequence;
    };

    /// Destination authority on @p ground: accepts only what the production gate accepts and places the pawn
    /// with the production arrival helper.
    struct DestinationAuthority final : TFHandoffParticipant::IAuthority
    {
        explicit DestinationAuthority(const ContinentGround& ground) : m_ground(ground) {}

        bool ResolveContinent(AreaID area, std::string& key) const override { return ResolveArea(area, key); }

        bool Capture(uint64_t, TFHandoffState&) override { return false; }

        bool CanInstall(uint64_t, const TFHandoffState& state) const override
        {
            return m_ground.loaded && TFHandoff_CanCarry(state);
        }

        bool Suspend(uint64_t) override { return true; }

        bool Install(const TFCharacterRecord&, const TFHandoffState& state) override
        {
            if (!CanInstall(0, state))
            {
                return false;
            }
            const TFHandoffArrival arrival =
                TFHandoff_Arrive(state, [this](const float prev[3], float pos[3], float vel[3], bool* grounded)
                                 { m_ground.Resolve(prev, pos, vel, grounded); });
            installed = state;
            placed = arrival;
            ++installs;
            return true;
        }

        void Retire(uint64_t) override {}

        std::optional<TFHandoffState> installed;
        TFHandoffArrival placed;
        int installs = 0;

      private:
        const ContinentGround& m_ground;
    };

    fs::path FreshTravelDb(const char* name)
    {
        const fs::path path = fs::absolute(fs::path("Saves") / name);
        fs::create_directories(path.parent_path());
        fs::remove(path);
        fs::remove(path.string() + ".lock");
        fs::remove(path.string() + ".tmp");
        fs::remove(path.string() + ".authority." + kCindral + ".lock");
        fs::remove(path.string() + ".authority." + kVeyra + ".lock");
        return path;
    }

    uint64_t SeedTraveller(const fs::path& path, const char* user)
    {
        TFDatabase db;
        TFAccountRecord account;
        TFCharacterRecord character;
        if (!db.Open(path) || !db.CreateAccount(user, "salt", "hash", account) ||
            !db.CreateCharacter(account.id, "Traveller", FactionId::MRA, character) || !db.Close())
        {
            return 0;
        }
        return character.id;
    }

    struct Sample
    {
        float pos[3]{};
        float vel[3]{};
        bool grounded = false;
    };

    /// The source's own continuation of the pawn, reported line by line by the peer.
    std::vector<Sample> ParseReference(const std::string& log)
    {
        std::vector<Sample> samples;
        std::istringstream lines(log);
        std::string line;
        while (std::getline(lines, line))
        {
            if (!line.starts_with(kPeerRef))
            {
                continue;
            }
            std::istringstream fields(line.substr(std::char_traits<char>::length(kPeerRef)));
            int index = -1;
            int grounded = 0;
            Sample sample;
            fields >> index >> sample.pos[0] >> sample.pos[1] >> sample.pos[2] >> sample.vel[0] >> sample.vel[1] >>
                sample.vel[2] >> grounded;
            if (!fields || index != static_cast<int>(samples.size()))
            {
                return {};
            }
            sample.grounded = grounded != 0;
            samples.push_back(sample);
        }
        return samples;
    }

    /// Peer role: the Cindral Wastes authority. It walks the pawn, reserves the handoff through the production
    /// participant, reports how the pawn would have continued on its own ground, and holds the continent until
    /// the coordinator kills it.
    void RunTravelSourcePeer()
    {
        const fs::path path = TF120Peer::EnvOrEmpty(TF120Peer::kDbEnv);
        const uint64_t character = std::strtoull(TF120Peer::EnvOrEmpty(TF120Peer::kCharEnv).c_str(), nullptr, 10);
        ASSERT_TRUE(Cindral().loaded);
        TFDatabase db;
        ASSERT_TRUE(character != 0 && db.Open(path) && db.BindAuthority(kCindral));
        TFCharacterRecord row;
        ASSERT_TRUE(db.ClaimCharacter(character, row));

        TFMoveState pawn = StandOnSpawnPad(Cindral());
        for (int tick = 0; tick < kCaptureTick; ++tick)
        {
            Tick(Cindral(), pawn, tick);
        }
        SourceAuthority authority(pawn, kCaptureTick);
        TFHandoffParticipant source(db, authority);
        const HandoffResult prepared = source.Prepare(TravelRequest(character));
        std::printf("%s%d\n", kPeerPrepared, prepared == HandoffResult::Applied && authority.suspends == 1 ? 1 : 0);

        for (int i = 0; i < kContinueTicks; ++i)
        {
            Tick(Cindral(), pawn, kCaptureTick + i);
            std::printf("%s%d %.9g %.9g %.9g %.9g %.9g %.9g %d\n", kPeerRef, i, pawn.pos[0], pawn.pos[1], pawn.pos[2],
                        pawn.vel[0], pawn.vel[1], pawn.vel[2], pawn.grounded ? 1 : 0);
        }
        std::printf("%s1\n", kPeerReady);
        std::fflush(stdout);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        while (std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    bool Near(float a, float b)
    {
        return std::fabs(a - b) <= kTolerance;
    }
} // namespace

TEST(TF120_Travel_RunningJumpCarriedAcrossProcessesKeepsPoseVelocityAndGround)
{
    if (TF120Peer::EnvOrEmpty(TF120Peer::kRoleEnv) == kPeerRoleSource)
    {
        RunTravelSourcePeer();
        return;
    }
    ASSERT_FALSE(RepoRoot().empty());
    ASSERT_TRUE(Cindral().loaded && Veyra().loaded);
    // The two authorities really run different ground: off the pad they disagree by meters.
    EXPECT_TRUE(std::fabs(Cindral().Height(kTFSanctuaryMinX + 20.0f, kTFSanctuaryMinZ + 24.0f) -
                          Veyra().Height(kTFSanctuaryMinX + 20.0f, kTFSanctuaryMinZ + 24.0f)) > 1.0f);

    const fs::path path = FreshTravelDb("test_tf120_travel_process.db");
    const uint64_t character = SeedTraveller(path, "traveller-process");
    ASSERT_TRUE(character != 0);

    auto launched = TF120Peer::SpawnPeer("TF120_Travel_RunningJumpCarriedAcrossProcessesKeepsPoseVelocityAndGround",
                                         {std::string(TF120Peer::kRoleEnv) + "=" + kPeerRoleSource,
                                          std::string(TF120Peer::kDbEnv) + "=" + path.string(),
                                          std::string(TF120Peer::kCharEnv) + "=" + std::to_string(character)});
    if (!launched)
    {
        // Not a skip: without the second process this case proves nothing about a cross-process handoff.
        std::fprintf(stderr, "cannot spawn a peer test process: %s\n", launched.error().c_str());
    }
    ASSERT_TRUE(launched.has_value());
    Spark::Process& sourceProcess = *launched;
    std::string log;
    const bool ready = TF120Peer::WaitPeerReady(sourceProcess, log, kPeerReady, std::chrono::seconds(60));
    if (!ready)
    {
        std::fprintf(stderr, "---- TF120 travel source output ----\n%s\n----\n", log.c_str());
    }
    ASSERT_TRUE(ready);
    ASSERT_EQ(TF120Peer::PeerReport(log, kPeerPrepared), 1LL);
    const std::vector<Sample> reference = ParseReference(log);
    ASSERT_EQ(reference.size(), static_cast<size_t>(kContinueTicks));

    // Veyra Highlands authority (this process) completes the fenced handoff while the source still lives.
    TFDatabase db;
    ASSERT_TRUE(db.Open(path) && db.BindAuthority(kVeyra));
    DestinationAuthority authority(Veyra());
    TFHandoffParticipant destination(db, authority);
    const HandoffRequest request = TravelRequest(character);
    EXPECT_TRUE(destination.Prepare(request) == HandoffResult::Applied);
    EXPECT_TRUE(destination.Transfer(request) == HandoffResult::Applied);
    EXPECT_TRUE(destination.Commit(request) == HandoffResult::Applied);
    ASSERT_EQ(authority.installs, 1);
    ASSERT_TRUE(authority.installed.has_value());
    const TFHandoffState& carried = *authority.installed;

    // The checkpoint really is mid-stride and mid-air, so continuity is not trivially a standing pawn.
    const float speed =
        std::sqrt(carried.velocity[0] * carried.velocity[0] + carried.velocity[2] * carried.velocity[2]);
    EXPECT_TRUE(speed > 5.0f);
    EXPECT_TRUE(carried.velocity[1] > 1.0f);
    EXPECT_FALSE(carried.grounded);
    EXPECT_TRUE(TFTravel_IsOnSharedPad(carried.position[0], carried.position[2]));
    EXPECT_EQ(carried.lastSequence, static_cast<uint32_t>(kCaptureTick));
    EXPECT_EQ(carried.yaw, kRunYaw);
    EXPECT_EQ(carried.pitch, -0.125f);
    EXPECT_EQ(carried.health, 87.5f);
    EXPECT_EQ(carried.shield, 12.25f);

    // Arrival: the destination's own ground leaves pose, velocity and contact state untouched.
    const TFHandoffArrival& placed = authority.placed;
    for (int axis = 0; axis < 3; ++axis)
    {
        EXPECT_EQ(placed.position[axis], carried.position[axis]);
        EXPECT_EQ(placed.velocity[axis], carried.velocity[axis]);
    }
    EXPECT_EQ(placed.grounded, carried.grounded);
    EXPECT_TRUE(placed.position[1] > Veyra().Height(placed.position[0], placed.position[2]));

    // The destination continues the jump on its ground; the source reported the same inputs on its own. They
    // must agree tick for tick: same landing tick, no pop, no fall-through, grounded after landing.
    TFMoveState pawn;
    std::copy_n(placed.position, 3, pawn.pos);
    std::copy_n(placed.velocity, 3, pawn.vel);
    pawn.grounded = placed.grounded;
    int landedAt = -1;
    int mismatches = 0;
    float lowest = 1.0e9f;
    for (int i = 0; i < kContinueTicks; ++i)
    {
        Tick(Veyra(), pawn, kCaptureTick + i);
        const Sample& expected = reference[static_cast<size_t>(i)];
        bool same = pawn.grounded == expected.grounded;
        for (int axis = 0; axis < 3; ++axis)
        {
            same = same && Near(pawn.pos[axis], expected.pos[axis]) && Near(pawn.vel[axis], expected.vel[axis]);
        }
        if (!same && mismatches++ == 0)
        {
            std::fprintf(stderr,
                         "[TF120 travel] tick %d: veyra (%.6f %.6f %.6f | %.6f %.6f %.6f | %d) cindral (%.6f "
                         "%.6f %.6f | %.6f %.6f %.6f | %d)\n",
                         i, pawn.pos[0], pawn.pos[1], pawn.pos[2], pawn.vel[0], pawn.vel[1], pawn.vel[2],
                         pawn.grounded ? 1 : 0, expected.pos[0], expected.pos[1], expected.pos[2], expected.vel[0],
                         expected.vel[1], expected.vel[2], expected.grounded ? 1 : 0);
        }
        lowest = std::min(lowest, pawn.pos[1] - Veyra().Height(pawn.pos[0], pawn.pos[2]));
        if (pawn.grounded && landedAt < 0)
        {
            landedAt = i;
        }
        if (landedAt >= 0)
        {
            EXPECT_TRUE(pawn.grounded);
        }
    }
    std::printf("[TF120 travel] carried at tick %d speed %.3f vy %.3f; landed %d ticks after arrival; lowest %.6f m "
                "above ground; %d mismatching ticks\n",
                kCaptureTick, speed, carried.velocity[1], landedAt, lowest, mismatches);
    EXPECT_EQ(mismatches, 0);
    EXPECT_TRUE(landedAt > 0 && landedAt < kContinueTicks - 10);
    EXPECT_TRUE(lowest >= -kTolerance);
    EXPECT_TRUE(TFTravel_IsOnSharedPad(pawn.pos[0], pawn.pos[2]));

    sourceProcess.Kill();
    EXPECT_FALSE(sourceProcess.IsRunning());
    TFCharacterRecord row;
    ASSERT_TRUE(db.FindCharacter(character, row));
    EXPECT_EQ(row.residentContinent, std::string(kVeyra));
    EXPECT_TRUE(row.migrationOperation.empty());
    EXPECT_TRUE(db.Close());
    fs::remove(path);
}

TEST(TF120_Travel_OffPadCheckpointNeverLeavesOrEntersEitherContinent)
{
    ASSERT_FALSE(RepoRoot().empty());
    ASSERT_TRUE(Cindral().loaded && Veyra().loaded);

    // A pawn standing in the sanctuary rectangle but off the pad stands on its continent's own ground.
    constexpr float kCornerX = kTFSanctuaryMinX + 20.0f;
    constexpr float kCornerZ = kTFSanctuaryMinZ + 24.0f;
    ASSERT_TRUE(TFTravel_IsInSanctuary(kCornerX, kCornerZ));
    ASSERT_FALSE(TFTravel_IsOnSharedPad(kCornerX, kCornerZ));
    const float cindralGround = Cindral().Height(kCornerX, kCornerZ);
    const float veyraGround = Veyra().Height(kCornerX, kCornerZ);
    std::printf("[TF120 travel] sanctuary corner ground: cindral %.3f m, veyra %.3f m\n", cindralGround, veyraGround);
    ASSERT_TRUE(std::fabs(cindralGround - veyraGround) > 1.0f);

    TFMoveState corner;
    corner.pos[0] = kCornerX;
    corner.pos[1] = cindralGround;
    corner.pos[2] = kCornerZ;
    corner.grounded = true;

    const fs::path path = FreshTravelDb("test_tf120_travel_offpad.db");
    const uint64_t character = SeedTraveller(path, "traveller-offpad");
    ASSERT_TRUE(character != 0);
    TFDatabase sourceDb;
    TFDatabase destinationDb;
    ASSERT_TRUE(sourceDb.Open(path) && sourceDb.BindAuthority(kCindral));
    ASSERT_TRUE(destinationDb.Open(path) && destinationDb.BindAuthority(kVeyra));
    TFCharacterRecord row;
    ASSERT_TRUE(sourceDb.ClaimCharacter(character, row));

    SourceAuthority sourceAuthority(corner, 3);
    DestinationAuthority destinationAuthority(Veyra());
    TFHandoffParticipant source(sourceDb, sourceAuthority);
    TFHandoffParticipant destination(destinationDb, destinationAuthority);
    const HandoffRequest request = TravelRequest(character);

    // The source refuses to reserve: nothing is written and the pawn stays in world where it stands.
    EXPECT_TRUE(source.Prepare(request) == HandoffResult::Rejected);
    EXPECT_EQ(sourceAuthority.captures, 1);
    EXPECT_EQ(sourceAuthority.suspends, 0);
    ASSERT_TRUE(sourceDb.FindCharacter(character, row));
    EXPECT_EQ(row.residentContinent, std::string(kCindral));
    EXPECT_TRUE(row.migrationOperation.empty());
    EXPECT_TRUE(destination.Commit(request) == HandoffResult::Rejected);
    EXPECT_EQ(destinationAuthority.installs, 0);

    // A reservation that carries an off-pad checkpoint anyway (older source, tooling) is refused by the
    // destination at every phase, so the pawn never appears on ground it was not standing on.
    TFHandoffState offPad;
    offPad.player = 7;
    offPad.cls = ClassId::Striker;
    std::copy_n(corner.pos, 3, offPad.position);
    offPad.health = 100.0f;
    offPad.grounded = true;
    const std::string operation = request.sessionId + "/" + std::to_string(request.epoch);
    ASSERT_TRUE(sourceDb.ReserveMigration(character, operation, request.epoch, kVeyra, offPad.Encode(), row));
    EXPECT_TRUE(destination.Prepare(request) == HandoffResult::Unavailable);
    EXPECT_TRUE(destination.Transfer(request) == HandoffResult::Unavailable);
    EXPECT_TRUE(destination.Commit(request) == HandoffResult::Unavailable);
    EXPECT_EQ(destinationAuthority.installs, 0);
    ASSERT_TRUE(destinationDb.FindCharacter(character, row));
    EXPECT_EQ(row.residentContinent, std::string(kCindral));

    EXPECT_TRUE(destinationDb.Close());
    EXPECT_TRUE(sourceDb.Close());
    fs::remove(path);
}

TEST(TF120_Travel_UnreadableSceneLeavesTerrainUnloaded)
{
    ASSERT_FALSE(RepoRoot().empty());
    TFTerrainParams params;
    params.baseHeight = -3.0f;
    EXPECT_FALSE(TFLoadTerrainParams((RepoRoot() / "Assets" / "Scenes" / "MMOFPS" / "missing.scene").string(), params));
    EXPECT_EQ(params.baseHeight, -3.0f);

    // Each continent's own [Terrain] keys, not the built-in defaults, define its ground.
    ASSERT_TRUE(Cindral().loaded && Veyra().loaded);
    EXPECT_EQ(Cindral().terrain.baseHeight, 8.0f);
    EXPECT_EQ(Veyra().terrain.baseHeight, 26.0f);
    EXPECT_EQ(Veyra().terrain.canyonDepth, 30.0f);
    EXPECT_EQ(Cindral().continent.regions.size(), Veyra().continent.regions.size());
    EXPECT_FALSE(Veyra().continent.regions.empty());
}
