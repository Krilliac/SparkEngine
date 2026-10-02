/**
 * @file PlatformerProgress.cpp
 * @brief Versioned, bounded save codec for platformer progress.
 *
 * Layout (whitespace-separated tokens, one section per line):
 * @code
 * platformer-progress 1
 * levels <n>
 * level <completed> <unlocked> <stars> <bestTimeBits hex> <bestDeaths> <secretFound>   (n lines)
 * collected <n> <id>...
 * counters <coins> <gems> <stars> <keys>
 * checkpoints <n> <id>...
 * respawn <id>
 * player <lives> <abilityBits>
 * end
 * @endcode
 */

#include "PlatformerProgress.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <charconv>
#include <cmath>
#include <system_error>

namespace Platformer
{
    namespace
    {
        // Ability bit positions in the encoded player section
        constexpr uint32_t kDoubleJumpBit = 1u << 0;
        constexpr uint32_t kWallJumpBit = 1u << 1;
        constexpr uint32_t kDashBit = 1u << 2;
        constexpr uint32_t kGroundPoundBit = 1u << 3;
        constexpr uint32_t kClimbBit = 1u << 4;
        constexpr uint32_t kAllAbilityBits = (1u << 5) - 1;

        /// Sequential reader over space/newline separated tokens.
        class TokenReader
        {
          public:
            explicit TokenReader(std::string_view text) : m_text(text) {}

            bool Next(std::string_view& token)
            {
                SkipSeparators();
                if (m_pos == m_text.size())
                    return false;
                const size_t start = m_pos;
                while (m_pos < m_text.size() && !IsSeparator(m_text[m_pos]))
                    ++m_pos;
                token = m_text.substr(start, m_pos - start);
                return true;
            }

            bool Expect(std::string_view keyword)
            {
                std::string_view token;
                return Next(token) && token == keyword;
            }

            template <typename T> bool Number(T& out, int base = 10)
            {
                std::string_view token;
                if (!Next(token))
                    return false;
                T value{};
                const auto [end, ec] = std::from_chars(token.data(), token.data() + token.size(), value, base);
                if (ec != std::errc{} || end != token.data() + token.size())
                    return false;
                out = value;
                return true;
            }

            bool AtEnd()
            {
                SkipSeparators();
                return m_pos == m_text.size();
            }

          private:
            static bool IsSeparator(char c) { return c == ' ' || c == '\n'; }

            void SkipSeparators()
            {
                while (m_pos < m_text.size() && IsSeparator(m_text[m_pos]))
                    ++m_pos;
            }

            std::string_view m_text;
            size_t m_pos = 0;
        };

        bool Fail(std::string& error, const std::string& reason)
        {
            error = "platformer progress: " + reason;
            return false;
        }

        bool ReadBounded(TokenReader& reader, int minValue, int maxValue, int& out)
        {
            int value = 0;
            if (!reader.Number(value) || value < minValue || value > maxValue)
                return false;
            out = value;
            return true;
        }

        bool ReadFlag(TokenReader& reader, bool& out)
        {
            int value = 0;
            if (!ReadBounded(reader, 0, 1, value))
                return false;
            out = value == 1;
            return true;
        }

        /// Read "<count> <id>..." with non-zero, strictly ascending ids.
        bool ReadIdList(TokenReader& reader, std::vector<uint32_t>& out)
        {
            size_t count = 0;
            if (!reader.Number(count) || count > PlatformerProgress::MAX_IDS)
                return false;
            std::vector<uint32_t> ids;
            ids.reserve(count);
            for (size_t i = 0; i < count; ++i)
            {
                uint32_t id = 0;
                if (!reader.Number(id) || id == 0 || (!ids.empty() && id <= ids.back()))
                    return false;
                ids.push_back(id);
            }
            out = std::move(ids);
            return true;
        }

        void AppendIdList(std::string& text, const std::vector<uint32_t>& ids)
        {
            text += std::to_string(ids.size());
            for (uint32_t id : ids)
                text += ' ' + std::to_string(id);
            text += '\n';
        }

        bool ReadLevel(TokenReader& reader, LevelProgress& level)
        {
            uint32_t timeBits = 0;
            if (!reader.Expect("level") || !ReadFlag(reader, level.completed) || !ReadFlag(reader, level.unlocked) ||
                !ReadBounded(reader, 0, PlatformerProgress::MAX_STARS_PER_LEVEL, level.starsEarned) ||
                !reader.Number(timeBits, 16) ||
                !ReadBounded(reader, 0, PlatformerProgress::MAX_COUNTER, level.bestDeaths) ||
                !ReadFlag(reader, level.secretFound))
                return false;

            level.bestTime = std::bit_cast<float>(timeBits);
            return std::isfinite(level.bestTime) && level.bestTime >= 0.0f &&
                   level.bestTime <= PlatformerProgress::MAX_BEST_TIME;
        }
    } // namespace

