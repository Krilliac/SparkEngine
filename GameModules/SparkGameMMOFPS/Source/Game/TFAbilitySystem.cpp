/**
 * @file TFAbilitySystem.cpp
 * @brief Class abilities (W9) — lifecycle (Initialize/Update/FixedUpdate),
 *        shared data helpers, spawn/death event handlers, HUD view, console
 *        commands, and debug UI. Split: the server wire/validation/effect half
 *        lives in TFAbilitySystemServer.cpp, the client mirror/input/veil half
 *        in TFAbilitySystemClient.cpp.
 *
 * See TFAbilitySystem.h for the full lane design. Wire convention: C->S
 * TF_AbilityRequest rides TFServerSim::RouteClientMessage (wiring snippet in
 * the wave report); S->C TF_AbilityState is broadcast on every phase
 * transition (self reads full detail, everyone else the active flag).
 */
#include "Game/TFAbilitySystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFDamageSystem.h"
#include "Game/TFPlayerSystem.h"
#include "Net/TFServerSim.h"

#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cstdio>

namespace Terrafront
{

    namespace
    {
        constexpr float kSweepPeriodSec = 5.0f; ///< idle server-rec cleanup cadence

        const char* PhaseText(TFAbilityPhase p)
        {
            switch (p)
            {
            case TFAbilityPhase::Ready:
                return "ready";
            case TFAbilityPhase::Active:
                return "ACTIVE";
            case TFAbilityPhase::Cooldown:
                return "cooldown";
            }
            return "?";
        }
    } // namespace

    TFAbilitySystem::TFAbilitySystem() = default;
    TFAbilitySystem::~TFAbilitySystem() = default;

    // ---------------------------------------------------------------------------
    // Lifecycle
    // ---------------------------------------------------------------------------

    bool TFAbilitySystem::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;

        events.Subscribe<EvPlayerSpawned>([this](const EvPlayerSpawned& ev) { OnPlayerSpawned(ev); });
        events.Subscribe<EvPlayerKilled>([this](const EvPlayerKilled& ev) { OnPlayerKilled(ev); });

        // Bulwark Field absorb — the one-function TFDamageSystem seam. The
        // filter checks authority itself, so installing it on every role is
        // harmless (ServerApplyDamage only runs on the authority anyway).
        if (m_ctx->damage)
            m_ctx->damage->SetIncomingDamageFilter([this](EntityId victim, float amount)
                                                   { return ServerFilterIncomingDamage(victim, amount); });

