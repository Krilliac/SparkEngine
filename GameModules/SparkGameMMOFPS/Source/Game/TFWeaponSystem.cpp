/**
 * @file TFWeaponSystem.cpp
 * @brief Client half of TERRAFRONT weapons: loadout, ammo/reload, ADS,
 *        ClientTriggerFire (fx + TF_FireEvent). Server half in TFWeaponServer.cpp;
 *        remote/distant fire audio in TFWeaponSystemRemoteFire.cpp.
 */
#include "Game/TFWeaponSystem.h"

#include "Game/TFAbilitySystem.h" // class-abilities lane (W9): Lockdown RoF mirror
#include "Game/TFImpactFx.h"      // impact-fx lane (W10): predicted impact burst on local fire
#include "Game/TFPlayerSystem.h"
#include "Game/TFWeaponMath.h"
#include "Game/TFViewModel.h"
#include "Net/TFClientNet.h"
#include "UI/TFHUD.h"
#include "World/TFWorldSetup.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#include "Audio/AudioEngine.h"
#include "Camera/SparkEngineCamera.h"
#include "Input/InputManager.h"
#include "Spark/IEngineContext.h"

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>

namespace Terrafront
{

    namespace
    {
        constexpr double kSwapTimeSec = 0.5; // weapon draw time after slot switch
    } // namespace

    TFWeaponSystem::TFWeaponSystem() = default;
    TFWeaponSystem::~TFWeaponSystem()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFWeaponSystem::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;