    bool PlatformerProgress::IsValidSlotName(std::string_view slotName)
    {
        if (slotName.empty() || slotName.size() > 64)
            return false;
        return std::ranges::all_of(slotName, [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; });
    }

    PlatformerProgressSnapshot PlatformerProgress::Capture(const PlatformerProgressSystems& systems)
    {
        PlatformerProgressSnapshot snapshot;
        snapshot.levels = systems.level->GetProgress();
        snapshot.collection = systems.collectibles->CaptureProgress();
        snapshot.checkpoints = systems.checkpoints->CaptureProgress();
        snapshot.player = systems.player->CaptureProgress();
        return snapshot;
    }

    std::string PlatformerProgress::Serialize(const PlatformerProgressSnapshot& snapshot)
    {
        std::string text = "platformer-progress " + std::to_string(FormatVersion) + '\n';

        text += "levels " + std::to_string(snapshot.levels.size()) + '\n';
        for (const LevelProgress& level : snapshot.levels)
        {
            char timeHex[9] = {};
            const auto timeEnd = std::to_chars(timeHex, timeHex + 8, std::bit_cast<uint32_t>(level.bestTime), 16);
            text += "level " + std::to_string(level.completed ? 1 : 0) + ' ' + std::to_string(level.unlocked ? 1 : 0) +
                    ' ' + std::to_string(level.starsEarned) + ' ' + std::string(timeHex, timeEnd.ptr) + ' ' +
                    std::to_string(level.bestDeaths) + ' ' + std::to_string(level.secretFound ? 1 : 0) + '\n';
        }

        const CollectionProgress& collection = snapshot.collection;
        text += "collected ";
        AppendIdList(text, collection.collectedIds);
        text += "counters " + std::to_string(collection.coins) + ' ' + std::to_string(collection.gems) + ' ' +
                std::to_string(collection.stars) + ' ' + std::to_string(collection.keys) + '\n';

        text += "checkpoints ";
        AppendIdList(text, snapshot.checkpoints.activatedIds);
        text += "respawn " + std::to_string(snapshot.checkpoints.lastActivatedId) + '\n';

        const AbilityFlags& abilities = snapshot.player.abilities;
        const uint32_t abilityBits = (abilities.doubleJump ? kDoubleJumpBit : 0u) |
                                     (abilities.wallJump ? kWallJumpBit : 0u) | (abilities.dash ? kDashBit : 0u) |
                                     (abilities.groundPound ? kGroundPoundBit : 0u) |
                                     (abilities.climb ? kClimbBit : 0u);
        text += "player " + std::to_string(snapshot.player.lives) + ' ' + std::to_string(abilityBits) + '\n';
        text += "end\n";
        return text;
    }

    bool PlatformerProgress::Deserialize(std::string_view text, PlatformerProgressSnapshot& outSnapshot,
                                         std::string& error)
    {
        if (text.size() > MAX_ENCODED_BYTES)
            return Fail(error, "encoded state is " + std::to_string(text.size()) + " bytes (limit " +
                                   std::to_string(MAX_ENCODED_BYTES) + ")");

        TokenReader reader(text);
        uint32_t version = 0;
        if (!reader.Expect("platformer-progress") || !reader.Number(version))
            return Fail(error, "missing format header");
        if (version != FormatVersion)
            return Fail(error, "format version " + std::to_string(version) + " is not supported (this build reads " +
                                   std::to_string(FormatVersion) + ")");

        PlatformerProgressSnapshot snapshot;
        size_t levelCount = 0;
        if (!reader.Expect("levels") || !reader.Number(levelCount) || levelCount == 0 || levelCount > MAX_LEVELS)
            return Fail(error, "bad level count");
        snapshot.levels.resize(levelCount);
        for (size_t i = 0; i < levelCount; ++i)
        {
            if (!ReadLevel(reader, snapshot.levels[i]))
                return Fail(error, "bad progress for level " + std::to_string(i));
        }

        CollectionProgress& collection = snapshot.collection;
        if (!reader.Expect("collected") || !ReadIdList(reader, collection.collectedIds))
            return Fail(error, "bad collected-item list");
        if (!reader.Expect("counters") || !ReadBounded(reader, 0, MAX_COUNTER, collection.coins) ||
            !ReadBounded(reader, 0, MAX_COUNTER, collection.gems) ||
            !ReadBounded(reader, 0, MAX_COUNTER, collection.stars) ||
            !ReadBounded(reader, 0, MAX_COUNTER, collection.keys))
            return Fail(error, "bad collection counters");

        CheckpointProgress& checkpoints = snapshot.checkpoints;
        if (!reader.Expect("checkpoints") || !ReadIdList(reader, checkpoints.activatedIds))
            return Fail(error, "bad activated-checkpoint list");
        if (!reader.Expect("respawn") || !reader.Number(checkpoints.lastActivatedId) ||
            (checkpoints.lastActivatedId != 0 &&
             !std::ranges::binary_search(checkpoints.activatedIds, checkpoints.lastActivatedId)))
            return Fail(error, "respawn checkpoint is not an activated checkpoint");

        uint32_t abilityBits = 0;
        if (!reader.Expect("player") ||
            !ReadBounded(reader, 1, PlatformerPlayerController::MAX_LIVES, snapshot.player.lives) ||
            !reader.Number(abilityBits) || (abilityBits & ~kAllAbilityBits) != 0)
            return Fail(error, "bad player lives or abilities");
        AbilityFlags& abilities = snapshot.player.abilities;
        abilities.doubleJump = (abilityBits & kDoubleJumpBit) != 0;
        abilities.wallJump = (abilityBits & kWallJumpBit) != 0;
        abilities.dash = (abilityBits & kDashBit) != 0;
        abilities.groundPound = (abilityBits & kGroundPoundBit) != 0;
        abilities.climb = (abilityBits & kClimbBit) != 0;

        if (!reader.Expect("end") || !reader.AtEnd())
            return Fail(error, "missing end marker or trailing data");

        outSnapshot = std::move(snapshot);
        error.clear();
        return true;
    }

