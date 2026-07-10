/**
 * @file TFOutfitStore.h
 * @brief TERRAFRONT outfit (clan/guild) persistence — atomic-JSON-file backing.
 *
 * OWNERSHIP: outfits lane. Owns its OWN JSON file ("Saves/outfits.json",
 * resolved relative to the working directory exactly like TFDatabase's
 * "Saves/terrafront.db") because TFDatabase.h/.cpp is a contended file this
 * lane must not edit. Follows the same patterns:
 *  - atomic tmp+rename writes (TFDatabase::SaveToDisk),
 *  - corrupt-file quarantine on Open (TFDatabase::Open),
 *  - additive JSON keys, absent keys load as defaults,
 * but flushes on a 2 s dirty-debounce (TFProgressionSystem spirit) instead of
 * eagerly per-mutation — outfit ops can burst (roster churn) and none of them
 * are as loss-critical as account creation. Close()/SaveNow() flush hard.
 *
 * Pure CRUD only: rank policy (who may kick/promote), auto-promotion on
 * leader-leave and disband-on-empty live in TFOutfitSystem, not here, so this
 * store stays unit-testable standalone (TFCharacterSystem pattern).
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Terrafront
{

    enum class TFOutfitRank : uint8_t
    {
        Member = 0,
        Officer = 1,
        Leader = 2,
    };

    inline const char* OutfitRankName(TFOutfitRank r)
    {
        switch (r)
        {
        case TFOutfitRank::Leader:
            return "Leader";
        case TFOutfitRank::Officer:
            return "Officer";
        default:
            return "Member";
        }
    }

    struct TFOutfitMemberRecord
    {
        uint64_t charId = 0;
        std::string name; ///< character name copy so rosters show offline members
        TFOutfitRank rank = TFOutfitRank::Member;
        int64_t joinedAtMs = 0;
    };

    struct TFOutfitRecord
    {
        uint32_t id = 0;
        std::string name; ///< 3-24 chars, unique (case-insensitive)
        std::string tag;  ///< 2-5 chars, unique (case-insensitive)
        int64_t createdAtMs = 0;
        std::vector<TFOutfitMemberRecord> members;

        const TFOutfitMemberRecord* FindMember(uint64_t charId) const;
        TFOutfitMemberRecord* FindMember(uint64_t charId);
        const TFOutfitMemberRecord* Leader() const;
    };

    class TFOutfitStore
    {
      public:
        TFOutfitStore() = default;
        ~TFOutfitStore();

        /// e.g. "Saves/outfits.json" (same working-dir-relative resolution as
        /// TFDatabase::Open("Saves/terrafront.db")). false on failure; a present
        /// but unreadable file is quarantined to <path>.corrupt-<ms>.bak and the
        /// open is refused (never silently wiped — TFDatabase pattern).
        bool Open(const std::string& path);
        void Close(); ///< flush + close (mutations after Close are rejected)
        bool IsOpen() const { return m_open; }

        /// Debounced flush pump — call once per frame (authority only). Writes
        /// 2 s after the most recent mutation (TFProgressionSystem save spirit).
        void Tick(float dt);
        bool SaveNow(); ///< immediate atomic flush (also clears the dirty flag)

        // --- queries (pointers valid until the next mutation; do not hold) ------
        const TFOutfitRecord* FindById(uint32_t id) const;
        const TFOutfitRecord* FindByCharacter(uint64_t charId) const;
        const TFOutfitRecord* FindByName(const std::string& name) const; ///< case-insensitive
        const TFOutfitRecord* FindByTag(const std::string& tag) const;   ///< case-insensitive
        size_t OutfitCount() const { return m_outfits.size(); }
        const std::vector<TFOutfitRecord>& All() const { return m_outfits; }

        // --- mutators (mark dirty; flushed by Tick debounce / SaveNow / Close) --

        /// nullptr if name or tag is already taken (case-insensitive) or the
        /// store is closed. Creator becomes the sole Leader member.
        const TFOutfitRecord* Create(const std::string& name, const std::string& tag, uint64_t leaderCharId,
                                     const std::string& leaderName, int64_t nowMs);

        bool AddMember(uint32_t outfitId, uint64_t charId, const std::string& name, TFOutfitRank rank, int64_t nowMs);
        bool RemoveMember(uint32_t outfitId, uint64_t charId);
        bool SetMemberRank(uint32_t outfitId, uint64_t charId, TFOutfitRank rank);
        bool Disband(uint32_t outfitId);

        /// Refresh the stored character-name copy wherever `charId` is a member
        /// (called when a character enters world, so renames/typos never go
        /// stale). No-op when the char is in no outfit.
        void UpdateMemberName(uint64_t charId, const std::string& name);

      private:
        bool LoadFromDisk();
        bool WriteToDisk() const;
        void MarkDirty();

        std::string m_path;
        bool m_open = false;
        bool m_dirty = false;
        float m_sinceDirty = 0.0f;

        std::vector<TFOutfitRecord> m_outfits;
        uint32_t m_nextOutfitId = 1;
    };

} // namespace Terrafront
