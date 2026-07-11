/**
 * @file TFDeathRecap.cpp
 * @brief Death recap v2 client panel — see TFDeathRecap.h for the full design
 *        note. Server side (damage log + TF_DeathRecap send) lives in
 *        Game/TFDamageSystem.cpp.
 */
#include "UI/TFDeathRecap.h"

#include "Data/TFDataTables.h"
#include "Game/TFOutfitSystem.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFSocialSystem.h"
#include "Net/TFClientNet.h"
#include "UI/TFMapScreen.h"
#include "UI/TFSpawnScreen.h"

#include "Graphics/GraphicsEngine.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace Terrafront
{

    namespace
    {

        /// Class icon/name key (TFScoreboard ClassIconKey convention —
        /// ui/128/class_<key>.png verified shipped for all six).
        const char* RecapClassKey(uint8_t cls)
        {
            switch (static_cast<ClassId>(cls))
            {
            case ClassId::Ghost:
                return "ghost";
            case ClassId::Striker:
                return "striker";
            case ClassId::Medtech:
                return "medtech";
            case ClassId::Fabricator:
                return "fabricator";
            case ClassId::Bulwark:
                return "bulwark";
            case ClassId::Colossus:
                return "colossus";
            default:
                return nullptr;
            }
        }

    } // namespace

    TFDeathRecap::TFDeathRecap() = default;
    TFDeathRecap::~TFDeathRecap()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFDeathRecap::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;

        // Lane self-wiring (TFClientNet::SetChatUI pattern): context pointers
        // are published before any Initialize, so ctx.damage is already valid.
        // This hook is how listen-host/standalone victims receive their recap
        // (no socket); on pure clients the mirror simply never fires.
        if (ctx.damage)
            ctx.damage->SetDeathRecapMirror([this](const TF_DeathRecap& rc) { ClientHandleRecap(rc); });

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFDeathRecap initialized");
        return true;
    }

    void TFDeathRecap::Shutdown()
    {
#ifdef ENABLE_NETWORKING
        if (m_clientHandlers)
            ReleaseClientHandlers();
#endif
        // Uninstall the mirror so no dangling `this` survives (the
        // TFAbilitySystem incoming-filter pattern).
        if (m_ctx && m_ctx->damage)
            m_ctx->damage->SetDeathRecapMirror(nullptr);
        m_valid = false;
        m_initialized = false;
    }

    void TFDeathRecap::Update(float deltaTime)
    {
        (void)deltaTime;
        if (!m_initialized || !m_ctx)
            return;

#ifdef ENABLE_NETWORKING
        // Client handler lifecycle (TFMedalSystem/TFSquadSystem pattern).
        const bool clientUp = ClientNetActive();
        if (clientUp && !m_clientHandlers)
            EnsureClientHandlers();
        else if (!clientUp && m_clientHandlers)
        {
            ReleaseClientHandlers();
            m_valid = false;
        }
#endif

        // Dead->alive edge: the recap belongs to the PREVIOUS life — clear it
        // on respawn (TFHUD m_wasViewAlive pattern).
        const bool alive = LocalPawnAlive();
        if (alive && !m_wasAlive)
            m_valid = false;
        m_wasAlive = alive;
    }

    void TFDeathRecap::FixedUpdate(float fixedDeltaTime)
    {
        (void)fixedDeltaTime; // event-driven; nothing to do on the fixed step
    }

    void TFDeathRecap::ClientHandleRecap(const TF_DeathRecap& recap)
    {
        m_recap = recap;
        if (m_recap.entryCount > kTFDeathRecapMaxEntries)
            m_recap.entryCount = kTFDeathRecapMaxEntries; // never trust the wire
        m_valid = true;
        ++m_recapsReceived;
    }

    PlayerId TFDeathRecap::LocalPlayer() const
    {
        if (!m_ctx)
            return kInvalidPlayer;
        if (m_ctx->localPlayer != kInvalidPlayer)
            return m_ctx->localPlayer;
        return m_ctx->clientNet ? m_ctx->clientNet->LocalPlayerId() : kInvalidPlayer;
    }

    bool TFDeathRecap::LocalPawnAlive() const
    {
        const PlayerId local = LocalPlayer();
        if (local == kInvalidPlayer || !m_ctx || !m_ctx->players)
            return false;
        PawnInfo pi{};
        return m_ctx->players->GetPawnByPlayer(local, pi) && pi.alive;
    }

    void TFDeathRecap::ResolvePlayerLabel(PlayerId id, char* out, size_t outSize) const
    {
        char base[24];
        std::string rosterName;
        if (id != kInvalidPlayer && id == LocalPlayer())
            std::snprintf(base, sizeof(base), "YOU"); // TFHUD HudPlayerLabel convention
        else if (m_ctx && m_ctx->social && m_ctx->social->NameOfPlayer(id, rosterName) && !rosterName.empty())
            std::snprintf(base, sizeof(base), "%s", rosterName.c_str());
        else if (id == kInvalidPlayer)
            std::snprintf(base, sizeof(base), "-");
        else if (id == kTFLocalHostPlayer)
            std::snprintf(base, sizeof(base), "HOST");
        else
            std::snprintf(base, sizeof(base), "p%u", id);

        OutfitTaggedLabel(m_ctx && m_ctx->outfits ? m_ctx->outfits->GetOutfitTag(id) : "", base, out, outSize);
    }

