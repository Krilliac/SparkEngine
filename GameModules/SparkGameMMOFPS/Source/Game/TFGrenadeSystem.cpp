/**
 * @file TFGrenadeSystem.cpp
 * @brief Frag grenades (W10) — lifecycle, per-frame/fixed ticks, time base and
 *        the loadout-depth grenade-kind resolution + "grenadeEffect" table.
 *
 * See TFGrenadeSystem.h for the full lane design. Wire convention: C->S
 * TF_GrenadeThrow rides TFServerSim::RouteClientMessage (wiring snippet in
 * the wave report); S->C spawn/update/boom are broadcast by this system with
 * a direct local-mirror call for the listen-host/standalone player.
 *
 * Split parts: TFGrenadeSystemServer.cpp (throw/simulate/detonate/broadcast),
 * TFGrenadeSystemClient.cpp (G-key entry, mirror store, net handlers, debug
 * UI), TFGrenadeSystemFx.cpp (sphere/boom/smoke rendering, flash overlay);
 * shared helpers live in TFGrenadeSystemInternal.h.
 */
#include "Game/TFGrenadeSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFProgressionSystem.h" // loadout-depth wave: GetLoadout (grenade kind resolution)
#include "Net/TFServerSim.h"

#include "Graphics/Mesh.h"
#include "Utils/JsonUtils.h" // loadout-depth wave: own lazy "grenadeEffect" parse (TFOpticsSystem precedent)
#include "Utils/LogMacros.h"

#include <fstream>
#include <sstream>

namespace Terrafront
{

    namespace
    {
        // loadout-depth wave: own "grenadeEffect" parse path.
        constexpr const char* kGrenadeDataJsonPath = "Assets/MMOFPS/Data/weapons.json";
    } // namespace

    TFGrenadeSystem::TFGrenadeSystem() = default;
    TFGrenadeSystem::~TFGrenadeSystem() = default;

    // ---------------------------------------------------------------------------
    // Lifecycle
    // ---------------------------------------------------------------------------

    bool TFGrenadeSystem::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;

