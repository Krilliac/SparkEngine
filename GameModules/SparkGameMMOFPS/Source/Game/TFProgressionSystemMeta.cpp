/**
 * @file TFProgressionSystemMeta.cpp
 * @brief TFProgressionSystem W6 progression expansion + loadout-depth wave:
 *        unlock tree queries/purchases, per-weapon aggregate stats, loadout
 *        save/load, grenade choice + suit slot validation and the suits.json
 *        passive table. Split from TFProgressionSystem.cpp; shared helpers
 *        live in TFProgressionSystemInternal.h.
 */
#include "Game/TFProgressionSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFGrenadeSystem.h" // loadout-depth wave: grenade choice key constants (single source of truth)
#include "Game/TFPlayerSystem.h"
#include "Game/TFProgressionSystemInternal.h"
#include "Persistence/TFUnlockTree.h" // W6 progression expansion: unlock table
#include "Utils/JsonUtils.h"
#include "Utils/LogMacros.h"

#include <cstring>

namespace Terrafront
{

    using namespace ProgressionDetail;

    namespace
    {

        // loadout-depth wave: own lazy parse of the suit passive table (TFOpticsSystem
        // ::EnsureTable precedent — Data/TFDataTables.h is a frozen contended surface).
        constexpr const char* kSuitsJsonPath = "Assets/MMOFPS/Data/suits.json";

    } // namespace

    // ---------------------------------------------------------------------------
    // W6 progression expansion: unlock tree / per-weapon stats / loadout
    // ---------------------------------------------------------------------------

    bool TFProgressionSystem::IsUnlocked(PlayerId player, std::string_view unlockKey) const
    {
        return IsUnlockedInternal(player, unlockKey, 0);
    }

    bool TFProgressionSystem::IsUnlockedInternal(PlayerId player, std::string_view unlockKey, int depth) const
    {
        if (depth > 8) // prereq-chain runaway guard (table is author-checked; belt & braces)
            return false;

        const TFUnlockDef* def = TFUnlockTree::Find(unlockKey);
        if (!def)
            return false;

        if (const auto* meta = m_meta.Find(player))
            if (meta->unlocks.count(std::string(unlockKey)) != 0)
                return true; // explicitly purchased/granted

        if (def->fluxCost > 0)
            return false; // purchasable node without a purchase record

        if (RankOf(player) < def->requiredRank)
            return false;
        if (def->prereq && !IsUnlockedInternal(player, def->prereq, depth + 1))
            return false;
        return true; // free rank unlock, auto-granted
    }

    bool TFProgressionSystem::IsWeaponUnlocked(PlayerId player, WeaponId weapon) const
    {
        const std::string* key = WeaponKeyOf(weapon);
        if (!key)
        {
            // Tables not loaded yet: fail open (nothing else can resolve the id
            // either). Tables loaded but id unknown: fail closed.
            return !(m_ctx && m_ctx->data && m_ctx->data->IsLoaded());
        }
        const TFUnlockDef* node = TFUnlockTree::FindByWeaponKey(*key);
        if (!node)
            return true; // not in the tree == default kit
        return IsUnlocked(player, node->key);
    }

    bool TFProgressionSystem::IsVehicleUnlocked(PlayerId player, VehicleId vehicle) const
    {
        const TFUnlockDef* node = TFUnlockTree::FindByVehicle(vehicle);
        if (!node)
            return true; // not in the tree (Drifter) == always available
        return IsUnlocked(player, node->key);
    }

