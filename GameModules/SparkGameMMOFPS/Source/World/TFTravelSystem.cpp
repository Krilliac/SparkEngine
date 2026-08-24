/**
 * @file TFTravelSystem.cpp
 * @brief Continent/sanctuary travel — lifecycle, continents.json display
 *        metadata, console commands, cross-lane surface and frame driving.
 *        The server placement/validation half, client terminal flow, wire
 *        handlers and the ImGui terminal live in the TFTravelSystemServer/
 *        -Client/-Net/-Ui split parts (same class, split per the repo
 *        file-size rules — mirrors the TFVehicleSystem split); shared
 *        internals live in TFTravelSystemInternal.h.
 *
 * See TFTravelSystem.h / TFSanctuaryZone.h for the architecture note (single
 * shared sim, positional mapId). Server rules enforced by this system:
 *  - first spawn of an entered-world session lands on a sanctuary pad
 *    (deferred one fixed tick — EvPlayerSpawned fires synchronously INSIDE
 *    the spawn handlers, which overwrite MoveState after the event returns,
 *    so an immediate TeleportPawn would be clobbered);
 *  - sanctuary -> continent requires standing at the travel terminal;
 *  - continent -> sanctuary is an unrestricted recall (PS2 style);
 *  - every travel teleports through TFServerSim::TeleportPawn (the one
 *    authoritative move primitive; prediction reconciles the snap client-side).
 */
#include "World/TFTravelSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFRedeployRules.h"   // ui-map-keys seam: SetExtraRule (sanctioned extension point)
#include "Game/TFTargetRange.h"     // sanctuary-v2 lane (W10): cosmetic firing range
#include "Game/TFTutorial.h"        // tutorial-flow lane (W12): first-join guided flow
#include "Game/TFUiSounds.h"        // W10 audio-wave-2: terminal bleeps
#include "Net/TFRedeployProtocol.h" // kTFRedeployBlocked reason code
#include "UI/TFMapScreen.h"
#include "UI/TFSpawnScreen.h"
#include "World/TFRegionSystem.h"
#include "World/TFSanctuaryDecor.h" // sanctuary-v2 lane (W10): decor + class terminal