    bool PlatformerProgress::Validate(const PlatformerProgressSnapshot& snapshot,
                                      const PlatformerProgressSystems& systems, std::string& error)
    {
        const PlatformerLevelSystem& level = *systems.level;
        if (snapshot.levels.size() != level.GetLevelCount())
            return Fail(error, "save has " + std::to_string(snapshot.levels.size()) + " levels, this build defines " +
                                   std::to_string(level.GetLevelCount()));

        int totalStars = 0;
        for (const LevelProgress& progress : snapshot.levels)
            totalStars += progress.starsEarned;

        for (size_t i = 0; i < snapshot.levels.size(); ++i)
        {
            const LevelProgress& progress = snapshot.levels[i];
            const std::string name = "level " + std::to_string(i);
            if (i == 0 && !progress.unlocked)
                return Fail(error, "level 0 must be unlocked");
            if (progress.completed && !progress.unlocked)
                return Fail(error, name + " is completed but locked");
            if (!progress.completed && (progress.starsEarned != 0 || progress.bestTime != 0.0f))
                return Fail(error, name + " has a rating but was never completed");
            if (i > 0 && progress.unlocked &&
                static_cast<int>(level.GetRequiredStarsToUnlock(static_cast<uint32_t>(i))) > totalStars)
                return Fail(error, name + " is unlocked without the stars it requires");
        }

        for (uint32_t id : snapshot.collection.collectedIds)
        {
            if (!systems.collectibles->HasCollectible(id))
                return Fail(error, "unknown collectible id " + std::to_string(id));
        }

        const auto& placed = systems.checkpoints->GetCheckpoints();
        for (uint32_t id : snapshot.checkpoints.activatedIds)
        {
            if (std::ranges::none_of(placed, [id](const CheckpointData& cp) { return cp.id == id; }))
                return Fail(error, "unknown checkpoint id " + std::to_string(id));
        }

        error.clear();
        return true;
    }

    bool PlatformerProgress::Apply(const PlatformerProgressSnapshot& snapshot, const PlatformerProgressSystems& systems,
                                   std::string& error)
    {
        if (!Validate(snapshot, systems, error))
            return false;

        // Validate covers every check the Restore calls make, so they all succeed; should the two ever drift,
        // put every system back to its pre-call state rather than leave a half-applied save.
        const PlatformerProgressSnapshot previous = Capture(systems);
        const auto restoreAll = [&systems](const PlatformerProgressSnapshot& state)
        {
            const bool levelRestored = systems.level->RestoreProgress(state.levels);
            const bool collectionRestored = systems.collectibles->RestoreProgress(state.collection);
            const bool checkpointsRestored = systems.checkpoints->RestoreProgress(state.checkpoints);
            systems.player->RestoreProgress(state.player);
            return levelRestored && collectionRestored && checkpointsRestored;
        };

        if (!restoreAll(snapshot))
        {
            restoreAll(previous);
            return Fail(error, "a gameplay system refused the validated snapshot; previous progress kept");
        }

        error.clear();
        return true;
    }
} // namespace Platformer