        // Force a loadout rebuild whenever our pawn respawns or tables hot-reload.
        events.Subscribe<EvPlayerSpawned>(
            [this](const EvPlayerSpawned& ev)
            {
                if (m_ctx && ev.player == m_ctx->localPlayer)
                    m_localPawn = kNoPawnEntity;
            });
        events.Subscribe<EvDataReloaded>([this](const EvDataReloaded&) { m_localPawn = kNoPawnEntity; });

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFWeaponSystem initialized");
        return true;
    }

    void TFWeaponSystem::Update(float deltaTime)
    {
        if (!m_initialized)
            return;
        m_clock += deltaTime;

        // W8 audio-polish: nearby-remote-fire heat decays toward calm (~6 s
        // half-life); ClientOnRemoteFire bumps it. Consumed by TFAudioAmbience.
        m_remoteFireHeat *= std::exp(-deltaTime * 0.1155245f); // ln(2)/6

        if (!m_ctx->HasLocalPlayer())
            return; // dedicated server has no local weapon state

        RefreshLocalLoadout();
        if (m_localPawn == kNoPawnEntity)
        {
            if (m_ctx->hud)
                m_ctx->hud->SetWeaponStatus(nullptr, -1, -1, false); // dead / no pawn -> clear
            return;
        }

        PollClientInput();
        UpdateReload();

        // Feed the HUD the active weapon each frame (name/ammo/reload). Without
        // this the HUD had no weapon source and always read "NO WEAPON / -- | --".
        if (m_ctx->hud)
        {
            const SlotState& slot = m_slots[m_activeSlot];
            if (slot.IsValid())
                m_ctx->hud->SetWeaponStatus(slot.def.name.c_str(), slot.magAmmo, slot.reserveAmmo, m_reloading);
            else
                m_ctx->hud->SetWeaponStatus(nullptr, -1, -1, false);
        }
    }

    void TFWeaponSystem::FixedUpdate(float fixedDeltaTime)
    {
        if (!m_initialized || !m_ctx->IsAuthority())
            return;
        m_serverClock += fixedDeltaTime;
        ServerStepProjectiles(fixedDeltaTime);
    }

    void TFWeaponSystem::Shutdown()
    {
        m_shooters.clear();
        m_projectiles.clear();
        m_loadedSounds.clear();
        m_localShotHook = nullptr; // W10 sanctuary-v2: drop the cosmetic seam
        m_initialized = false;
    }

    std::string TFWeaponSystem::ActiveWeaponModel() const
    {
        if (m_localPawn == kNoPawnEntity)
            return {};
        const SlotState& slot = m_slots[m_activeSlot];
        if (!slot.IsValid() || slot.def.model.empty())
            return {};
        // weapons.json paths are Assets-relative (e.g. "Models/MMOFPS/weapons/...").
        const std::string& m = slot.def.model;
        return m.rfind("Assets/", 0) == 0 ? m : "Assets/" + m;
    }

    const WeaponDef* TFWeaponSystem::ActiveWeaponDefLocal() const
    {
        // W11 weapon-optics lane: presentation-side read of the active slot's
        // resolved def (see the header note; never fed back into gameplay).
        if (m_localPawn == kNoPawnEntity)
            return nullptr;
        const SlotState& slot = m_slots[m_activeSlot];
        return slot.IsValid() ? &slot.def : nullptr;
    }

    // ---------------------------------------------------------------------------
    // Frozen API: client fire
    // ---------------------------------------------------------------------------

    void TFWeaponSystem::ClientTriggerFire()
    {
        if (!m_initialized || !m_ctx->HasLocalPlayer() || !m_ctx->players)
            return;

        PawnInfo pawn;
        if (!m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, pawn) || !pawn.alive)
            return;

        SlotState& slot = m_slots[m_activeSlot];
        if (!slot.IsValid())
            return;
        if (slot.def.kind == "beam")
            return; // TF-W3: repair torch / med applicator beam channel

        if (m_clock < m_nextFireTime || m_clock < m_swapEndTime)
            return;

        if (m_reloading)
        {
            // Shell-by-shell reloads (shotgun) can be interrupted by firing.
            if (slot.def.reloadPerShell && slot.magAmmo > 0)
                m_reloading = false;
            else
                return;
        }

        if (slot.def.magSize > 0 && slot.magAmmo <= 0)
        {
            TFViewModel::Get().NotifyDryFire(); // client-visible empty-mag click
            StartReload();
            return;
        }

        float origin[3], dir[3];
        if (!BuildViewRay(pawn, origin, dir))
            return;
        WeaponMath::PerturbCone(dir, m_ads ? slot.def.spreadAdsDeg : slot.def.spreadHipDeg, m_rng);
        // TF-W2: apply recoilVert/recoilHoriz kick to the view.

        TF_FireEvent ev{};
        ev.seq = ++m_fireSeq; // TF-W2: anchor to TFClientNet's real input sequence
        ev.weaponId = slot.weapon;
        ev.originX = origin[0];
        ev.originY = origin[1];
        ev.originZ = origin[2];
        ev.dirX = dir[0];
        ev.dirY = dir[1];
        ev.dirZ = dir[2];

        if (m_ctx->clientNet && m_ctx->clientNet->IsConnected())
            m_ctx->clientNet->SendMsg(TFMsg::FireEvent, &ev, sizeof(ev));
        // Standalone (no server connection): fx-only dry fire for local testing.

        if (slot.def.magSize > 0)
            --slot.magAmmo;
        float rof = std::max(1.0f, slot.def.rofRpm);
        if (m_ctx->abilities)
            rof *= m_ctx->abilities->RoFMultiplierLocal(); // class-abilities lane (W9)
        m_nextFireTime = m_clock + 60.0 / rof;

        // Round-robin fire one-shots so sustained fire does not machine-gun one
        // identical clip; falls back to audioFire via the parser-populated list.
        if (!slot.def.audioFireVariants.empty())
            PlayWeaponAudio(slot.def.audioFireVariants[m_fireAudioSeq++ % slot.def.audioFireVariants.size()]);
        else
            PlayWeaponAudio(slot.def.audioFire);
        if (m_ctx->world)
        {
            m_ctx->world->SpawnMuzzleFx(origin, dir);
            // impact-fx lane (W10): client-predicted impact burst at the traced
            // hit point of the same (spread-perturbed) ray the tracer uses.
            TFImpactFx::Get().OnLocalShot(*m_ctx, origin, dir);
        }
        TFViewModel::Get().NotifyLocalFire(slot.def.recoilVert, slot.def.recoilHoriz);

        // W10 sanctuary-v2: cosmetic local-shot seam (target-range dummies).
        // Same ray as the TF_FireEvent above; consumers are presentation-only.
        if (m_localShotHook)
            m_localShotHook(origin, dir, slot.def);
    }

    // ---------------------------------------------------------------------------
    // Loadout
    // ---------------------------------------------------------------------------

    void TFWeaponSystem::RefreshLocalLoadout()
    {
        if (!m_ctx->players || !m_ctx->data || !m_ctx->data->IsLoaded())
            return;

        PawnInfo pawn;
        if (!m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, pawn) || !pawn.alive)
        {
            m_localPawn = kNoPawnEntity;
            return;
        }
        if (pawn.entity == m_localPawn)
            return;

        // Fresh pawn: build the W1 default loadout for its class + faction.
        // TF-W3: TF_LoadoutChange lets players customise slots.
        m_localPawn = pawn.entity;
        for (SlotState& s : m_slots)
            s = SlotState{};

        const ClassDef* cls = m_ctx->data->GetClass(pawn.cls);
        if (!cls)
            return;

        const auto equip = [&](int slotIdx, WeaponId id)
        {
            if (id == kInvalidWeapon)
                return;
            SlotState& s = m_slots[slotIdx];
            s.weapon = id;
            s.def = m_ctx->data->ResolveWeapon(id, pawn.faction);
            s.magAmmo = s.def.magSize;
            s.reserveAmmo = s.def.reserve;
        };

        if (!cls->primarySlots.empty())
            equip(SlotPrimary, FindWeaponForSlotKey(cls->primarySlots.front(), pawn.faction));
        equip(SlotSecondary, FindWeaponForSlotKey(cls->secondarySlot, pawn.faction));
        equip(SlotTool, FindToolWeapon(cls->toolKey));
        equip(SlotMelee, FindWeaponForSlotKey("melee", pawn.faction));

        m_activeSlot = m_slots[SlotPrimary].IsValid() ? SlotPrimary : SlotSecondary;
        m_reloading = false;
        m_ads = false;
        m_nextFireTime = m_clock;
        m_swapEndTime = m_clock;

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] Loadout: %s / %s (class %u, %s)",
                       m_slots[SlotPrimary].IsValid() ? m_slots[SlotPrimary].def.name.c_str() : "-",
                       m_slots[SlotSecondary].IsValid() ? m_slots[SlotSecondary].def.name.c_str() : "-",
                       static_cast<unsigned>(pawn.cls), FactionTag(pawn.faction));
    }

    WeaponId TFWeaponSystem::FindWeaponForSlotKey(const std::string& slotKey, FactionId faction) const
    {
        if (slotKey.empty() || slotKey == "none")
            return kInvalidWeapon;

        WeaponId commonPool = kInvalidWeapon;
        for (const WeaponDef& w : m_ctx->data->AllWeapons())
        {
            if (w.slot != slotKey)
                continue;
            if (w.faction == faction)
                return w.id; // faction-specific weapon wins
            if (w.faction == FactionId::None && commonPool == kInvalidWeapon)
                commonPool = w.id; // common pool ("ALL") fallback, e.g. shotgun
        }
        return commonPool;
    }

    WeaponId TFWeaponSystem::FindToolWeapon(const std::string& toolKey) const
    {
        if (toolKey.empty() || toolKey == "none")
            return kInvalidWeapon;
        if (const WeaponDef* w = m_ctx->data->GetWeaponByKey(toolKey))
            return w->id;

        // classes.json tool keys -> weapons.json common-pool tool keys
        static constexpr std::pair<const char*, const char*> kAliases[] = {
            {"med_applicator", "tool_med"},
            {"repair_torch", "tool_repair"},
        };
        for (const auto& [from, to] : kAliases)
        {
            if (toolKey == from)
            {
                if (const WeaponDef* w = m_ctx->data->GetWeaponByKey(to))
                    return w->id;
            }
        }
        return kInvalidWeapon; // recon_dart etc. — TF-W3
    }

    // ---------------------------------------------------------------------------
    // Input / reload / view
    // ---------------------------------------------------------------------------

    void TFWeaponSystem::PollClientInput()
    {
        InputManager* input = m_ctx->engine ? m_ctx->engine->GetInput() : nullptr;
        if (!input)
            return;

        m_ads = input->IsMouseButtonDown(1); // RMB = ADS (mirrors TFB_AltFire)

        if (input->WasKeyPressed('R'))
            StartReload();

        for (int i = 0; i < SlotCount; ++i)
        {
            if (input->WasKeyPressed('1' + i))
                SwitchSlot(i);
        }
    }

    void TFWeaponSystem::StartReload()
    {
        SlotState& slot = m_slots[m_activeSlot];
        if (!slot.IsValid() || m_reloading || slot.def.magSize <= 0)
            return;
        if (slot.magAmmo >= slot.def.magSize || slot.reserveAmmo <= 0)
            return;

        m_reloading = true;
        m_reloadEndTime = m_clock + slot.def.reloadSec; // per-shell: time for the first shell
        PlayWeaponAudio(slot.def.audioReload);
    }

    void TFWeaponSystem::UpdateReload()
    {
        if (!m_reloading || m_clock < m_reloadEndTime)
            return;

        SlotState& slot = m_slots[m_activeSlot];
        if (!slot.IsValid())
        {
            m_reloading = false;
            return;
        }

        if (slot.def.reloadPerShell)
        {
            if (slot.reserveAmmo > 0 && slot.magAmmo < slot.def.magSize)
            {
                ++slot.magAmmo;
                --slot.reserveAmmo;
            }
            if (slot.reserveAmmo > 0 && slot.magAmmo < slot.def.magSize)
                m_reloadEndTime = m_clock + slot.def.reloadSec;
            else
                m_reloading = false;
        }
        else
        {
            const int moved = std::min(slot.def.magSize - slot.magAmmo, slot.reserveAmmo);
            slot.magAmmo += moved;
            slot.reserveAmmo -= moved;
            m_reloading = false;
        }
    }

    void TFWeaponSystem::SwitchSlot(int slotIdx)
    {
        if (slotIdx == m_activeSlot || slotIdx < 0 || slotIdx >= SlotCount)
            return;
        if (!m_slots[slotIdx].IsValid())
            return;

        m_activeSlot = slotIdx;
        m_reloading = false;
        m_swapEndTime = m_clock + kSwapTimeSec;
    }

    bool TFWeaponSystem::BuildViewRay(const PawnInfo& pawn, float outOrigin[3], float outDir[3]) const
    {
        const SparkEngineCamera* cam = m_ctx->engine ? m_ctx->engine->GetCamera() : nullptr;
        if (cam)
        {
            const auto& p = cam->GetPosition();
            const auto& f = cam->GetForward();
            outOrigin[0] = p.x;
            outOrigin[1] = p.y;
            outOrigin[2] = p.z;
            outDir[0] = f.x;
            outDir[1] = f.y;
            outDir[2] = f.z;
            return WeaponMath::Normalize3(outDir);
        }

        // No camera (headless/test): derive the ray from the pawn's view angles.
        outOrigin[0] = pawn.pos[0];
        outOrigin[1] = pawn.pos[1] + WeaponMath::kEyeHeightM;
        outOrigin[2] = pawn.pos[2];
        outDir[0] = std::cos(pawn.pitch) * std::sin(pawn.yaw);
        outDir[1] = -std::sin(pawn.pitch); // TF-W2: confirm pitch sign vs TFPlayerSystem
        outDir[2] = std::cos(pawn.pitch) * std::cos(pawn.yaw);
        return WeaponMath::Normalize3(outDir);
    }

    void TFWeaponSystem::PlayWeaponAudio(const std::string& assetPath, float volume)
    {
        if (assetPath.empty() || !m_ctx->engine)
            return;
        ::AudioEngine* audio = m_ctx->engine->GetAudio();
        if (!audio)
        {
            static bool s_warnedNoAudio = false;
            if (!s_warnedNoAudio)
            {
                s_warnedNoAudio = true;
                Spark::SimpleConsole::GetInstance().LogWarning("[TFAudio] no audio system (GetAudio null)");
            }
            return;
        }

        if (m_loadedSounds.insert(assetPath).second)
        {
            const std::string full = "Assets/" + assetPath; // weapons.json paths are Assets-relative
            if (FAILED(audio->LoadSound(assetPath, std::wstring(full.begin(), full.end()))))
                Spark::SimpleConsole::GetInstance().LogWarning("[TFAudio] load FAIL " + assetPath);
        }
        if (!audio->PlaySound(assetPath, volume))
            Spark::SimpleConsole::GetInstance().LogWarning("[TFAudio] play FAIL " + assetPath);
    }

    // ---------------------------------------------------------------------------
    // Debug UI
    // ---------------------------------------------------------------------------

    void TFWeaponSystem::RenderDebugUI()
    {
#ifdef SPARK_HAS_IMGUI
        if (!ImGui::CollapsingHeader("TF Weapons"))
            return;

        static const char* kSlotNames[SlotCount] = {"Primary", "Secondary", "Tool", "Melee"};
        for (int i = 0; i < SlotCount; ++i)
        {
            const SlotState& s = m_slots[i];
            if (!s.IsValid())
            {
                ImGui::Text("%s%-9s  --", i == m_activeSlot ? ">" : " ", kSlotNames[i]);
                continue;
            }
            ImGui::Text("%s%-9s  %-16s %d/%d", i == m_activeSlot ? ">" : " ", kSlotNames[i], s.def.name.c_str(),
                        s.magAmmo, s.reserveAmmo);
        }
        ImGui::Text("ADS: %s  Reloading: %s", m_ads ? "yes" : "no", m_reloading ? "yes" : "no");
        ImGui::Separator();
        ImGui::Text("Server: %u ok / %u rejected, %zu projectiles, %zu shooters", m_shotsValidated, m_shotsRejected,
                    m_projectiles.size(), m_shooters.size());
#endif
    }

} // namespace Terrafront