#include "Input/InputManager.h"
#include "Spark/IEngineContext.h"
#include "Utils/JsonUtils.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace Terrafront
{

    namespace
    {

        constexpr float kInteractDebounceSec = 0.25f;
        constexpr float kMenuAutoCloseM = 20.0f; ///< walk away -> menu closes
        constexpr double kInfoRefreshSec = 2.0;  ///< menu population refresh cadence
        constexpr const char* kContinentsJson = "Assets/MMOFPS/Data/continents.json";

    } // namespace

    TFTravelSystem::TFTravelSystem() = default;
    TFTravelSystem::~TFTravelSystem()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFTravelSystem::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;

        events.Subscribe<EvPlayerSpawned>([this](const EvPlayerSpawned& ev) { OnPlayerSpawned(ev); });

        // Sanctuary redeploy veto — installed into the ui-map-keys lane's
        // sanctioned extension point (TFRedeployRules.h names this lane as the
        // extender). Deliberately CAPTURE-LESS: the rule outliving this system
        // can never dangle. Runs on BOTH sides after the baseline friendly +
        // non-contested checks, so this only adds the sanctuary rule.
        TFRedeployRules::SetExtraRule(
            [](const TFGameContext& ctx, PlayerId player, RegionId /*region*/) -> uint8_t
            {
                PawnInfo pawn{};
                if (ctx.players && ctx.players->GetPawnByPlayer(player, pawn) && pawn.alive &&
                    TFTravel_IsInSanctuary(pawn.pos[0], pawn.pos[2]))
                    return kTFRedeployBlocked; // use the travel terminal instead
                return kTFRedeployOk;
            });

        LoadContinentMeta();
        RegisterConsoleCommands();

        // sanctuary-v2 lane (W10): sanctuary decor/class terminal + firing
        // range live under this system (TFRegionSystem::m_decor precedent —
        // no contended Main.cpp wiring). Both no-op on dedicated servers.
        m_sanctuaryDecor = std::make_unique<TFSanctuaryDecor>();
        m_sanctuaryDecor->Initialize(ctx);
        m_targetRange = std::make_unique<TFTargetRange>();
        m_targetRange->Initialize(ctx);

        // tutorial-flow lane (W12): first-join guided flow, owned here like
        // the decor/range above. AttachRange installs the read-only dummy-hit
        // observer for the firing-range step.
        m_tutorial = std::make_unique<TFTutorial>();
        m_tutorial->Initialize(ctx);
        m_tutorial->AttachRange(m_targetRange.get());

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game,
                       "[TF] TFTravelSystem initialized (sanctuary pad %.0f/%.0f, terminal %.0f/%.0f)",
                       kTFSanctuaryCenterX, kTFSanctuaryCenterZ, kTFSanctuaryTerminalX, kTFSanctuaryTerminalZ);
        return true;
    }

    void TFTravelSystem::Shutdown()
    {
        if (!m_initialized)
            return;
        // tutorial-flow lane (W12): the tutorial detaches its TFTargetRange
        // hit observer in Shutdown, so it must go down BEFORE the range.
        if (m_tutorial)
        {
            m_tutorial->Shutdown();
            m_tutorial.reset();
        }
        // sanctuary-v2 lane (W10): tear down owned subsystems first (the range
        // releases its TFWeaponSystem hook and both destroy their entities).
        if (m_targetRange)
        {
            m_targetRange->Shutdown();
            m_targetRange.reset();
        }
        if (m_sanctuaryDecor)
        {
            m_sanctuaryDecor->Shutdown();
            m_sanctuaryDecor.reset();
        }
        auto& console = Spark::SimpleConsole::GetInstance();
        if (m_travelCommandRegistered)
            console.UnregisterCommand("tf_travel");
        if (m_hopCommandRegistered)
            console.UnregisterCommand("tf_travel_hop");
        if (m_debugCommandRegistered)
            console.UnregisterCommand("tf_travel_debug");
        m_travelCommandRegistered = false;
        m_hopCommandRegistered = false;
        m_debugCommandRegistered = false;
        TFRedeployRules::SetExtraRule({}); // capture-less, but clear for symmetry
#ifdef ENABLE_NETWORKING
        if (m_serverHandlers)
            ServerReleaseNetHandlers();
        if (m_clientHandlers)
            ClientReleaseNetHandlers();
#endif
        m_mapOf.clear();
        m_pendingPlace.clear();
        m_initialized = false;
    }

    // ---------------------------------------------------------------------------
    // Data / console
    // ---------------------------------------------------------------------------

    void TFTravelSystem::LoadContinentMeta()
    {
        // Display metadata ONLY (names/blurb). Geometry/zone constants are
        // compile-time in TFSanctuaryZone.h so terrain/damage determinism never
        // depends on a data file load order (see the header note).
        std::ifstream f(kContinentsJson, std::ios::binary);
        if (!f.is_open())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] %s missing; using built-in continent names",
                           kContinentsJson);
            return;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        const Spark::Json::Value root = Spark::Json::Parse(ss.str());
        if (!root.IsObject() || !root.HasKey("continents") || !root["continents"].IsArray())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] %s malformed; using built-in continent names",
                           kContinentsJson);
            return;
        }
        const Spark::Json::Value& arr = root["continents"];

        // W12 continent-2-data: collect EVERY registered continent. Which one
        // is live in THIS process is decided by TFDataTables (it loaded exactly
        // one region lattice — Main.cpp initializes data before travel); its
        // scene path is the stable join key against continents.json entries.
        const std::string activeScene =
            (m_ctx && m_ctx->data && m_ctx->data->IsLoaded()) ? m_ctx->data->GetContinent().scene : std::string();
        m_continentList.clear();
        for (size_t i = 0; i < arr.Size(); ++i)
        {
            const Spark::Json::Value& o = arr[i];
            if (!o.IsObject() || !o.HasKey("mapId"))
                continue;
            const int mapId = o["mapId"].AsInt(-1);
            const std::string name = o.HasKey("name") ? o["name"].AsString("") : std::string();
            if (name.empty())
                continue;
            if (mapId == kTFMapSanctuary)
            {
                m_sanctuaryDisplayName = name;
                continue;
            }
            ContinentMeta meta;
            meta.mapId = mapId;
            meta.key = o.HasKey("key") ? o["key"].AsString("") : std::string();
            meta.name = name;
            meta.blurb = o.HasKey("blurb") ? o["blurb"].AsString("") : std::string();
            meta.active = !activeScene.empty() && o.HasKey("scene") && o["scene"].AsString("") == activeScene;
            // multimap-plumbing lane (W13): OPTIONAL operator-configured
            // endpoint for a continent hosted by a different process (see
            // docs/TERRAFRONT_MULTIMAP.md). Absent on every shipped entry
            // today — no second server actually runs — so the terminal always
            // shows "no server hosting X" until an operator adds these keys.
            meta.host = o.HasKey("host") ? o["host"].AsString("") : std::string();
            meta.port = o.HasKey("port") ? static_cast<uint16_t>(o["port"].AsInt(0)) : uint16_t{0};
            m_continentList.push_back(std::move(meta));
        }

        // Fallback (older continents.json / no data tables): mapId 1 is active.
        if (std::none_of(m_continentList.begin(), m_continentList.end(),
                         [](const ContinentMeta& c) { return c.active; }))
        {
            for (ContinentMeta& c : m_continentList)
                c.active = (c.mapId == kTFMapCindralWastes);
        }
        for (const ContinentMeta& c : m_continentList)
        {
            if (!c.active)
                continue;
            m_continentDisplayName = c.name;
            if (!c.blurb.empty())
                m_continentBlurb = c.blurb;
            break;
        }
    }

    void TFTravelSystem::RegisterConsoleCommands()
    {
        auto& console = Spark::SimpleConsole::GetInstance();

        if (!console.HasCommand("tf_travel"))
        {
            console.RegisterCommand(
                "tf_travel",
                [this](const std::vector<std::string>& args) -> std::string
                {
                    if (!m_initialized)
                        return "[TF] travel system not ready";
                    if (args.empty())
                        return "usage: tf_travel <sanctuary|cindral>";
                    const std::string& a = args[0];
                    uint8_t dest;
                    if (a == "sanctuary" || a == "haven" || a == "0")
                        dest = kTFMapSanctuary;
                    else if (a == "cindral" || a == "wastes" || a == "continent" || a == "1")
                        dest = kTFMapCindralWastes;
                    else
                        return "usage: tf_travel <sanctuary|cindral>";
                    ClientRequestTravel(dest);
                    return std::string("[TF] travel requested -> ") + TFTravel_MapName(dest) +
                           " (server validates; see reply in log/menu)";
                },
                "Request travel to a map (server validates: alive, entered world, terminal proximity for "
                "sanctuary departures)",
                "TERRAFRONT", "tf_travel <sanctuary|cindral>");
            m_travelCommandRegistered = true;
        }

        // multimap-plumbing lane (W13): console-testable hop to a continent
        // hosted by a different process (mirrors the terminal menu button —
        // see docs/TERRAFRONT_MULTIMAP.md).
        if (!console.HasCommand("tf_travel_hop"))
        {
            console.RegisterCommand(
                "tf_travel_hop",
                [this](const std::vector<std::string>& args) -> std::string
                {
                    if (!m_initialized)
                        return "[TF] travel system not ready";
                    if (args.empty())
                        return "usage: tf_travel_hop <continent key>";
                    for (const ContinentMeta& c : m_continentList)
                    {
                        if (c.active || c.key != args[0])
                            continue;
                        ClientRequestContinentHop(c);
                        return std::string("[TF] ") + m_lastTravelMsg;
                    }
                    return "[TF] unknown or already-active continent key: " + args[0];
                },
                "Hop to a different continent's server (needs continents.json host/port for that key)", "TERRAFRONT",
                "tf_travel_hop <key>");
            m_hopCommandRegistered = true;
        }

        if (!console.HasCommand("tf_travel_debug"))
        {
            console.RegisterCommand(
                "tf_travel_debug",
                [this](const std::vector<std::string>&) -> std::string
                {
                    ToggleDebugUI();
                    return "[TF] travel debug panel toggled (needs tf_debug panels on)";
                },
                "Toggle the TF Travel debug panel", "TERRAFRONT", "tf_travel_debug");
            m_debugCommandRegistered = true;
        }
    }

    // ---------------------------------------------------------------------------
    // Cross-lane surface
    // ---------------------------------------------------------------------------

    uint8_t TFTravelSystem::ServerMapOf(PlayerId player) const
    {
        const auto it = m_mapOf.find(player);
        return it != m_mapOf.end() ? it->second : kTFMapSanctuary;
    }

    uint8_t TFTravelSystem::LocalMapId() const
    {
        float pos[3];
        bool alive = false;
        if (!LocalPawn(pos, alive))
            return kTFMapCindralWastes;
        return TFTravel_MapIdAt(pos[0], pos[2]);
    }

    bool TFTravelSystem::CanRedeployTo(RegionId region) const
    {
        if (!m_ctx || !m_ctx->regions)
            return false;

        // In the sanctuary there is no redeploy at all — the terminal is the way out.
        float pos[3];
        bool alive = false;
        if (LocalPawn(pos, alive) && TFTravel_IsInSanctuary(pos[0], pos[2]))
            return false;

        // On the continent: friendly AND non-contested regions only.
        const FactionId myFaction = m_ctx->localFaction;
        if (myFaction == FactionId::None)
            return false;
        if (m_ctx->regions->OwnerOf(region) != myFaction)
            return false;
        FactionId capturing = FactionId::None;
        bool contested = false;
        const float progress = m_ctx->regions->CaptureProgress(region, capturing, contested);
        if (contested || (capturing != FactionId::None && progress > 0.0f))
            return false;
        return true;
    }

    // ---------------------------------------------------------------------------
    // Frame driving
    // ---------------------------------------------------------------------------

    void TFTravelSystem::Update(float deltaTime)
    {
        if (!m_initialized || !m_ctx)
            return;

        // server-authoritative continent-hop follow-up (W13): apply any
        // TF_ContinentHopReply that arrived since the last tick BEFORE
        // anything below reads ctx->role/localPlayer/clientNet — a positive
        // reply disconnects and reconnects, changing all three.
        if (m_hopReplyPending)
            ApplyPendingContinentHop();

        m_clock += deltaTime;
        m_interactDebounce = std::max(0.0f, m_interactDebounce - deltaTime);

        // sanctuary-v2 lane (W10): drive the owned subsystems BEFORE this
        // system's client-UX early returns — they gate themselves internally
        // (HasLocalPlayer, open menus, pawn state).
        if (m_sanctuaryDecor)
            m_sanctuaryDecor->Update(deltaTime);
        if (m_targetRange)
            m_targetRange->Update(deltaTime);
        if (m_tutorial) // tutorial-flow lane (W12): gates itself internally
            m_tutorial->Update(deltaTime);

#ifdef ENABLE_NETWORKING
        // Late registration polls (region-system pattern: roles appear after
        // Initialize, once tf_host/tf_connect runs).
        const bool serverUp = ServerNetActive();
        if (serverUp && !m_serverHandlers)
            ServerEnsureNetHandlers();
        else if (!serverUp && m_serverHandlers)
            ServerReleaseNetHandlers();
        const bool clientUp = ClientNetActive();
        if (clientUp && !m_clientHandlers)
            ClientEnsureNetHandlers();
        else if (!clientUp && m_clientHandlers)
            ClientReleaseNetHandlers();
#endif

        if (m_ctx->IsAuthority())
            ServerPruneStale(deltaTime);

        // ---- client-side terminal UX (all roles with a local player) ----------
        m_nearTerminal = false;
        if (!m_ctx->HasLocalPlayer())
            return;
        // Fullscreen UIs own the keys (vehicle-shop pattern).
        if ((m_ctx->map && m_ctx->map->IsOpen()) || (m_ctx->spawnUI && m_ctx->spawnUI->IsOpen()))
        {
            SetMenuOpen(false);
            return;
        }
        InputManager* input = m_ctx->engine ? m_ctx->engine->GetInput() : nullptr;
        float pos[3];
        bool alive = false;
        if (!input || !LocalPawn(pos, alive) || !alive)
        {
            SetMenuOpen(false);
            return;
        }

        // NOTE: not named `near` — windef.h #defines near/far on Windows.
        const bool nearTerm = NearTerminal(pos, m_menuOpen ? kMenuAutoCloseM : kTFTravelTerminalUseM);
        if (nearTerm)
        {
            m_nearTerminal = true;
            if (input->WasKeyPressed('E') && m_interactDebounce <= 0.0f)
            {
                m_interactDebounce = kInteractDebounceSec;
                SetMenuOpen(!m_menuOpen);
                TFUiSounds_Play(m_ctx, m_menuOpen ? TFUiBleep::Open : TFUiBleep::Close); // W10 audio-wave-2
                if (m_menuOpen)
                    ClientRequestInfo();
            }
            // Periodic population refresh while the menu is up.
            if (m_menuOpen && m_clock - m_infoRequestedAt >= kInfoRefreshSec)
                ClientRequestInfo();
        }
        else
        {
            SetMenuOpen(false);
        }
    }

    void TFTravelSystem::FixedUpdate(float fixedDeltaTime)
    {
        (void)fixedDeltaTime;
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority())
            return;
        ServerPlacePending();
    }

} // namespace Terrafront
