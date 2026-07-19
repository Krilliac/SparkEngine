/**
 * @file TFBotSystem.cpp
 * @brief Server-side bots driving the REAL game paths: faction select + spawn
 *        request pipeline, TF_ClientInput movement through TFServerSim,
 *        TF_FireEvent combat through TFWeaponSystem, TF_VehicleSeatOp driving
 *        through TFVehicleSystem, 8 s respawn loop.
 *
 * Tactics (all server-side, deterministic given the same tick sequence):
 *  - objective bias: contested / actively-capturing enemy regions and own
 *    regions under attack score far better than quiet frontier regions
 *    (TFRegionSystem CaptureProgress), distance-weighted;
 *  - fireteam cohesion: the first alive bot of a faction is the implicit
 *    leader; the rest adopt its objective and regroup when strung out;
 *  - engage-vs-advance: healthy + loaded bots push the target, hurt or
 *    reloading bots give ground while keeping the target in their sights;
 *  - vehicles: when the objective is a long march, enter the driver seat of a
 *    nearby friendly vehicle (TFVehicleSystem::ServerHandleSeatOp) and drive
 *    it there — the normal TF_ClientInput enqueue steers it because
 *    TFServerSim forwards a seated player's inputs to ServerHandleSeatedInput.
 *
 * Region objectives consume the W2 TFRegionSystem contract (OwnerOf /
 * IsCapturable / CaptureProgress) through a compile-time detection shim: the
 * shim keeps this file building green even if those methods change shape in a
 * parallel lane (static regions.json fallback). No behavior change is needed
 * here when they land.
 *
 * Split (bloat threshold): this TU keeps lifecycle, spawn/despawn, loadout
 * and debug UI; the brain/objectives, combat/abilities, navigation, vehicle
 * and chaos method groups live in the TFBotSystemBrain / TFBotSystemCombat /
 * TFBotSystemNav / TFBotSystemVehicle / TFBotSystemChaos siblings, with the
 * shared tuning constants and shims in TFBotSystemInternal.h.
 */
#include "Game/TFBotSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFBotSystemInternal.h"
#include "Game/TFChaosHarness.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFVehicleSystem.h"
#include "Net/TFNetProtocol.h"
#include "Net/TFServerSim.h"
#include "Utils/TFPerfCounters.h" // TF-W13 server-perf lane: AI phase timing