        RegisterConsoleCommands();

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFAbilitySystem initialized");
        return true;
    }

    void TFAbilitySystem::Shutdown()
    {
        if (!m_initialized)
            return;

        // No dangling `this`: the damage filter and the net handler both
        // capture this instance (TFSquadSystem::ReleaseClientHandlers pattern).
        if (m_ctx && m_ctx->damage)
            m_ctx->damage->SetIncomingDamageFilter(nullptr);
#ifdef ENABLE_NETWORKING
        if (m_clientHandlers)
            ReleaseClientHandlers();
#endif

        if (m_cmdsRegistered)
        {
            auto& console = Spark::SimpleConsole::GetInstance();
            console.UnregisterCommand("tf_ability");
            console.UnregisterCommand("tf_ability_status");
            m_cmdsRegistered = false;
        }

        // Never leave a replicated pawn mesh invisible across a shutdown/reload.
        for (const auto& [net, local] : m_hiddenMeshes)
        {
            (void)local;
            SetPawnMeshVisible(net, true);
        }
        m_hiddenMeshes.clear();
        m_activePawns.clear();
        m_server.clear();
        m_pawnOwner.clear();
        m_initialized = false;
    }

    void TFAbilitySystem::Update(float deltaTime)
    {
        if (!m_initialized || !m_ctx)
            return;

#ifdef ENABLE_NETWORKING
        // Client mirror handler lifecycle (TFSquadSystem pattern: registered
        // after link-up so the real handler wins the per-type slot).
        const bool clientUp = ClientNetActive();
        if (clientUp && !m_clientHandlers)
            EnsureClientHandlers();
        else if (!clientUp && m_clientHandlers)
        {
            ReleaseClientHandlers();
            m_mirror = SelfMirror{};
            for (const auto& [net, local] : m_hiddenMeshes)
            {
                (void)local;
                SetPawnMeshVisible(net, true);
            }
            m_hiddenMeshes.clear();
            m_activePawns.clear();
        }

        // Late-joiner burst: newly connected clients get every non-Ready state
        // (TFDeployableSystem's GetClients() diff-poll pattern).
        if (m_ctx->IsAuthority())
        {
            m_sweepAccum += deltaTime; // FixedUpdate's idle-record sweep cadence
            auto& nm = Spark::Net::NetworkManager::GetInstance();
            if (nm.IsInitialized() && nm.GetRole() == Spark::Net::NetworkRole::Server)
            {
                for (const auto& [id, info] : nm.GetClients())
                {
                    if (info.state != Spark::Net::ConnectionState::Connected || m_knownClients.contains(id))
                        continue;
                    m_knownClients.insert(id);
                    for (const auto& [player, rec] : m_server)
                    {
                        if (rec.phase == TFAbilityPhase::Ready && player != id)
                            continue; // only non-Ready states matter to strangers
                        TF_AbilityState st{};
                        st.pawnEntity = rec.pawn;
                        st.player = player;
                        st.abilityClass = static_cast<uint8_t>(rec.cls);
                        st.phase = static_cast<uint8_t>(rec.phase);
                        const double now = NowSec();
                        if (rec.phase == TFAbilityPhase::Active)
                            st.remainingSec = rec.activeUntil > 0.0 ? static_cast<float>(rec.activeUntil - now) : 0.0f;
                        else if (rec.phase == TFAbilityPhase::Cooldown)
                            st.remainingSec = static_cast<float>(rec.cooldownUntil - now);
                        st.fuel01 = rec.fuel01;
                        SendStateWire(id, st);
                    }
                }
                std::erase_if(m_knownClients, [&nm](PlayerId id) { return !nm.GetClients().contains(id); });
            }
        }
#else
        m_sweepAccum += deltaTime;
#endif

        if (m_ctx->HasLocalPlayer())
        {
            ClientPollAbilityKey();
            ClientTickMirror(deltaTime);
            UpdateVeilVisuals();
        }
    }

    void TFAbilitySystem::FixedUpdate(float fixedDeltaTime)
    {
        m_clock += fixedDeltaTime;
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || !m_ctx->data || !m_ctx->data->IsLoaded())
            return;

        const double now = NowSec();
        for (auto& [player, rec] : m_server)
        {
            const ClassAbilityDef* def = AbilityDefOf(rec.cls);
            if (!def)
                continue;
            const TFAbilityKind kind = KindOfKey(def->key);

            // Jets fuel model: drain while thrusting, regen otherwise.
            if (kind == TFAbilityKind::Jets)
            {
                if (rec.phase == TFAbilityPhase::Active)
                {
                    rec.fuel01 -= fixedDeltaTime / std::max(def->durationSec, 0.1f);
                    if (rec.fuel01 <= 0.0f)
                    {
                        rec.fuel01 = 0.0f;
                        ServerEndActive(player, rec, true);
                    }
                }
                else
                {
                    rec.fuel01 = std::min(1.0f, rec.fuel01 + def->regenPerSec * fixedDeltaTime);
                }
            }

            // Medtech heal pulse while active.
            if (kind == TFAbilityKind::Surge && rec.phase == TFAbilityPhase::Active)
                ServerTickSurge(rec, fixedDeltaTime);

            ServerAdvancePhase(player, rec);
        }

        // Cheap idle-record cleanup (bots included — they re-seed lazily).
        if (m_sweepAccum >= kSweepPeriodSec)
        {
            m_sweepAccum = 0.0f;
            std::erase_if(m_server,
                          [this, now](const auto& kv)
                          {
                              const ServerRec& rec = kv.second;
                              if (rec.phase != TFAbilityPhase::Ready || now < rec.cooldownUntil)
                                  return false;
                              PawnInfo pi{};
                              const bool hasPawn =
                                  m_ctx->players && m_ctx->players->GetPawnByPlayer(kv.first, pi) && pi.alive;
                              if (!hasPawn)
                                  m_pawnOwner.erase(rec.pawn);
                              return !hasPawn;
                          });
        }
    }

    // ---------------------------------------------------------------------------
    // Data helpers
    // ---------------------------------------------------------------------------

    TFAbilityKind TFAbilitySystem::KindOfKey(const std::string& key)
    {
        if (key.empty())
            return TFAbilityKind::None;
        if (key == "veil")
            return TFAbilityKind::Veil;
        if (key == "jets")
            return TFAbilityKind::Jets;
        if (key == "surge")
            return TFAbilityKind::Surge;
        if (key == "forge")
            return TFAbilityKind::Forge;
        if (key == "aegiswall")
            return TFAbilityKind::AegisWall;
        if (key == "lockdown")
            return TFAbilityKind::Lockdown;
        return TFAbilityKind::GenericTimed;
    }

    const ClassAbilityDef* TFAbilitySystem::AbilityDefOf(ClassId cls) const
    {
        if (!m_ctx || !m_ctx->data || !m_ctx->data->IsLoaded())
            return nullptr;
        const ClassDef* cd = m_ctx->data->GetClass(cls);
        if (!cd || cd->ability.key.empty())
            return nullptr;
        return &cd->ability;
    }

    double TFAbilitySystem::NowSec() const
    {
        if (m_ctx && m_ctx->IsAuthority() && m_ctx->serverSim)
            return m_ctx->serverSim->ServerTime();
        return m_clock;
    }

    // ---------------------------------------------------------------------------
    // Events
    // ---------------------------------------------------------------------------

    void TFAbilitySystem::OnPlayerSpawned(const EvPlayerSpawned& ev)
    {
        if (!m_ctx || !m_ctx->IsAuthority())
            return;
        ServerRec& rec = m_server[ev.player];
        const EntityId oldPawn = rec.pawn;
        if (rec.cls != ev.cls)
        {
            rec = ServerRec{}; // new class == fresh ability state (incl. cooldown)
            rec.cls = ev.cls;
        }
        if (oldPawn != ev.pawn)
            m_pawnOwner.erase(oldPawn);
        rec.pawn = ev.pawn;
        m_pawnOwner[ev.pawn] = ev.player;
    }

    void TFAbilitySystem::OnPlayerKilled(const EvPlayerKilled& ev)
    {
        if (!m_ctx || !m_ctx->IsAuthority())
            return;
        auto it = m_server.find(ev.victim);
        if (it == m_server.end())
            return;
        if (it->second.phase == TFAbilityPhase::Active)
            ServerEndActive(ev.victim, it->second, true); // death ends it, cd applies
        m_pawnOwner.erase(it->second.pawn);
        it->second.pawn = 0;
    }

    // ---------------------------------------------------------------------------
    // HUD view + console
    // ---------------------------------------------------------------------------

    TFAbilitySystem::HudView TFAbilitySystem::GetLocalHudView() const
    {
        HudView v;
        if (!m_ctx || !m_mirror.valid)
            return v;
        const ClassAbilityDef* def = AbilityDefOf(m_mirror.cls);
        if (!def)
            return v;
        const TFAbilityKind kind = KindOfKey(def->key);
        v.valid = true;
        v.name = def->name.empty() ? def->key : def->name;
        v.phase = m_mirror.phase;
        v.remainingSec = m_mirror.remainingSec;
        if (kind == TFAbilityKind::Jets)
        {
            v.fuelBased = true;
            v.fraction01 = m_mirror.fuel01;
        }
        else if (m_mirror.phase == TFAbilityPhase::Cooldown && def->cooldownSec > 0.0f)
        {
            v.fraction01 = 1.0f - std::clamp(m_mirror.remainingSec / def->cooldownSec, 0.0f, 1.0f);
        }
        else if (m_mirror.phase == TFAbilityPhase::Active && def->durationSec > 0.0f)
        {
            v.fraction01 = std::clamp(m_mirror.remainingSec / def->durationSec, 0.0f, 1.0f);
        }
        return v;
    }

    void TFAbilitySystem::RegisterConsoleCommands()
    {
        if (m_cmdsRegistered)
            return;
        auto& console = Spark::SimpleConsole::GetInstance();

        console.RegisterCommand(
            "tf_ability",
            [this](const std::vector<std::string>& args) -> std::string
            {
                bool on = !(m_mirror.valid && m_mirror.phase == TFAbilityPhase::Active);
                if (!args.empty())
                {
                    if (args[0] == "on")
                        on = true;
                    else if (args[0] == "off")
                        on = false;
                    else
                        return "usage: tf_ability [on|off]";
                }
                SendRequest(on);
                return std::string("[TF] ability request sent (") + (on ? "on" : "off") + ")";
            },
            "Activate/toggle your class ability (same as the F key)", "TERRAFRONT", "tf_ability [on|off]");

        console.RegisterCommand(
            "tf_ability_status",
            [this](const std::vector<std::string>&) -> std::string
            {
                const HudView v = GetLocalHudView();
                if (!v.valid)
                    return "[TF] no ability state yet (spawn first)";
                char buf[160];
                std::snprintf(buf, sizeof(buf), "[TF] %s: %s  remaining %.1fs  %s %.0f%%", v.name.c_str(),
                              PhaseText(v.phase), v.remainingSec, v.fuelBased ? "fuel" : "fill", v.fraction01 * 100.0f);
                return buf;
            },
            "Show your class ability phase/cooldown/fuel", "TERRAFRONT", "tf_ability_status");

        m_cmdsRegistered = true;
    }

    // ---------------------------------------------------------------------------
    // Debug UI
    // ---------------------------------------------------------------------------

    void TFAbilitySystem::RenderDebugUI()
    {
#ifdef SPARK_HAS_IMGUI
        if (!ImGui::CollapsingHeader("TF Abilities"))
            return;
        ImGui::Text("activations %u  rejected %u  bad packets %u", m_activations, m_rejected, m_badPackets);
        ImGui::Text("heal given %.0f hp  absorbed %.0f hp", m_healGiven, m_absorbed);

        if (m_ctx && m_ctx->HasLocalPlayer())
        {
            const HudView v = GetLocalHudView();
            if (v.valid)
                ImGui::Text("local: %s  %s  %.1fs  %s %.0f%%", v.name.c_str(), PhaseText(v.phase), v.remainingSec,
                            v.fuelBased ? "fuel" : "fill", v.fraction01 * 100.0f);
            else
                ImGui::TextUnformatted("local: (no state)");
        }

        if (m_ctx && m_ctx->IsAuthority())
        {
            const double now = NowSec();
            ImGui::Text("server records: %zu  active-pawn mirror: %zu", m_server.size(), m_activePawns.size());
            for (const auto& [player, rec] : m_server)
            {
                const ClassAbilityDef* def = AbilityDefOf(rec.cls);
                ImGui::Text("  p%u %-10s %s  cd %.1f  fuel %.2f  absorb %.0f", player, def ? def->key.c_str() : "?",
                            PhaseText(rec.phase), std::max(0.0, rec.cooldownUntil - now), rec.fuel01, rec.absorbPool);
            }
        }
#endif
    }

} // namespace Terrafront