        events.Subscribe<EvPlayerSpawned>([this](const EvPlayerSpawned& ev) { OnPlayerSpawned(ev); });
        events.Subscribe<EvPlayerKilled>([this](const EvPlayerKilled& ev) { OnPlayerKilled(ev); });

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFGrenadeSystem initialized");
        return true;
    }

    void TFGrenadeSystem::Shutdown()
    {
        if (!m_initialized)
            return;

#ifdef ENABLE_NETWORKING
        if (m_clientHandlers)
            ReleaseClientHandlers();
#endif
        m_throwers.clear();
        m_live.clear();
        m_flashedUntil.clear(); // loadout-depth wave
        m_clientGrenades.clear();
        m_booms.clear();
        m_smokePuffs.clear(); // loadout-depth wave
        m_sphere.reset();
        m_quad.reset();
        m_initialized = false;
    }

    void TFGrenadeSystem::Update(float deltaTime)
    {
        if (!m_initialized || !m_ctx)
            return;

#ifdef ENABLE_NETWORKING
        // Client mirror handler lifecycle (TFAbilitySystem pattern: registered
        // after link-up so the real handler wins the per-type slot).
        const bool clientUp = ClientNetActive();
        if (clientUp && !m_clientHandlers)
            EnsureClientHandlers();
        else if (!clientUp && m_clientHandlers)
        {
            ReleaseClientHandlers();
            m_clientGrenades.clear();
            m_booms.clear();
            m_smokePuffs.clear(); // loadout-depth wave
            m_localFlashUntil = -1.0;
            m_localRemaining = -1;
        }
#endif

        if (m_ctx->HasLocalPlayer())
        {
            ClientReseedLocalCount();
            ClientPollThrowKey();
            ClientAdvance(deltaTime);
            ClientAdvanceSmoke(deltaTime); // loadout-depth wave
        }
    }

    void TFGrenadeSystem::FixedUpdate(float fixedDeltaTime)
    {
        m_clock += fixedDeltaTime;
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || !m_ctx->data || !m_ctx->data->IsLoaded())
            return;

        m_updateAccum += fixedDeltaTime;
        const bool sendTick = m_updateAccum >= kTFGrenadeUpdatePeriodSec;
        if (sendTick)
            m_updateAccum -= kTFGrenadeUpdatePeriodSec;

        for (auto it = m_live.begin(); it != m_live.end();)
        {
            ServerGrenade& g = *it;
            g.lifeSec += fixedDeltaTime;

            if (g.lifeSec >= kTFGrenadeFuseSec)
            {
                ServerDetonate(g);
                TF_GrenadeBoom boom{};
                boom.grenadeId = g.id;
                boom.posQX = GrenadeDetail::QuantPos(g.pos[0]);
                boom.posQY = GrenadeDetail::QuantPos(g.pos[1]);
                boom.posQZ = GrenadeDetail::QuantPos(g.pos[2]);
                ServerBroadcast(kTFMsgGrenadeBoom, &boom, sizeof(boom), /*reliable*/ true);
                it = m_live.erase(it);
                continue;
            }

            const bool wasResting = g.resting;
            if (!g.resting)
                ServerStepGrenade(g, fixedDeltaTime);

            // Rest transitions get one reliable pin; airborne grenades get the
            // ~10 Hz unreliable correction stream.
            if ((g.resting && !wasResting) || (sendTick && !g.resting))
            {
                TF_GrenadeUpdate up{};
                up.grenadeId = g.id;
                up.posQX = GrenadeDetail::QuantPos(g.pos[0]);
                up.posQY = GrenadeDetail::QuantPos(g.pos[1]);
                up.posQZ = GrenadeDetail::QuantPos(g.pos[2]);
                up.velQX = GrenadeDetail::QuantVel(g.vel[0]);
                up.velQY = GrenadeDetail::QuantVel(g.vel[1]);
                up.velQZ = GrenadeDetail::QuantVel(g.vel[2]);
                up.flags = g.resting ? 1 : 0;
                ServerBroadcast(kTFMsgGrenadeUpdate, &up, sizeof(up), /*reliable*/ g.resting);
            }
            ++it;
        }
    }

    double TFGrenadeSystem::NowSec() const
    {
        if (m_ctx && m_ctx->IsAuthority() && m_ctx->serverSim)
            return m_ctx->serverSim->ServerTime();
        return m_clock;
    }

    const WeaponDef* TFGrenadeSystem::FragDef() const
    {
        if (!m_ctx || !m_ctx->data || !m_ctx->data->IsLoaded())
            return nullptr;
        return m_ctx->data->GetWeaponByKey(kTFGrenadeWeaponKey);
    }

    // ---------------------------------------------------------------------------
    // loadout-depth wave: grenade-kind resolution + effect table
    // ---------------------------------------------------------------------------

    const WeaponDef* TFGrenadeSystem::SelectedGrenadeDef(PlayerId player) const
    {
        if (m_ctx && m_ctx->progression && m_ctx->data && m_ctx->data->IsLoaded())
        {
            if (const TFLoadout* lo = m_ctx->progression->GetLoadout(player))
            {
                if (!lo->grenade.empty() && lo->grenade != kTFGrenadeWeaponKey)
                {
                    if (const WeaponDef* def = m_ctx->data->GetWeaponByKey(lo->grenade))
                    {
                        // Defense in depth: the pick was already validated at
                        // save time (TFProgressionSystem::ValidGrenadeChoiceKey);
                        // re-checking the unlock here means a later un-grant
                        // can't leave a stale pick silently in effect.
                        if (m_ctx->progression->IsWeaponUnlocked(player, def->id))
                            return def;
                    }
                }
            }
        }
        return FragDef();
    }

    void TFGrenadeSystem::EnsureEffectTable()
    {
        if (m_effectTableLoaded)
            return;
        m_effectTableLoaded = true; // one attempt; missing/malformed == everything defaults to damage

        std::ifstream f(kGrenadeDataJsonPath, std::ios::binary);
        if (!f.is_open())
            return;
        std::ostringstream ss;
        ss << f.rdbuf();

        const Spark::Json::Value root = Spark::Json::Parse(ss.str());
        if (!root.IsObject() || !root.HasKey("weapons") || !root["weapons"].IsArray())
            return;

        const Spark::Json::Value& weapons = root["weapons"];
        for (size_t i = 0; i < weapons.Size(); ++i)
        {
            const Spark::Json::Value& w = weapons[i];
            if (!w.IsObject() || !w.HasKey("key") || !w.HasKey("grenadeEffect"))
                continue;
            const std::string key = w["key"].AsString({});
            const std::string effect = w["grenadeEffect"].AsString({});
            if (key.empty())
                continue;

            uint8_t kind = kTFGrenadeEffectDamage;
            if (effect == "smoke")
                kind = kTFGrenadeEffectSmoke;
            else if (effect == "flash")
                kind = kTFGrenadeEffectFlash;
            else if (effect != "damage" && !effect.empty())
            {
                SPARK_LOG_WARN(Spark::LogCategory::Game,
                               "[TF] grenade effect: '%s': unknown grenadeEffect '%s' (damage)", key.c_str(),
                               effect.c_str());
            }
            m_effectByKey[key] = kind;
        }
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] grenade effect: %zu tagged rows", m_effectByKey.size());
    }

    uint8_t TFGrenadeSystem::EffectKindOf(const std::string& weaponKey)
    {
        EnsureEffectTable();
        auto it = m_effectByKey.find(weaponKey);
        return it != m_effectByKey.end() ? it->second : kTFGrenadeEffectDamage;
    }

    // ---------------------------------------------------------------------------
    // Events (authority bookkeeping)
    // ---------------------------------------------------------------------------

    void TFGrenadeSystem::OnPlayerSpawned(const EvPlayerSpawned& ev)
    {
        if (!m_ctx || !m_ctx->IsAuthority())
            return;
        const ClassDef* cls = m_ctx->data ? m_ctx->data->GetClass(ev.cls) : nullptr;
        ThrowerRec& rec = m_throwers[ev.player];
        rec.remaining = cls ? cls->grenades : 0;
        rec.nextThrowAt = 0.0;
    }

    void TFGrenadeSystem::OnPlayerKilled(const EvPlayerKilled& ev)
    {
        if (!m_ctx || !m_ctx->IsAuthority())
            return;
        // Per-LIFE quota: the record dies with the pawn (respawn reseeds it).
        // Live grenades keep flying — a dead thrower's frag still detonates.
        m_throwers.erase(ev.victim);
    }

} // namespace Terrafront