#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace Terrafront
{

    using namespace BotDetail;

    // ---------------------------------------------------------------------------
    // Lifecycle
    // ---------------------------------------------------------------------------

    TFBotSystem::TFBotSystem() = default;
    TFBotSystem::~TFBotSystem()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFBotSystem::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;
        m_bots.reserve(kTFMaxBots);

        // Weapon stats are cached per bot; refresh them on tf_reload_data.
        events.Subscribe<EvDataReloaded>(
            [this](const EvDataReloaded&)
            {
                for (Bot& b : m_bots)
                    ResolveLoadout(b);
            });

        m_initialized = true;

        // Chaos validation harness (bots-chaos lane): kill/flip observers live
        // for the module lifetime, counters reset per chaos run.
        m_chaos = std::make_unique<TFChaosHarness>();
        m_chaos->Initialize(ctx, events);

        // Console wiring inside the owning system (the TFRegionSystem
        // tf_capture_debug pattern) — TFCommands.cpp is a contended file.
        auto& console = Spark::SimpleConsole::GetInstance();
        if (!console.HasCommand("tf_chaos"))
        {
            console.RegisterCommand(
                "tf_chaos",
                [this](const std::vector<std::string>& args) -> std::string
                {
                    if (!m_initialized || !m_ctx)
                        return "[TF] bot system not ready";
                    if (!m_ctx->IsAuthority())
                        return "[TF] tf_chaos: authority only (tf_dedicated / tf_host first)";
                    if (args.empty())
                        return std::string("[TF] chaos ") + (m_chaosActive ? "ACTIVE" : "idle") +
                               "  bots=" + std::to_string(BotCount()) + "  (tf_chaos <0-32> [seconds])";
                    const int n = std::atoi(args[0].c_str());
                    if (n < 0 || n > static_cast<int>(kTFMaxBots))
                        return "[TF] tf_chaos: count must be 0-32";
                    float seconds = 300.0f;
                    if (args.size() > 1)
                        seconds = static_cast<float>(std::atof(args[1].c_str()));
                    ServerStartChaos(static_cast<uint32_t>(n), seconds);
                    if (n == 0)
                        return "[TF] chaos stopped, bots despawned";
                    return "[TF] chaos started: " + std::to_string(n) + " bots, " +
                           std::to_string(static_cast<int>(seconds)) + " s (tf_validate for the report)";
                },
                "Chaos exercise mode: spawn N bots across all factions that scatter onto capture "
                "points, fight, capture, place deployables and try vehicles to stress-test the server",
                "TERRAFRONT", "tf_chaos <botcount 0-32> [seconds=300]");
            m_chaosCmds = true;
        }
        if (!console.HasCommand("tf_validate"))
        {
            console.RegisterCommand(
                "tf_validate", [this](const std::vector<std::string>&) -> std::string { return ValidationReport(); },
                "Print machine-greppable [TF-VALIDATE] PASS/FAIL lines for the last/current chaos "
                "run: physics, collision, capture, kills, terrain",
                "TERRAFRONT", "tf_validate");
            m_chaosCmds = true;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFBotSystem initialized");
        return true;
    }

    void TFBotSystem::Shutdown()
    {
        if (!m_initialized)
            return;
        m_chaosActive = false;
        if (m_chaosCmds)
        {
            auto& console = Spark::SimpleConsole::GetInstance();
            console.UnregisterCommand("tf_chaos");
            console.UnregisterCommand("tf_validate");
            m_chaosCmds = false;
        }
        for (Bot& b : m_bots)
            DespawnBot(b);
        m_bots.clear();
        m_initialized = false;
    }

    double TFBotSystem::Now() const
    {
        return (m_ctx && m_ctx->serverSim) ? m_ctx->serverSim->ServerTime() : m_clock;
    }

    void TFBotSystem::Update(float deltaTime)
    {
        if (!m_initialized || !m_ctx)
            return;
        m_clock += deltaTime;

        if (!m_ctx->IsAuthority())
        {
            // Lost authority (e.g. host became a pure client): bots go away.
            if (!m_bots.empty())
            {
                for (Bot& b : m_bots)
                    DespawnBot(b);
                m_bots.clear();
                SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] no longer authority — all bots despawned");
            }
            return;
        }

        const double now = Now();
        for (Bot& b : m_bots)
        {
            if (now >= b.nextThinkAt)
            {
                b.nextThinkAt = now + kThinkIntervalSec;
                Think(b, now);
            }
        }

        // Chaos run bookkeeping: expire the timer and feed the validation
        // sampler (1 Hz region-progress + below-terrain sweeps).
        if (m_chaosActive)
        {
            if (m_chaos)
                m_chaos->Sample(now);
            if (m_chaosEndsAt > 0.0 && now >= m_chaosEndsAt)
            {
                m_chaosActive = false;
                SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] chaos run complete — run tf_validate for the report");
            }
        }
    }

    void TFBotSystem::FixedUpdate(float fixedDeltaTime)
    {
        Terrafront::TFPerfCounters::ScopedTimer _tfPerf(Terrafront::TFPerfCounters::Phase::AI);
        (void)fixedDeltaTime;
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || !m_ctx->serverSim)
            return;

        // Per-tick path: no allocations — POD input copies + weapon fire only.
        // Seated bots ride the same path: TFServerSim forwards their input to
        // TFVehicleSystem::ServerHandleSeatedInput (driver throttle/steer).
        const double now = Now();
        for (Bot& b : m_bots)
        {
            if (!b.wantMove)
                continue;
            b.input.seq = ++b.seq;
            m_ctx->serverSim->EnqueueInput(b.id, b.input);

            if (b.state == BotState::Fighting && b.targetEntity != 0)
                TryFire(b, now);
        }
    }

    // ---------------------------------------------------------------------------
    // W2 cross-agent contract
    // ---------------------------------------------------------------------------

    void TFBotSystem::ServerSetBotCount(uint32_t n)
    {
        if (!m_initialized || !m_ctx)
            return;
        if (!m_ctx->IsAuthority())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] ServerSetBotCount ignored — not the authority");
            return;
        }

        n = std::min(n, kTFMaxBots);

        while (m_bots.size() > n)
        {
            DespawnBot(m_bots.back());
            m_bots.pop_back();
        }
        while (m_bots.size() < n)
            SpawnBotSlot(static_cast<uint32_t>(m_bots.size()));

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] bot count -> %u", BotCount());
    }

    void TFBotSystem::SpawnBotSlot(uint32_t slot)
    {
        Bot b;
        b.id = kTFBotIdBase + slot;
        b.faction = static_cast<FactionId>(1 + (slot % 3)); // round-robin MRA/AUC/HLX
        b.cls = static_cast<ClassId>(m_rng() % kSelectableClasses);
        b.state = BotState::Deploying;
        b.strafePhase = static_cast<float>(slot) * 1.7f;
        b.feelerPhase = static_cast<uint8_t>(slot & 3u); // desync the 1-in-4 feeler patterns
        const double now = Now();
        b.nextThinkAt = now + static_cast<double>(slot) * 0.031; // stagger the 5 Hz brains
        b.nextSpawnTryAt = now;
        ResolveLoadout(b);
        m_bots.push_back(b);
    }

    void TFBotSystem::DespawnBot(Bot& bot)
    {
        if (!m_ctx || !m_ctx->players)
            return;
        if (m_ctx->vehicles && m_ctx->vehicles->IsSeated(bot.id))
        {
            TF_VehicleSeatOp op{};
            op.vehicleEntity = bot.vehicleEntity;
            m_ctx->vehicles->ServerHandleSeatOp(bot.id, op, false);
        }
        PawnInfo pawn;
        if (m_ctx->players->GetPawnByPlayer(bot.id, pawn) && pawn.alive)
        {
            // Real kill path first so TFServerSim/TFDamageSystem drop their state
            // (EvPlayerKilled), then remove the player record entirely.
            m_ctx->players->ServerKillPawn(pawn.entity, kInvalidPlayer, kInvalidWeapon, false);
        }
        m_ctx->players->ServerHandlePlayerDisconnect(bot.id);
        bot.wantMove = false;
    }

    // ---------------------------------------------------------------------------
    // Loadout
    // ---------------------------------------------------------------------------

    void TFBotSystem::ResolveLoadout(Bot& bot) const
    {
        bot.weapon = kInvalidWeapon;
        if (!m_ctx || !m_ctx->data || !m_ctx->data->IsLoaded())
            return;

        // Faction rifle first; any faction/common shootable primary as fallback.
        const WeaponDef* pick = nullptr;
        for (const WeaponDef& w : m_ctx->data->AllWeapons())
        {
            if (w.kind == "melee" || w.kind == "beam")
                continue;
            if (w.faction == bot.faction && w.slot == "rifle")
            {
                pick = &w;
                break;
            }
            if (!pick && (w.faction == bot.faction || w.faction == FactionId::None) &&
                (w.slot == "rifle" || w.slot == "carbine" || w.slot == "lmg"))
                pick = &w;
        }
        if (!pick)
            return;

        const WeaponDef def = m_ctx->data->ResolveWeapon(pick->id, bot.faction);
        bot.weapon = def.id;
        bot.rofIntervalSec = 60.0f / std::max(1.0f, def.rofRpm);
        bot.magSize = std::max(1, def.magSize);
        bot.reloadSec = std::max(0.5f, def.reloadSec);
        bot.magLeft = bot.magSize;
    }

    // ---------------------------------------------------------------------------
    // Spawn / respawn (the real pipeline)
    // ---------------------------------------------------------------------------

    void TFBotSystem::TrySpawn(Bot& bot, double now)
    {
        bot.nextSpawnTryAt = now + kSpawnRetrySec;
        if (!m_ctx->players || !m_ctx->data || !m_ctx->data->IsLoaded())
            return;

        // Faction select through both registries, exactly like the network path
        // (TFServerSim::HandleFactionSelect -> SetPlayerFaction; TFPlayerSystem
        // accepts either registry in ServerHandleSpawnRequest).
        if (m_ctx->serverSim)
            m_ctx->serverSim->SetPlayerFaction(bot.id, bot.faction);
        m_ctx->players->ServerHandleFactionSelect(bot.id, bot.faction);

        if (bot.weapon == kInvalidWeapon)
            ResolveLoadout(bot);

        // Skyanchor spawn request — the same validation + ServerSpawnPawn +
        // TF_SpawnReply flow clients use. The reply send for a bot id is a safe
        // no-op (verified: NetworkManager::SendToClient drops unknown client ids;
        // NetRole::Standalone skips the send before reaching NetworkManager).
        TF_SpawnRequest req{};
        req.classId = static_cast<uint8_t>(bot.cls);
        req.spawnKind = 0; // skyanchor
        m_ctx->players->ServerHandleSpawnRequest(bot.id, req);
        ++m_spawnRequests;
        // Success is observed on the next Think() via GetPawnByPlayer().alive.
    }

    // ---------------------------------------------------------------------------
    // Debug UI
    // ---------------------------------------------------------------------------

    const char* TFBotSystem::StateName(BotState s)
    {
        switch (s)
        {
        case BotState::Deploying:
            return "deploying";
        case BotState::Moving:
            return "moving";
        case BotState::ToVehicle:
            return "toVehicle";
        case BotState::Driving:
            return "DRIVING";
        case BotState::Fighting:
            return "FIGHTING";
        case BotState::Dead:
            return "dead";
        }
        return "?";
    }

    std::string TFBotSystem::DebugSummary() const
    {
        std::ostringstream os;
        os << "[TF] bots " << m_bots.size() << "  spawnReqs " << m_spawnRequests << "  shots " << m_shotsFired
           << "  chaos " << (m_chaosActive ? "ON" : "off") << " (deployTries " << m_chaosDeployTries << ", vehTries "
           << m_chaosVehicleTries << ", vehBuys " << m_chaosVehiclePurchases << ")  abilityUses " << m_abilityUses
           << "  nav(blockedFeelers " << m_feelerBlocked << ", unsticks " << m_unsticks << ", stuckTeleports "
           << m_stuckTeleports << ")  now " << Now();
        for (const Bot& b : m_bots)
        {
            PawnInfo p{};
            const bool have = m_ctx && m_ctx->players && m_ctx->players->GetPawnByPlayer(b.id, p);
            os << "\n  p" << b.id << " " << FactionTag(b.faction) << " " << StateName(b.state)
               << " pawn=" << (have ? (p.alive ? "alive" : "dead") : "NONE");
            if (have)
                os << " pos(" << static_cast<int>(p.pos[0]) << "," << static_cast<int>(p.pos[1]) << ","
                   << static_cast<int>(p.pos[2]) << ") hp=" << static_cast<int>(p.health);
            os << " obj=r" << (b.objectiveRegion == kInvalidRegion ? -1 : static_cast<int>(b.objectiveRegion))
               << " tgt=" << b.targetEntity << " veh=" << b.vehicleEntity
               << " us=" << static_cast<unsigned>(b.unstickCount) << " nextSpawnTry=" << b.nextSpawnTryAt;
        }
        return os.str();
    }

    void TFBotSystem::RenderDebugUI()
    {
#ifdef SPARK_HAS_IMGUI
        if (!m_showDebug)
            return;
        if (ImGui::Begin("TF Bots", &m_showDebug))
        {
            ImGui::Text("bots: %u / %u   shots fired: %u   spawn requests: %u", BotCount(), kTFMaxBots, m_shotsFired,
                        m_spawnRequests);
            ImGui::Text("nav: blocked feelers %u   unsticks %u   stuck teleports %u", m_feelerBlocked, m_unsticks,
                        m_stuckTeleports);
            ImGui::Separator();
            for (const Bot& b : m_bots)
            {
                const char* clsName = "?";
                if (m_ctx && m_ctx->data)
                    if (const ClassDef* cd = m_ctx->data->GetClass(b.cls))
                        clsName = cd->name.c_str();
                ImGui::Text("0x%08X %s %-10s %-9s obj=r%u (%.0f,%.0f) tgt=%u veh=%u mag=%d%s", b.id,
                            FactionTag(b.faction), clsName, StateName(b.state),
                            b.objectiveRegion == kInvalidRegion ? 0u : static_cast<unsigned>(b.objectiveRegion),
                            b.objectiveX, b.objectiveZ, b.targetEntity, b.vehicleEntity, b.magLeft,
                            b.lowHealth ? " LOW" : "");
            }
        }
        ImGui::End();
#endif // SPARK_HAS_IMGUI
    }

} // namespace Terrafront