    TFUnlockResult TFProgressionSystem::ServerTryUnlock(PlayerId player, std::string_view unlockKey)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || player == kInvalidPlayer)
            return TFUnlockResult::NotAuthority;

        const TFUnlockDef* def = TFUnlockTree::Find(unlockKey);
        if (!def)
            return TFUnlockResult::UnknownKey;
        if (IsUnlocked(player, unlockKey))
            return TFUnlockResult::AlreadyUnlocked;
        if (RankOf(player) < def->requiredRank)
            return TFUnlockResult::RankTooLow;
        if (def->prereq && !IsUnlocked(player, def->prereq))
            return TFUnlockResult::PrereqLocked;
        if (def->fluxCost > 0 && !ServerSpendFlux(player, def->fluxCost))
            return TFUnlockResult::InsufficientFlux;

        auto& meta = m_meta.Ensure(player);
        meta.unlocks.insert(def->key);
        meta.dirty = true;
        m_dirty = true; // prompt durability for purchases (2 s debounce path)

        SendXPEvent(player, 0, kXPReasonUnlock);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] player %u unlocked '%s' (%s, %u flux)", player, def->key,
                       def->name, def->fluxCost);
        return TFUnlockResult::Ok;
    }

    TFWeaponAggStats TFProgressionSystem::GetStats(PlayerId player, WeaponId weapon) const
    {
        const std::string* key = WeaponKeyOf(weapon);
        return key ? GetStatsByKey(player, *key) : TFWeaponAggStats{};
    }

    TFWeaponAggStats TFProgressionSystem::GetStatsByKey(PlayerId player, std::string_view weaponKey) const
    {
        if (const auto* meta = m_meta.Find(player))
        {
            auto it = meta->stats.find(std::string(weaponKey));
            if (it != meta->stats.end())
                return it->second;
        }
        return TFWeaponAggStats{};
    }

    const std::unordered_map<std::string, TFWeaponAggStats>* TFProgressionSystem::AllStats(PlayerId player) const
    {
        const auto* meta = m_meta.Find(player);
        return meta ? &meta->stats : nullptr;
    }

    void TFProgressionSystem::ServerRecordShots(PlayerId player, WeaponId weapon, uint16_t count)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || player == kInvalidPlayer || count == 0)
            return;
        if (const std::string* key = WeaponKeyOf(weapon))
        {
            auto& meta = m_meta.Ensure(player);
            meta.stats[*key].shots += count;
            meta.dirty = true; // meta-only dirt: persists on the periodic sweep
        }
    }

    void TFProgressionSystem::ServerRecordHits(PlayerId player, WeaponId weapon, uint16_t count)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || player == kInvalidPlayer || count == 0)
            return;
        if (const std::string* key = WeaponKeyOf(weapon))
        {
            auto& meta = m_meta.Ensure(player);
            meta.stats[*key].hits += count;
            meta.dirty = true;
        }
    }

    const TFLoadout* TFProgressionSystem::GetLoadout(PlayerId player) const
    {
        const auto* meta = m_meta.Find(player);
        return (meta && meta->loadout.Any()) ? &meta->loadout : nullptr;
    }

    bool TFProgressionSystem::ServerSetLoadout(PlayerId player, const TFLoadout& loadout)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || player == kInvalidPlayer)
            return false;

        // Slot indices mirror TFWeaponSystem's Slot enum semantics: 0=primary,
        // 1=secondary, 2=tool. Empty keys mean "class default" and always pass.
        if (!ValidLoadoutSlotKey(player, loadout.primary, 0) || !ValidLoadoutSlotKey(player, loadout.secondary, 1) ||
            !ValidLoadoutSlotKey(player, loadout.tool, 2))
            return false;

        auto& meta = m_meta.Ensure(player);
        meta.loadout = loadout;
        meta.dirty = true;
        m_dirty = true; // prompt durability (2 s debounce path)
        return true;
    }

    const std::string* TFProgressionSystem::WeaponKeyOf(WeaponId weapon) const
    {
        if (!m_ctx || !m_ctx->data || !m_ctx->data->IsLoaded())
            return nullptr;
        const WeaponDef* def = m_ctx->data->GetWeapon(weapon);
        return def ? &def->key : nullptr;
    }

    bool TFProgressionSystem::ValidLoadoutSlotKey(PlayerId player, const std::string& weaponKey, int slot) const
    {
        if (weaponKey.empty())
            return true; // class default
        if (!m_ctx || !m_ctx->data || !m_ctx->data->IsLoaded())
            return false; // can't validate a concrete key without tables: reject

        const WeaponDef* def = m_ctx->data->GetWeaponByKey(weaponKey);
        if (!def)
            return false;

        // Slot category must match what the slot holds.
        if (slot == 0)
        {
            if (def->slot != "rifle" && def->slot != "carbine" && def->slot != "lmg" && def->slot != "sniper" &&
                def->slot != "shotgun" && def->slot != "launcher")
                return false;
        }
        else if (slot == 1)
        {
            if (def->slot != "pistol")
                return false;
        }
        else
        {
            if (def->slot != "tool")
                return false;
        }

        // Faction pool: common ("ALL" == FactionId::None) or the player's own.
        if (def->faction != FactionId::None && m_ctx->players && def->faction != m_ctx->players->FactionOf(player))
            return false;

        return IsWeaponUnlocked(player, def->id);
    }

    // ---------------------------------------------------------------------------
    // loadout-depth wave: grenade choice + suit slot
    // ---------------------------------------------------------------------------

    void TFProgressionSystem::LoadSuitTable()
    {
        std::string text;
        if (!ReadAllText(kSuitsJsonPath, text))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] suits: cannot open %s (no passives)", kSuitsJsonPath);
            return;
        }
        const Spark::Json::Value root = Spark::Json::Parse(text);
        if (!root.IsObject() || !root.HasKey("suits") || !root["suits"].IsArray())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] suits: %s malformed (no passives)", kSuitsJsonPath);
            return;
        }
        const Spark::Json::Value& suits = root["suits"];
        for (size_t i = 0; i < suits.Size(); ++i)
        {
            const Spark::Json::Value& s = suits[i];
            if (!s.IsObject() || !s.HasKey("key"))
                continue;
            TFSuitDef def;
            def.key = s["key"].AsString({});
            if (def.key.empty())
                continue;
            def.name = s["name"].AsString(def.key);
            def.desc = s["desc"].AsString({});
            if (s.HasKey("shieldMult"))
                def.shieldMult = static_cast<float>(s["shieldMult"].AsNumber(1.0));
            if (s.HasKey("regenDelayMult"))
                def.regenDelayMult = static_cast<float>(s["regenDelayMult"].AsNumber(1.0));
            if (s.HasKey("reserveMult"))
                def.reserveMult = static_cast<float>(s["reserveMult"].AsNumber(1.0));
            if (s.HasKey("healthMult"))
                def.healthMult = static_cast<float>(s["healthMult"].AsNumber(1.0));
            m_suits.emplace(def.key, def);
        }
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] suits: %zu passives loaded from %s", m_suits.size(),
                       kSuitsJsonPath);
    }

    const TFSuitDef* TFProgressionSystem::SuitDefFor(PlayerId player) const
    {
        const TFLoadout* lo = GetLoadout(player);
        if (!lo || lo->suit.empty())
            return nullptr;
        const auto it = m_suits.find(lo->suit);
        return it != m_suits.end() ? &it->second : nullptr;
    }

    float TFProgressionSystem::SuitShieldMult(PlayerId player) const
    {
        const TFSuitDef* d = SuitDefFor(player);
        return d ? d->shieldMult : 1.0f;
    }

    float TFProgressionSystem::SuitRegenDelayMult(PlayerId player) const
    {
        const TFSuitDef* d = SuitDefFor(player);
        return d ? d->regenDelayMult : 1.0f;
    }

    float TFProgressionSystem::SuitReserveMult(PlayerId player) const
    {
        const TFSuitDef* d = SuitDefFor(player);
        return d ? d->reserveMult : 1.0f;
    }

    float TFProgressionSystem::SuitHealthMult(PlayerId player) const
    {
        const TFSuitDef* d = SuitDefFor(player);
        return d ? d->healthMult : 1.0f;
    }

    bool TFProgressionSystem::ValidGrenadeChoiceKey(PlayerId player, const std::string& weaponKey) const
    {
        if (weaponKey.empty())
            return true; // frag_grenade default
        if (weaponKey != kTFGrenadeKeySmoke && weaponKey != kTFGrenadeKeyFlash)
            return false; // only these two are player-selectable; frag is the empty-string default
        // Unlock-gated, mirroring ValidLoadoutSlotKey's weapon check (IsWeaponUnlocked
        // resolves through TFUnlockTree::FindByWeaponKey -> "gr_smoke"/"gr_flash").
        if (!m_ctx || !m_ctx->data || !m_ctx->data->IsLoaded())
            return false; // can't validate a concrete key without tables: reject
        const WeaponDef* def = m_ctx->data->GetWeaponByKey(weaponKey);
        return def && IsWeaponUnlocked(player, def->id);
    }

    bool TFProgressionSystem::ValidSuitChoiceKey(PlayerId player, const std::string& suitKey) const
    {
        if (suitKey.empty())
            return true; // no passive
        if (m_suits.find(suitKey) == m_suits.end())
            return false; // unknown suits.json key
        const TFUnlockDef* node = TFUnlockTree::FindBySuitKey(suitKey);
        if (!node)
            return true; // not in the tree (overshield) == default kit
        return IsUnlocked(player, node->key);
    }

    void TFProgressionSystem::ServerHandleLoadoutExtMsgRaw(PlayerId sender, const void* data, size_t size)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || size < sizeof(TF_LoadoutExtChange) || !data)
            return;
        TF_LoadoutExtChange msg{};
        std::memcpy(&msg, data, sizeof(msg));
        msg.grenadeKey[sizeof(msg.grenadeKey) - 1] = '\0';
        msg.suitKey[sizeof(msg.suitKey) - 1] = '\0';
        ServerSetLoadoutExt(sender, std::string(msg.grenadeKey), std::string(msg.suitKey));
    }

    bool TFProgressionSystem::ServerSetLoadoutExt(PlayerId player, const std::string& grenadeKey,
                                                  const std::string& suitKey)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || player == kInvalidPlayer)
            return false;
        if (!ValidGrenadeChoiceKey(player, grenadeKey) || !ValidSuitChoiceKey(player, suitKey))
            return false;

        // Merges into the existing TFLoadout in place — deliberately never
        // touches primary/secondary/tool (see the header note on why this is
        // a separate entry point from ServerSetLoadout).
        auto& meta = m_meta.Ensure(player);
        meta.loadout.grenade = grenadeKey;
        meta.loadout.suit = suitKey;
        meta.dirty = true;
        m_dirty = true; // prompt durability (2 s debounce path)
        return true;
    }

} // namespace Terrafront