#ifdef ENABLE_NETWORKING

    bool TFDeathRecap::ClientNetActive() const
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        return m_ctx->role == NetRole::Client && nm.IsInitialized() &&
               nm.GetRole() == Spark::Net::NetworkRole::Client &&
               nm.GetConnectionState() == Spark::Net::ConnectionState::Connected;
    }

    void TFDeathRecap::EnsureClientHandlers()
    {
        using Spark::Net::MessageType;
        using Spark::Net::NetworkMessage;
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        nm.RegisterHandler(static_cast<MessageType>(kTFMsgDeathRecap),
                           [this](const NetworkMessage& m)
                           {
                               if (m.payload.size() != sizeof(TF_DeathRecap))
                               {
                                   ++m_badPackets;
                                   return;
                               }
                               TF_DeathRecap rc{};
                               std::memcpy(&rc, m.payload.data(), sizeof(rc));
                               ClientHandleRecap(rc);
                           });
        m_clientHandlers = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] death-recap mirror handler registered");
    }

    void TFDeathRecap::ReleaseClientHandlers()
    {
        // NetworkManager has no per-type removal; replace with a no-op so no
        // dangling `this` survives module shutdown (TFServerSim pattern).
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        nm.RegisterHandler(static_cast<Spark::Net::MessageType>(kTFMsgDeathRecap),
                           [](const Spark::Net::NetworkMessage&) {});
        m_clientHandlers = false;
    }

#endif // ENABLE_NETWORKING

    // ---------------------------------------------------------------------------
    // Rendering
    // ---------------------------------------------------------------------------

#ifdef SPARK_HAS_IMGUI

    void TFDeathRecap::RenderUI()
    {
        if (!m_initialized || !m_ctx || !m_ctx->HasLocalPlayer() || !m_valid)
            return;
        if (LocalPawnAlive())
            return; // only meaningful on the death screen
        // Yield to the full-screen UIs exactly like TFHUD::DrawDeathActions.
        if (m_ctx->map && m_ctx->map->IsOpen())
            return;
        if (m_ctx->spawnUI && m_ctx->spawnUI->IsOpen())
            return;

        GraphicsEngine* gfx = m_ctx->engine ? m_ctx->engine->GetGraphics() : nullptr;
        const bool canDrawIcons = gfx && gfx->GetDevice() && gfx->GetContext();

        // Below the HUD death overlay (center text) and the DEPLOY/MAP buttons
        // (center+96): top-anchored at center+150 so nothing overlaps.
        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f + 150.0f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.55f);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                       ImGuiWindowFlags_AlwaysAutoResize;
        if (!ImGui::Begin("##TFDeathRecap", nullptr, flags))
        {
            ImGui::End();
            return;
        }

        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.30f, 1.0f), "DEATH RECAP");
        ImGui::Separator();

        // --- killer summary ---------------------------------------------------
        if (m_recap.killerPlayer != kInvalidPlayer)
        {
            char killerLabel[48];
            ResolvePlayerLabel(m_recap.killerPlayer, killerLabel, sizeof(killerLabel));

            // Class icon (18 px, scoreboard convention) + faction-tinted name.
            if (canDrawIcons)
            {
                if (const char* key = RecapClassKey(m_recap.killerClass))
                {
                    char iconPath[80];
                    std::snprintf(iconPath, sizeof(iconPath), "Assets/Textures/MMOFPS/ui/64/class_%s.png", key);
                    if (ID3D11ShaderResourceView* srv = gfx->GetOrLoadTextureSRV(iconPath))
                    {
                        ImGui::Image(static_cast<void*>(srv), ImVec2(18.0f, 18.0f));
                        ImGui::SameLine();
                    }
                }
            }

            float fc[4];
            const uint8_t f = m_recap.killerFaction;
            FactionColor(f < static_cast<uint8_t>(FactionId::COUNT) ? static_cast<FactionId>(f) : FactionId::None, fc);
            ImGui::TextColored(ImVec4(fc[0], fc[1], fc[2], 1.0f), "%s", killerLabel);

            const char* className = nullptr;
            if (m_ctx->data && m_ctx->data->IsLoaded() && m_recap.killerClass < static_cast<uint8_t>(ClassId::COUNT))
            {
                if (const ClassDef* cd = m_ctx->data->GetClass(static_cast<ClassId>(m_recap.killerClass)))
                    className = cd->name.c_str();
            }
            if (className)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", className);
            }
            if (m_recap.distanceDm != 0xFFFFu)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("%.0f m", static_cast<float>(m_recap.distanceDm) * 0.1f);
            }

            if (m_recap.killerHealth != 0xFFFFu && m_recap.killerShield != 0xFFFFu)
                ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.85f, 1.0f), "Killer left at %u HP + %u shield",
                                   m_recap.killerHealth, m_recap.killerShield);

            if (m_recap.hitsOnKiller > 0)
                ImGui::TextColored(ImVec4(0.65f, 0.85f, 1.0f, 1.0f), "You dealt %u damage in %u hit%s",
                                   m_recap.damageToKiller, m_recap.hitsOnKiller, m_recap.hitsOnKiller == 1 ? "" : "s");
            else
                ImGui::TextDisabled("You didn't damage your killer");
        }
        else
        {
            ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.85f, 1.0f), "Killed by the environment");
        }

        // --- hit timeline (oldest first; ids resolve client-side) -------------
        if (m_recap.entryCount > 0)
        {
            ImGui::Separator();
            for (uint32_t i = 0; i < m_recap.entryCount; ++i)
            {
                const TF_DeathRecapEntry& e = m_recap.entries[i];

                // Weapon icon from the weapon KEY (ui/128/wpn_<key>.png).
                const WeaponDef* wd = (m_ctx->data && m_ctx->data->IsLoaded())
                                          ? m_ctx->data->GetWeapon(static_cast<WeaponId>(e.weaponId))
                                          : nullptr;
                bool drewIcon = false;
                if (canDrawIcons && wd && !wd->key.empty())
                {
                    char iconPath[96];
                    std::snprintf(iconPath, sizeof(iconPath), "Assets/Textures/MMOFPS/ui/128/wpn_%s.png",
                                  wd->key.c_str());
                    if (ID3D11ShaderResourceView* srv = gfx->GetOrLoadTextureSRV(iconPath))
                    {
                        ImGui::Image(static_cast<void*>(srv), ImVec2(20.0f, 20.0f));
                        drewIcon = true;
                    }
                }
                if (!drewIcon)
                    ImGui::Dummy(ImVec2(20.0f, 20.0f)); // headless / missing texture
                ImGui::SameLine();

                char attacker[48];
                ResolvePlayerLabel(e.attackerPlayer, attacker, sizeof(attacker));
                const char* weaponName = (wd && !wd->name.empty()) ? wd->name.c_str() : "-";

                if (e.headshot)
                    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "-%4.1fs  %s  %s  %u  HEADSHOT",
                                       static_cast<float>(e.ageDeciSec) * 0.1f, attacker, weaponName, e.damage);
                else
                    ImGui::TextColored(ImVec4(0.88f, 0.88f, 0.90f, 1.0f), "-%4.1fs  %s  %s  %u",
                                       static_cast<float>(e.ageDeciSec) * 0.1f, attacker, weaponName, e.damage);
            }
        }

        ImGui::End();
    }

    void TFDeathRecap::RenderDebugUI()
    {
        if (!ImGui::CollapsingHeader("TF Death Recap"))
            return;
        ImGui::Text("recaps recv : %u", m_recapsReceived);
        ImGui::Text("bad packets : %u", m_badPackets);
        ImGui::Text("valid       : %s", m_valid ? "yes" : "no");
        ImGui::Text("entries     : %u", m_valid ? m_recap.entryCount : 0u);
    }

#else // !SPARK_HAS_IMGUI — headless: wire + state only

    void TFDeathRecap::RenderUI() {}
    void TFDeathRecap::RenderDebugUI() {}

#endif // SPARK_HAS_IMGUI

} // namespace Terrafront
