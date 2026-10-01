/**
 * @file MMOPersistenceSystem.cpp
 * @brief Database schema, prepared statements, and save/load implementation
 */

#include "MMOPersistenceSystem.h"
#include "MMOCharacterRecord.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <string_view>
#include <system_error>

namespace MMO
{

    using namespace Spark::Persistence;

    namespace
    {
        /// Upper bound on a stored inventory's slot count; larger values are corrupt.
        constexpr int kMaxInventorySlots = 512;

        constexpr PreparedStatementID Sid(MMOStmtId id)
        {
            return static_cast<PreparedStatementID>(id);
        }

        uint64_t GetTimestamp()
        {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count());
        }

        PreparedStatementParam MakeInt(int64_t v)
        {
            return PreparedStatementParam{QueryValue{v}};
        }

        PreparedStatementParam MakeDouble(double v)
        {
            return PreparedStatementParam{QueryValue{v}};
        }

        PreparedStatementParam MakeString(const std::string& v)
        {
            return PreparedStatementParam{QueryValue{v}};
        }

        // SQLiteConnection::Execute single-quotes string parameters (escaping ' as '')
        // before substitution, and the key-value store keeps that text verbatim.
        // Loads must invert the transformation to recover the original string.
        std::string UnquoteStoredString(std::string s)
        {
            if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'')
            {
                s = s.substr(1, s.size() - 2);
            }
            size_t pos = 0;
            while ((pos = s.find("''", pos)) != std::string::npos)
            {
                s.erase(pos, 1);
                ++pos;
            }
            return s;
        }

        /// Whole-string integer parse; nullopt on empty, trailing text or overflow.
        template <typename T> std::optional<T> ParseInteger(std::string_view text)
        {
            T value{};
            const char* last = text.data() + text.size();
            const auto [ptr, ec] = std::from_chars(text.data(), last, value);
            if (text.empty() || ec != std::errc() || ptr != last)
            {
                return std::nullopt;
            }
            return value;
        }

        std::optional<float> ParseFiniteFloat(const std::string& text)
        {
            try
            {
                size_t used = 0;
                const float value = std::stof(text, &used);
                if (used != text.size() || !std::isfinite(value))
                {
                    return std::nullopt;
                }
                return value;
            }
            catch (const std::exception&)
            {
                return std::nullopt;
            }
        }

        /// Fields between separators. Empty fields, a trailing one included, are
        /// kept (an empty guild MOTD is a field); empty text has no fields.
        std::vector<std::string> Split(const std::string& text, char separator)
        {
            std::vector<std::string> parts;
            if (text.empty())
            {
                return parts;
            }
            size_t begin = 0;
            for (size_t end = text.find(separator); end != std::string::npos; end = text.find(separator, begin))
            {
                parts.push_back(text.substr(begin, end - begin));
                begin = end + 1;
            }
            parts.push_back(text.substr(begin));
            return parts;
        }

        /// Percent-escape '%' and the '|' field separator in free text.
        std::string EscapeField(const std::string& text)
        {
            std::string escaped;
            escaped.reserve(text.size());
            for (const char c : text)
            {
                if (c == '%')
                {
                    escaped += "%25";
                }
                else if (c == '|')
                {
                    escaped += "%7C";
                }
                else
                {
                    escaped += c;
                }
            }
            return escaped;
        }

        /// Inverse of EscapeField; nullopt on any other '%' sequence.
        std::optional<std::string> UnescapeField(const std::string& text)
        {
            std::string plain;
            plain.reserve(text.size());
            for (size_t i = 0; i < text.size(); ++i)
            {
                if (text[i] != '%')
                {
                    plain += text[i];
                    continue;
                }
                const std::string_view code = std::string_view(text).substr(i + 1, 2);
                if (code == "25")
                {
                    plain += '%';
                }
                else if (code == "7C")
                {
                    plain += '|';
                }
                else
                {
                    return std::nullopt;
                }
                i += 2;
            }
            return plain;
        }

        /// First column of a single-row KV result as a string, or nullptr.
        const std::string* FirstString(const QueryResult& result)
        {
            if (!result.success || result.rows.empty() || result.rows[0].columns.empty())
            {
                return nullptr;
            }
            return std::get_if<std::string>(&result.rows[0].columns[0]);
        }

        /// Parse a stored character row into the identity/location/stat fields of @p out,
        /// which is left untouched when the row does not decode.
        bool ParseCharacterRecord(const std::string& stored, CharacterSaveData& out)
        {
            CharacterRecordFields fields;
            if (!DecodeCharacterRecord(stored, fields))
            {
                return false;
            }
            out.name = std::move(fields.name);
            out.accountId = fields.accountId;
            out.level = fields.level;
            out.xp = fields.xp;
            out.areaId = fields.areaId;
            out.posX = fields.posX;
            out.posY = fields.posY;
            out.posZ = fields.posZ;
            out.rotY = fields.rotY;
            out.health = fields.health;
            out.maxHealth = fields.maxHealth;
            out.mana = fields.mana;
            out.maxMana = fields.maxMana;
            out.playTime = fields.playTime;
            out.inventory.currency = fields.currency;
            return true;
        }

        /// The row BuildCharacterSave stores, or nullopt when it could not be loaded back.
        std::optional<std::string> EncodeCharacterRow(const CharacterSaveData& data)
        {
            CharacterRecordFields fields;
            fields.name = data.name;
            fields.accountId = data.accountId;
            fields.level = data.level;
            fields.xp = data.xp;
            fields.areaId = data.areaId;
            fields.posX = data.posX;
            fields.posY = data.posY;
            fields.posZ = data.posZ;
            fields.rotY = data.rotY;
            fields.health = data.health;
            fields.maxHealth = data.maxHealth;
            fields.mana = data.mana;
            fields.maxMana = data.maxMana;
            fields.playTime = data.playTime;
            fields.currency = data.inventory.currency;
            return EncodeCharacterRecord(fields);
        }

        /// "1|maxSlots|maxWeight|slotCount|slot:itemDefId:count,..." — every slot
        /// keeps its index, so empty gaps and the slot count survive a reload.
        std::string EncodeInventoryRecord(const InventoryData& inv)
        {
            std::string text = std::format("1|{}|{}|{}|", inv.maxSlots, inv.maxWeight, inv.slots.size());
            bool first = true;
            for (size_t i = 0; i < inv.slots.size(); ++i)
            {
                const ItemStack& slot = inv.slots[i];
                if (slot.IsEmpty())
                {
                    continue;
                }
                text += std::format("{}{}:{}:{}", first ? "" : ",", i, slot.itemDefId, slot.count);
                first = false;
            }
            return text;
        }

        bool ParseInventoryRecord(const std::string& text, InventoryData& out)
        {
            const std::vector<std::string> fields = Split(text, '|');
            if ((fields.size() != 4 && fields.size() != 5) || fields[0] != "1")
            {
                return false;
            }
            const auto maxSlots = ParseInteger<int>(fields[1]);
            const auto maxWeight = ParseFiniteFloat(fields[2]);
            const auto slotCount = ParseInteger<int>(fields[3]);
            if (!maxSlots || *maxSlots < 0 || *maxSlots > kMaxInventorySlots || !maxWeight || !slotCount ||
                *slotCount < 0 || *slotCount > kMaxInventorySlots)
            {
                return false;
            }

            out.maxSlots = *maxSlots;
            out.maxWeight = *maxWeight;
            out.slots.assign(static_cast<size_t>(*slotCount), ItemStack{});
            if (fields.size() == 4)
            {
                return true;
            }
            for (const std::string& entry : Split(fields[4], ','))
            {
                const std::vector<std::string> parts = Split(entry, ':');
                if (parts.size() != 3)
                {
                    return false;
                }
                const auto index = ParseInteger<int>(parts[0]);
                const auto itemDefId = ParseInteger<uint32_t>(parts[1]);
                const auto count = ParseInteger<int>(parts[2]);
                if (!index || *index < 0 || *index >= *slotCount || !itemDefId || *itemDefId == 0 || !count ||
                    *count <= 0)
                {
                    return false;
                }
                ItemStack& slot = out.slots[static_cast<size_t>(*index)];
                if (!slot.IsEmpty())
                {
                    return false; // the same slot listed twice
                }
                slot.itemDefId = *itemDefId;
                slot.count = *count;
            }
            return true;
        }

        /// The text Execute() substitutes for a string parameter, which is also
        /// what a key built from one holds (achievement stat names).
        std::string QuoteLiteral(const std::string& text)
        {
            std::string quoted = "'";
            for (const char c : text)
            {
                quoted += c;
                if (c == '\'')
                {
                    quoted += '\'';
                }
            }
            return quoted + "'";
        }

        /// Rebuild the bound parameters that produced a stored key suffix:
        /// '_'-separated integers (slot, faction, dungeon and difficulty) or one
        /// quoted string literal (achievement stat names).
        std::optional<std::vector<PreparedStatementParam>> SuffixParams(const std::string& suffix)
        {
            std::vector<PreparedStatementParam> params;
            if (suffix.size() >= 2 && suffix.front() == '\'' && suffix.back() == '\'')
            {
                params.push_back(MakeString(UnquoteStoredString(suffix)));
                return params;
            }
            for (const std::string& piece : Split(suffix, '_'))
            {
                const auto value = ParseInteger<int64_t>(piece);
                if (!value)
                {
                    return std::nullopt;
                }
                params.push_back(MakeInt(*value));
            }
            if (params.empty())
            {
                return std::nullopt;
            }
            return params;
        }

        /// A family of records keyed "<prefix><owner>_<suffix>" (or "<prefix><suffix>"
        /// when they have no owner): the statement that lists them and the one
        /// that deletes one.
        struct KeyFamily
        {
            const char* prefix;
            MMOStmtId list;
            MMOStmtId remove;
        };

        // Indices into kCharacterFamilies: the family of a character's StoredKey.
        constexpr size_t kReputationFamily = 1;
        constexpr size_t kAchievementFamily = 2;
        constexpr size_t kAchievementStatFamily = 3;
        constexpr size_t kCraftingSkillFamily = 4;
        constexpr size_t kKnownRecipeFamily = 5;
        constexpr size_t kLockoutFamily = 6;

        // Family 0 holds the per-slot inventory keys of earlier builds: never
        // written, so every save deletes the ones it finds.
        constexpr std::array<KeyFamily, 7> kCharacterFamilies{{
            {"inv_", MMOStmtId::ListLegacyInventorySlots, MMOStmtId::DeleteLegacyInventorySlot},
            {"rep_", MMOStmtId::LoadReputation, MMOStmtId::DeleteReputation},
            {"ach_", MMOStmtId::LoadAchievements, MMOStmtId::DeleteAchievement},
            {"achstat_", MMOStmtId::LoadAchievementStats, MMOStmtId::DeleteAchievementStat},
            {"craft_", MMOStmtId::LoadCraftingSkills, MMOStmtId::DeleteCraftingSkill},
            {"recipe_", MMOStmtId::LoadKnownRecipes, MMOStmtId::DeleteKnownRecipe},
            {"lockout_", MMOStmtId::LoadLockouts, MMOStmtId::DeleteLockout},
        }};

        /// Append a delete for every key in @p stored that is not in @p keep.
        /// @p ownerParams are bound ahead of the parameters rebuilt from the suffix.
        template <typename KeySetT>
        void AppendKeyDeletes(Transaction& tx, std::span<const KeyFamily> families, const KeySetT& stored,
                              const KeySetT& keep, const std::vector<PreparedStatementParam>& ownerParams)
        {
            for (const auto& key : stored)
            {
                if (keep.contains(key))
                {
                    continue;
                }
                auto params = SuffixParams(key.second);
                if (!params)
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Game,
                                   "MMOPersistence: cannot address stored %s key '%s' for delete",
                                   families[key.first].prefix, key.second.c_str());
                    continue;
                }
                params->insert(params->begin(), ownerParams.begin(), ownerParams.end());
                tx.Append(Sid(families[key.first].remove), std::move(*params));
            }
        }

        // Guild records: "guild_<guildId>" and "gm_<guildId>_<playerId>".
        constexpr size_t kGuildFamily = 0;
        constexpr size_t kGuildMemberFamily = 1;
        constexpr std::array<KeyFamily, 2> kGuildFamilies{{
            {"guild_", MMOStmtId::LoadGuilds, MMOStmtId::DeleteGuild},
            {"gm_", MMOStmtId::LoadGuildMembers, MMOStmtId::DeleteGuildMember},
        }};

        /// "1|name|tag|leaderId|bankCurrency|maxMembers|motd", free text escaped.
        std::string EncodeGuildRecord(const Guild& guild)
        {
            return std::format("1|{}|{}|{}|{}|{}|{}", EscapeField(guild.name), EscapeField(guild.tag), guild.leaderId,
                               guild.bankCurrency, guild.maxMembers, EscapeField(guild.motd));
        }

        bool ParseGuildRecord(const std::string& text, Guild& out)
        {
            const std::vector<std::string> fields = Split(text, '|');
            if (fields.size() != 7 || fields[0] != "1")
            {
                return false;
            }
            auto name = UnescapeField(fields[1]);
            auto tag = UnescapeField(fields[2]);
            const auto leaderId = ParseInteger<uint32_t>(fields[3]);
            const auto bankCurrency = ParseInteger<int>(fields[4]);
            const auto maxMembers = ParseInteger<int>(fields[5]);
            auto motd = UnescapeField(fields[6]);
            if (!name || name->empty() || !tag || !leaderId || !bankCurrency || *bankCurrency < 0 || !maxMembers ||
                *maxMembers <= 0 || !motd)
            {
                return false;
            }
            out.name = std::move(*name);
            out.tag = std::move(*tag);
            out.leaderId = *leaderId;
            out.bankCurrency = *bankCurrency;
            out.maxMembers = *maxMembers;
            out.motd = std::move(*motd);
            return true;
        }

        /// "1|rank|name", the name escaped.
        std::string EncodeGuildMemberRecord(const GuildMember& member)
        {
            return std::format("1|{}|{}", static_cast<int>(member.rank), EscapeField(member.name));
        }

        bool ParseGuildMemberRecord(const std::string& text, GuildMember& out)
        {
            const std::vector<std::string> fields = Split(text, '|');
            if (fields.size() != 3 || fields[0] != "1")
            {
                return false;
            }
            const auto rank = ParseInteger<int>(fields[1]);
            auto name = UnescapeField(fields[2]);
            if (!rank || *rank < 0 || *rank >= static_cast<int>(GuildRank::Count) || !name)
            {
                return false;
            }
            out.rank = static_cast<GuildRank>(*rank);
            out.name = std::move(*name);
            return true;
        }

        /// Two '_'-separated IDs (guild and player, or boss and kill time).
        template <typename First, typename Second>
        std::optional<std::pair<First, Second>> ParseIdPair(const std::string& suffix)
        {
            const std::vector<std::string> parts = Split(suffix, '_');
            const auto first = parts.size() == 2 ? ParseInteger<First>(parts[0]) : std::nullopt;
            const auto second = parts.size() == 2 ? ParseInteger<Second>(parts[1]) : std::nullopt;
            if (!first || !second)
            {
                return std::nullopt;
            }
            return std::make_pair(*first, *second);
        }
    } // namespace

    // =========================================================================
    // Initialization
    // =========================================================================

    bool MMOPersistenceSystem::Initialize(Spark::IEngineContext* context, const std::string& dbPath)
    {
        if (m_initialized || m_db)
        {
            Shutdown();
        }

        m_context = context;
        m_db = std::make_unique<AsyncDatabasePool>();

        // One worker: every query already serializes on the pool's single shared
        // connection, and a single worker also runs queued saves in submission
        // order, so an older auto-save can never land on top of a newer one.
        if (!m_db->Open(dbPath, 1))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "Failed to open MMO database: %s", dbPath.c_str());
            Spark::SimpleConsole::GetInstance().LogError("[MMO] Failed to open database: " + dbPath);
            return false;
        }

        RegisterPreparedStatements();
        CreateSchema();
        SeedCharacterIdCounter();

        m_initialized = true;
        m_autoSaveTimer = m_autoSaveInterval;

        SPARK_LOG_INFO(Spark::LogCategory::Game, "MMO persistence system initialized (db: %s)", dbPath.c_str());
        Spark::SimpleConsole::GetInstance().LogInfo("[MMO] Persistence system initialized (db: " + dbPath + ")");
        return true;
    }

    void MMOPersistenceSystem::Shutdown()
    {
        if (m_db)
        {
            // Close drains every queued save before the workers stop.
            m_db->Close();
            m_db.reset();
        }
        m_context = nullptr;
        m_autoSaveTimer = 0.0f;
        m_initialized = false;
        m_nextCharacterId = 1;
        m_characterKeys.clear();
        m_guildKeys.reset();
    }

    void MMOPersistenceSystem::Update(float dt)
    {
        if (!m_initialized || !m_db)
        {
            return;
        }

        // Process async callback results
        m_db->ProcessCallbacks();

        // Auto-save timer (module is responsible for triggering the actual save)
        if (dt > 0.0f)
        {
            m_autoSaveTimer -= dt;
        }
    }

    void MMOPersistenceSystem::RegisterPreparedStatements()
    {
        // Character table
        m_db->PrepareStatement(Sid(MMOStmtId::CreateCharacterTable),
                               "SET __schema_characters CREATE TABLE characters (id INTEGER PRIMARY KEY, "
                               "account_id INTEGER, name TEXT, level INTEGER, xp INTEGER, "
                               "area_id INTEGER, pos_x REAL, pos_y REAL, pos_z REAL, rot_y REAL, "
                               "health REAL, max_health REAL, mana REAL, max_mana REAL, "
                               "play_time REAL, currency INTEGER, "
                               "created_at INTEGER, last_login INTEGER, last_save INTEGER)");

        // Placeholders use the engine's zero-based "?N" syntax — the only form
        // SQLiteConnection::Execute substitutes (AsyncDatabase.cpp).
        m_db->PrepareStatement(Sid(MMOStmtId::InsertCharacter),
                               "SET character_?0 ?1|?2|1|0|1|0.0|1.0|0.0|0.0|100.0|100.0|50.0|50.0|0.0|0");

        // ?1 is the full pipe-delimited character blob built by the save paths.
        m_db->PrepareStatement(Sid(MMOStmtId::UpdateCharacter), "SET character_?0 ?1");
        m_db->PrepareStatement(Sid(MMOStmtId::LoadCharacter), "GET character_?0");
        m_db->PrepareStatement(Sid(MMOStmtId::DeleteCharacter), "DELETE character_?0");
        m_db->PrepareStatement(Sid(MMOStmtId::ListCharacters), "KEYS character_");
        m_db->PrepareStatement(Sid(MMOStmtId::LoadNextCharacterId), "GET meta_next_character_id");
        m_db->PrepareStatement(Sid(MMOStmtId::SaveNextCharacterId), "SET meta_next_character_id ?0");

        // Inventory (?1 is the record built by EncodeInventoryRecord)
        m_db->PrepareStatement(Sid(MMOStmtId::SaveInventory), "SET inventory_?0 ?1");
        m_db->PrepareStatement(Sid(MMOStmtId::LoadInventory), "GET inventory_?0");
        m_db->PrepareStatement(Sid(MMOStmtId::DeleteInventory), "DELETE inventory_?0");
        m_db->PrepareStatement(Sid(MMOStmtId::ListLegacyInventorySlots), "KEYS inv_?0_");
        m_db->PrepareStatement(Sid(MMOStmtId::DeleteLegacyInventorySlot), "DELETE inv_?0_?1");
        m_db->PrepareStatement(Sid(MMOStmtId::SaveCurrency), "SET currency_?0 ?1");
        m_db->PrepareStatement(Sid(MMOStmtId::LoadCurrency), "GET currency_?0");
        m_db->PrepareStatement(Sid(MMOStmtId::DeleteCurrency), "DELETE currency_?0");

        // Reputation
        m_db->PrepareStatement(Sid(MMOStmtId::SaveReputation), "SET rep_?0_?1 ?2");
        m_db->PrepareStatement(Sid(MMOStmtId::LoadReputation), "KEYS rep_?0_");
        m_db->PrepareStatement(Sid(MMOStmtId::LoadReputationValue), "GET rep_?0_?1");
        m_db->PrepareStatement(Sid(MMOStmtId::DeleteReputation), "DELETE rep_?0_?1");

        // Achievements
        m_db->PrepareStatement(Sid(MMOStmtId::SaveAchievement), "SET ach_?0_?1 1");
        m_db->PrepareStatement(Sid(MMOStmtId::LoadAchievements), "KEYS ach_?0_");
        m_db->PrepareStatement(Sid(MMOStmtId::DeleteAchievement), "DELETE ach_?0_?1");
        m_db->PrepareStatement(Sid(MMOStmtId::SaveAchievementStat), "SET achstat_?0_?1 ?2");
        m_db->PrepareStatement(Sid(MMOStmtId::LoadAchievementStats), "KEYS achstat_?0_");
        m_db->PrepareStatement(Sid(MMOStmtId::LoadAchievementStatValue), "GET achstat_?0_?1");
        m_db->PrepareStatement(Sid(MMOStmtId::DeleteAchievementStat), "DELETE achstat_?0_?1");

        // Crafting
        m_db->PrepareStatement(Sid(MMOStmtId::SaveCraftingSkill), "SET craft_?0_?1 ?2|?3");
        m_db->PrepareStatement(Sid(MMOStmtId::LoadCraftingSkills), "KEYS craft_?0_");
        m_db->PrepareStatement(Sid(MMOStmtId::LoadCraftingSkillValue), "GET craft_?0_?1");
        m_db->PrepareStatement(Sid(MMOStmtId::DeleteCraftingSkill), "DELETE craft_?0_?1");
        m_db->PrepareStatement(Sid(MMOStmtId::SaveKnownRecipe), "SET recipe_?0_?1 1");
        m_db->PrepareStatement(Sid(MMOStmtId::LoadKnownRecipes), "KEYS recipe_?0_");
        m_db->PrepareStatement(Sid(MMOStmtId::DeleteKnownRecipe), "DELETE recipe_?0_?1");

        // Guilds (?1 / ?2 are the records EncodeGuildRecord / EncodeGuildMemberRecord build)
        m_db->PrepareStatement(Sid(MMOStmtId::SaveGuild), "SET guild_?0 ?1");
        m_db->PrepareStatement(Sid(MMOStmtId::LoadGuilds), "KEYS guild_");
        m_db->PrepareStatement(Sid(MMOStmtId::LoadGuildValue), "GET guild_?0");
        m_db->PrepareStatement(Sid(MMOStmtId::DeleteGuild), "DELETE guild_?0");
        m_db->PrepareStatement(Sid(MMOStmtId::SaveGuildMember), "SET gm_?0_?1 ?2");
        m_db->PrepareStatement(Sid(MMOStmtId::LoadGuildMembers), "KEYS gm_");
        m_db->PrepareStatement(Sid(MMOStmtId::LoadGuildMemberValue), "GET gm_?0_?1");
        m_db->PrepareStatement(Sid(MMOStmtId::DeleteGuildMember), "DELETE gm_?0_?1");
        m_db->PrepareStatement(Sid(MMOStmtId::LoadNextGuildId), "GET meta_next_guild_id");
        m_db->PrepareStatement(Sid(MMOStmtId::SaveNextGuildId), "SET meta_next_guild_id ?0");

        // Lockouts
        m_db->PrepareStatement(Sid(MMOStmtId::SaveLockout), "SET lockout_?0_?1_?2 ?3");
        m_db->PrepareStatement(Sid(MMOStmtId::LoadLockouts), "KEYS lockout_?0_");
        m_db->PrepareStatement(Sid(MMOStmtId::LoadLockoutValue), "GET lockout_?0_?1_?2");
        m_db->PrepareStatement(Sid(MMOStmtId::DeleteLockout), "DELETE lockout_?0_?1_?2");
    }

    void MMOPersistenceSystem::CreateSchema()
    {
        // Execute schema creation (the key-value store doesn't need DDL,
        // but this ensures the schema marker exists for future migration checks)
        m_db->SyncQuery(Sid(MMOStmtId::CreateCharacterTable));
    }

    void MMOPersistenceSystem::SeedCharacterIdCounter()
    {
        // The stored counter covers IDs whose characters were later deleted; the
        // key scan covers stores written before the counter existed (including
        // the timestamp-derived IDs earlier builds generated).
        uint64_t next = 1;
        const QueryResult counter = m_db->SyncQuery(Sid(MMOStmtId::LoadNextCharacterId));
        if (const std::string* stored = FirstString(counter))
        {
            if (const auto value = ParseInteger<uint64_t>(*stored))
            {
                next = std::max(next, *value);
            }
            else
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game, "MMOPersistence: corrupt character ID counter '%s'",
                                stored->c_str());
            }
        }

        const QueryResult keys = m_db->SyncQuery(Sid(MMOStmtId::ListCharacters));
        for (const auto& row : keys.rows)
        {
            const std::string* key = row.columns.empty() ? nullptr : std::get_if<std::string>(&row.columns[0]);
            if (!key || !key->starts_with("character_"))
            {
                continue;
            }
            if (const auto id = ParseInteger<uint32_t>(std::string_view(*key).substr(10)))
            {
                next = std::max<uint64_t>(next, uint64_t{*id} + 1);
            }
        }
        m_nextCharacterId = next;
    }

    // =========================================================================
    // Character CRUD
    // =========================================================================

    uint32_t MMOPersistenceSystem::AllocateCharacterId()
    {
        if (!m_initialized)
        {
            return 0;
        }
        if (m_nextCharacterId > std::numeric_limits<uint32_t>::max())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "MMOPersistence: character ID space exhausted");
            return 0;
        }

        // Persist the advanced counter before handing the ID out, so a crash
        // after this point can never reissue it to a second character.
        const uint64_t next = m_nextCharacterId + 1;
        const QueryResult saved =
            m_db->SyncQuery(Sid(MMOStmtId::SaveNextCharacterId), {MakeInt(static_cast<int64_t>(next))});
        if (!saved.success)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "MMOPersistence: could not persist character ID counter: %s",
                            saved.errorMessage.c_str());
            return 0;
        }
        const auto id = static_cast<uint32_t>(m_nextCharacterId);
        m_nextCharacterId = next;
        return id;
    }

    uint32_t MMOPersistenceSystem::CreateCharacter(const std::string& name, uint32_t accountId)
    {
        const uint32_t charId = AllocateCharacterId();
        if (charId == 0)
        {
            return 0;
        }

        auto result =
            m_db->SyncQuery(Sid(MMOStmtId::InsertCharacter), {MakeInt(charId), MakeString(name), MakeInt(accountId)});

        if (result.success)
        {
            Spark::SimpleConsole::GetInstance().LogInfo("[MMO] Character created: " + name + " (ID " +
                                                        std::to_string(charId) + ")");
        }
        return result.success ? charId : 0;
    }

    bool MMOPersistenceSystem::LoadCharacter(uint32_t characterId, CharacterSaveData& outData)
    {
        if (!m_initialized)
        {
            return false;
        }

        const QueryResult row = m_db->SyncQuery(Sid(MMOStmtId::LoadCharacter), {MakeInt(characterId)});
        const std::string* stored = FirstString(row);
        if (!stored)
        {
            return false;
        }

        outData.characterId = characterId;
        if (!ParseCharacterRecord(*stored, outData))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "MMOPersistence: corrupt or incomplete character data for ID %u",
                            characterId);
            return false;
        }

        // A corrupt inventory fails the load: entering the world with an empty
        // bag would let the next save overwrite the stored items.
        if (!LoadInventory(characterId, outData.inventory))
        {
            return false;
        }
        LoadReputationState(characterId, outData.reputationState);
        LoadAchievementState(characterId, outData.achievementState);
        LoadCraftingState(characterId, outData.craftingState);
        LoadLockouts(characterId, outData.dungeonState);

        outData.lastLogin = GetTimestamp();
        m_totalLoads++;

        SPARK_LOG_INFO(Spark::LogCategory::Game, "Character loaded: %s (Lv%d, ID %u)", outData.name.c_str(),
                       outData.level, characterId);
        Spark::SimpleConsole::GetInstance().LogInfo("[MMO] Character loaded: " + outData.name + " (Lv" +
                                                    std::to_string(outData.level) + ")");
        return true;
    }

    std::optional<MMOPersistenceSystem::Transaction> MMOPersistenceSystem::BuildCharacterSave(
        const CharacterSaveData& data)
    {
        // A row LoadCharacter would refuse must never replace the stored one: the
        // character could not log in again.
        const std::optional<std::string> row = EncodeCharacterRow(data);
        if (!row)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game,
                            "MMOPersistence: character %u was not saved: its name or a location/stat value cannot be "
                            "stored",
                            data.characterId);
            return std::nullopt;
        }

        Transaction tx;
        KeySet written;
        tx.Append(Sid(MMOStmtId::UpdateCharacter), {MakeInt(data.characterId), MakeString(*row)});
        SaveInventory(tx, data.characterId, data.inventory);
        SaveReputationState(tx, data.characterId, data.reputationState, written);
        SaveAchievementState(tx, data.characterId, data.achievementState, written);
        SaveCraftingState(tx, data.characterId, data.craftingState, written);
        SaveLockouts(tx, data.characterId, data.dungeonState, written);

        // Delete every record the character may still have that this save did
        // not write. The candidates come from memory, not a store scan, because
        // an earlier save can still be queued: a scan would miss the records it
        // is about to write. The set never shrinks, so a delete lost to a failed
        // transaction is issued again by the next save.
        auto [known, firstSave] = m_characterKeys.try_emplace(data.characterId);
        if (firstSave)
        {
            known->second = ScanCharacterKeys(data.characterId);
        }
        AppendKeyDeletes(tx, kCharacterFamilies, known->second, written, {MakeInt(data.characterId)});
        known->second.insert(written.begin(), written.end());
        return tx;
    }

    void MMOPersistenceSystem::SaveCharacterAsync(const CharacterSaveData& data)
    {
        if (!m_initialized)
        {
            return;
        }

        // One transaction per save: the character row and its subsystem records
        // commit together (one flush), and the single worker applies saves in order.
        std::optional<Transaction> tx = BuildCharacterSave(data);
        if (!tx)
        {
            return;
        }
        m_db->AsyncTransaction(std::move(*tx));

        SPARK_LOG_DEBUG(Spark::LogCategory::Game, "Async save character: %s (ID %u)", data.name.c_str(),
                        data.characterId);
        m_totalSaves++;
        m_lastSaveTime = data.playTime;
    }

    bool MMOPersistenceSystem::SaveCharacterSync(const CharacterSaveData& data)
    {
        if (!m_initialized)
        {
            return false;
        }

        // Queued behind any pending auto-save, so this state is the one that lands last.
        std::optional<Transaction> tx = BuildCharacterSave(data);
        if (!tx)
        {
            return false;
        }
        const QueryResult result = m_db->AsyncTransaction(std::move(*tx)).get();
        if (!result.success)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "MMOPersistence: save failed for character %u: %s",
                            data.characterId, result.errorMessage.c_str());
            return false;
        }
        m_totalSaves++;
        return true;
    }

    std::optional<std::vector<std::string>> MMOPersistenceSystem::ListKeySuffixes(MMOStmtId listStmt, Params params,
                                                                                  const std::string& keyPrefix)
    {
        const QueryResult keys = m_db->SyncQuery(Sid(listStmt), std::move(params));
        if (!keys.success)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "MMOPersistence: listing '%s' keys failed: %s", keyPrefix.c_str(),
                            keys.errorMessage.c_str());
            return std::nullopt;
        }
        std::vector<std::string> suffixes;
        for (const auto& row : keys.rows)
        {
            const std::string* key = row.columns.empty() ? nullptr : std::get_if<std::string>(&row.columns[0]);
            if (key && key->starts_with(keyPrefix))
            {
                suffixes.push_back(key->substr(keyPrefix.size()));
            }
        }
        return suffixes;
    }

    std::optional<std::string> MMOPersistenceSystem::GetValue(MMOStmtId getStmt, Params params)
    {
        const QueryResult result = m_db->SyncQuery(Sid(getStmt), std::move(params));
        const std::string* text = FirstString(result);
        return text ? std::optional<std::string>(*text) : std::nullopt;
    }

    MMOPersistenceSystem::KeySet MMOPersistenceSystem::ScanCharacterKeys(uint32_t charId)
    {
        KeySet keys;
        for (size_t family = 0; family < kCharacterFamilies.size(); ++family)
        {
            const std::string prefix = std::format("{}{}_", kCharacterFamilies[family].prefix, charId);
            const auto suffixes = ListKeySuffixes(kCharacterFamilies[family].list, {MakeInt(charId)}, prefix);
            for (const std::string& suffix : suffixes.value_or(std::vector<std::string>{}))
            {
                keys.emplace(family, suffix);
            }
        }
        return keys;
    }

    bool MMOPersistenceSystem::DeleteCharacter(uint32_t characterId)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Deleting character from persistence: ID %u", characterId);
        if (!m_initialized)
        {
            return false;
        }

        // A read queued behind every pending save: once it completes, no earlier
        // save can still create keys after the key scan below.
        m_db->AsyncQuery(Sid(MMOStmtId::LoadCharacter), {MakeInt(characterId)}).wait();

        Transaction tx;
        tx.Append(Sid(MMOStmtId::DeleteCharacter), {MakeInt(characterId)});
        tx.Append(Sid(MMOStmtId::DeleteInventory), {MakeInt(characterId)});
        tx.Append(Sid(MMOStmtId::DeleteCurrency), {MakeInt(characterId)});
        AppendKeyDeletes(tx, kCharacterFamilies, ScanCharacterKeys(characterId), KeySet{}, {MakeInt(characterId)});
        m_characterKeys.erase(characterId);
        return m_db->AsyncTransaction(std::move(tx)).get().success;
    }

    std::vector<std::pair<uint32_t, std::string>> MMOPersistenceSystem::ListCharacters(uint32_t accountId)
    {
        std::vector<std::pair<uint32_t, std::string>> result;
        if (!m_initialized || accountId == 0)
        {
            return result;
        }

        const QueryResult keys = m_db->SyncQuery(Sid(MMOStmtId::ListCharacters));
        for (const auto& row : keys.rows)
        {
            const std::string* key = row.columns.empty() ? nullptr : std::get_if<std::string>(&row.columns[0]);
            if (!key || !key->starts_with("character_"))
            {
                continue;
            }
            const auto charId = ParseInteger<uint32_t>(std::string_view(*key).substr(10));
            if (!charId)
            {
                continue;
            }
            const QueryResult stored = m_db->SyncQuery(Sid(MMOStmtId::LoadCharacter), {MakeInt(*charId)});
            const std::string* text = FirstString(stored);
            CharacterSaveData record;
            if (text && ParseCharacterRecord(*text, record) && record.accountId == accountId)
            {
                result.emplace_back(*charId, record.name);
            }
        }
        return result;
    }

    // =========================================================================
    // Subsystem Save/Load Helpers
    // =========================================================================

    void MMOPersistenceSystem::SaveInventory(Transaction& tx, uint32_t charId, const InventoryData& inv) const
    {
        tx.Append(Sid(MMOStmtId::SaveInventory), {MakeInt(charId), MakeString(EncodeInventoryRecord(inv))});
        tx.Append(Sid(MMOStmtId::SaveCurrency), {MakeInt(charId), MakeInt(inv.currency)});
    }

    bool MMOPersistenceSystem::LoadInventory(uint32_t charId, InventoryData& inv)
    {
        const QueryResult record = m_db->SyncQuery(Sid(MMOStmtId::LoadInventory), {MakeInt(charId)});
        if (!record.success)
        {
            return false;
        }
        if (!record.rows.empty())
        {
            const std::string* text = FirstString(record);
            InventoryData parsed;
            if (!text || !ParseInventoryRecord(*text, parsed))
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game, "MMOPersistence: corrupt inventory record for character %u",
                                charId);
                return false;
            }
            parsed.currency = inv.currency;
            inv = std::move(parsed);
        }

        const QueryResult currencyRow = m_db->SyncQuery(Sid(MMOStmtId::LoadCurrency), {MakeInt(charId)});
        if (const std::string* currency = FirstString(currencyRow))
        {
            if (const auto value = ParseInteger<int>(*currency))
            {
                inv.currency = *value;
            }
            else
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game, "MMOPersistence: corrupt currency for character %u", charId);
            }
        }
        return true;
    }

    void MMOPersistenceSystem::SaveReputationState(Transaction& tx, uint32_t charId, const ReputationState& state,
                                                   KeySet& written) const
    {
        for (const auto& [factionId, standing] : state.standings)
        {
            tx.Append(Sid(MMOStmtId::SaveReputation),
                      {MakeInt(charId), MakeInt(factionId), MakeInt(standing.reputation)});
            written.emplace(kReputationFamily, std::to_string(factionId));
        }
    }

    void MMOPersistenceSystem::LoadReputationState(uint32_t charId, ReputationState& state)
    {
        // Key "rep_<charId>_<factionId>", value = reputation
        state.standings.clear();
        const std::string prefix = std::format("rep_{}_", charId);
        const auto suffixes = ListKeySuffixes(MMOStmtId::LoadReputation, {MakeInt(charId)}, prefix);
        for (const std::string& suffix : suffixes.value_or(std::vector<std::string>{}))
        {
            const auto factionId = ParseInteger<uint32_t>(suffix);
            const auto text = factionId
                                  ? GetValue(MMOStmtId::LoadReputationValue, {MakeInt(charId), MakeInt(*factionId)})
                                  : std::nullopt;
            const auto reputation = text ? ParseInteger<int>(*text) : std::nullopt;
            if (!factionId || !reputation)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game, "MMOPersistence: corrupt reputation '%s%s'", prefix.c_str(),
                                suffix.c_str());
                continue;
            }
            FactionStanding& standing = state.standings[*factionId];
            standing.factionId = *factionId;
            standing.reputation = *reputation;
            standing.tier = FactionStanding::GetTierForValue(*reputation);
        }
    }

    void MMOPersistenceSystem::SaveAchievementState(Transaction& tx, uint32_t charId, const AchievementState& state,
                                                    KeySet& written) const
    {
        for (uint32_t achId : state.completedIds)
        {
            tx.Append(Sid(MMOStmtId::SaveAchievement), {MakeInt(charId), MakeInt(achId)});
            written.emplace(kAchievementFamily, std::to_string(achId));
        }
        for (const auto& [key, val] : state.stats)
        {
            tx.Append(Sid(MMOStmtId::SaveAchievementStat), {MakeInt(charId), MakeString(key), MakeInt(val)});
            written.emplace(kAchievementStatFamily, QuoteLiteral(key));
        }
    }

    void MMOPersistenceSystem::LoadAchievementState(uint32_t charId, AchievementState& state)
    {
        // Keys "ach_<charId>_<achievementId>" (value 1) and "achstat_<charId>_'<stat>'"
        // (value = counter). The stat key keeps the quotes MakeString added on save.
        state.completedIds.clear();
        state.stats.clear();
        const std::string achPrefix = std::format("ach_{}_", charId);
        const auto achievements = ListKeySuffixes(MMOStmtId::LoadAchievements, {MakeInt(charId)}, achPrefix);
        for (const std::string& suffix : achievements.value_or(std::vector<std::string>{}))
        {
            if (const auto achId = ParseInteger<uint32_t>(suffix))
            {
                state.completedIds.insert(*achId);
            }
            else
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game, "MMOPersistence: corrupt achievement key '%s%s'",
                                achPrefix.c_str(), suffix.c_str());
            }
        }

        const std::string statPrefix = std::format("achstat_{}_", charId);
        const auto stats = ListKeySuffixes(MMOStmtId::LoadAchievementStats, {MakeInt(charId)}, statPrefix);
        for (const std::string& suffix : stats.value_or(std::vector<std::string>{}))
        {
            const std::string statKey = UnquoteStoredString(suffix);
            const auto text = GetValue(MMOStmtId::LoadAchievementStatValue, {MakeInt(charId), MakeString(statKey)});
            const auto value = text ? ParseInteger<int>(*text) : std::nullopt;
            if (!value)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game, "MMOPersistence: corrupt achievement stat '%s%s'",
                                statPrefix.c_str(), suffix.c_str());
                continue;
            }
            state.stats[statKey] = *value;
        }
    }

    void MMOPersistenceSystem::SaveCraftingState(Transaction& tx, uint32_t charId, const CraftingState& state,
                                                 KeySet& written) const
    {
        for (const auto& [discipline, skill] : state.skills)
        {
            tx.Append(Sid(MMOStmtId::SaveCraftingSkill),
                      {MakeInt(charId), MakeInt(discipline), MakeInt(skill.level), MakeInt(skill.currentXP)});
            written.emplace(kCraftingSkillFamily, std::to_string(discipline));
        }
        for (uint32_t recipeId : state.knownRecipes)
        {
            tx.Append(Sid(MMOStmtId::SaveKnownRecipe), {MakeInt(charId), MakeInt(recipeId)});
            written.emplace(kKnownRecipeFamily, std::to_string(recipeId));
        }
    }

    void MMOPersistenceSystem::LoadCraftingState(uint32_t charId, CraftingState& state)
    {
        // Skills: key "craft_<charId>_<discipline>", value "level|xp"
        state.skills.clear();
        state.knownRecipes.clear();
        const int maxLevel = CraftingSkill{}.maxLevel;
        const std::string skillPrefix = std::format("craft_{}_", charId);
        const auto skills = ListKeySuffixes(MMOStmtId::LoadCraftingSkills, {MakeInt(charId)}, skillPrefix);
        for (const std::string& suffix : skills.value_or(std::vector<std::string>{}))
        {
            const auto discipline = ParseInteger<int>(suffix);
            const bool knownDiscipline =
                discipline && *discipline >= 0 && *discipline < static_cast<int>(CraftingDiscipline::Count);
            const auto text = knownDiscipline
                                  ? GetValue(MMOStmtId::LoadCraftingSkillValue, {MakeInt(charId), MakeInt(*discipline)})
                                  : std::nullopt;
            const std::vector<std::string> fields = text ? Split(*text, '|') : std::vector<std::string>{};
            const auto level = fields.size() == 2 ? ParseInteger<int>(fields[0]) : std::nullopt;
            const auto xp = fields.size() == 2 ? ParseInteger<int>(fields[1]) : std::nullopt;
            if (!discipline || !level || *level < 1 || *level > maxLevel || !xp || *xp < 0)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game, "MMOPersistence: corrupt crafting skill '%s%s'",
                                skillPrefix.c_str(), suffix.c_str());
                continue;
            }
            CraftingSkill skill;
            skill.discipline = static_cast<CraftingDiscipline>(*discipline);
            skill.level = *level;
            skill.currentXP = *xp;
            state.skills[*discipline] = skill;
        }

        // Known recipes: key "recipe_<charId>_<recipeId>", value 1
        const std::string recipePrefix = std::format("recipe_{}_", charId);
        const auto recipes = ListKeySuffixes(MMOStmtId::LoadKnownRecipes, {MakeInt(charId)}, recipePrefix);
        for (const std::string& suffix : recipes.value_or(std::vector<std::string>{}))
        {
            if (const auto recipeId = ParseInteger<uint32_t>(suffix))
            {
                state.knownRecipes.push_back(*recipeId);
            }
            else
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game, "MMOPersistence: corrupt recipe key '%s%s'",
                                recipePrefix.c_str(), suffix.c_str());
            }
        }
        std::sort(state.knownRecipes.begin(), state.knownRecipes.end());
    }

    void MMOPersistenceSystem::SaveLockouts(Transaction& tx, uint32_t charId, const DungeonPlayerState& state,
                                            KeySet& written) const
    {
        for (const auto& lo : state.lockouts)
        {
            // An expired lockout is already over; storing it would only revive it.
            if (!std::isfinite(lo.remaining) || lo.remaining <= 0.0f)
            {
                continue;
            }
            const auto difficulty = static_cast<int64_t>(lo.difficulty);
            tx.Append(Sid(MMOStmtId::SaveLockout), {MakeInt(charId), MakeInt(lo.dungeonDefId), MakeInt(difficulty),
                                                    MakeDouble(static_cast<double>(lo.remaining))});
            written.emplace(kLockoutFamily, std::format("{}_{}", lo.dungeonDefId, difficulty));
        }
    }

    void MMOPersistenceSystem::LoadLockouts(uint32_t charId, DungeonPlayerState& state)
    {
        // Key "lockout_<charId>_<dungeonDefId>_<difficulty>", value = seconds remaining
        state.lockouts.clear();
        const std::string prefix = std::format("lockout_{}_", charId);
        const auto suffixes = ListKeySuffixes(MMOStmtId::LoadLockouts, {MakeInt(charId)}, prefix);
        for (const std::string& suffix : suffixes.value_or(std::vector<std::string>{}))
        {
            const std::vector<std::string> parts = Split(suffix, '_');
            const auto dungeonDefId = parts.size() == 2 ? ParseInteger<uint32_t>(parts[0]) : std::nullopt;
            const auto difficulty = parts.size() == 2 ? ParseInteger<int>(parts[1]) : std::nullopt;
            const bool knownDifficulty =
                difficulty && *difficulty >= 0 && *difficulty < static_cast<int>(DungeonDifficulty::Count);
            const auto text = dungeonDefId && knownDifficulty
                                  ? GetValue(MMOStmtId::LoadLockoutValue,
                                             {MakeInt(charId), MakeInt(*dungeonDefId), MakeInt(*difficulty)})
                                  : std::nullopt;
            const auto remaining = text ? ParseFiniteFloat(*text) : std::nullopt;
            if (!dungeonDefId || !difficulty || !remaining)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game, "MMOPersistence: corrupt lockout '%s%s'", prefix.c_str(),
                                suffix.c_str());
                continue;
            }
            if (*remaining <= 0.0f)
            {
                continue; // expired before the record was written; the next save deletes it
            }
            LootLockout lo;
            lo.dungeonDefId = *dungeonDefId;
            lo.difficulty = static_cast<DungeonDifficulty>(*difficulty);
            lo.remaining = *remaining;
            state.lockouts.push_back(lo);
        }
    }

    // =========================================================================
    // World Persistence
    // =========================================================================

    MMOPersistenceSystem::Transaction MMOPersistenceSystem::BuildWorldSave(const WorldSaveData& data)
    {
        Transaction tx;
        KeySet written;
        tx.Append(Sid(MMOStmtId::SaveNextGuildId), {MakeInt(data.nextGuildId)});
        for (const Guild& guild : data.guilds)
        {
            tx.Append(Sid(MMOStmtId::SaveGuild), {MakeInt(guild.id), MakeString(EncodeGuildRecord(guild))});
            written.emplace(kGuildFamily, std::to_string(guild.id));
            for (const GuildMember& member : guild.members)
            {
                tx.Append(Sid(MMOStmtId::SaveGuildMember),
                          {MakeInt(guild.id), MakeInt(member.playerId), MakeString(EncodeGuildMemberRecord(member))});
                written.emplace(kGuildMemberFamily, std::format("{}_{}", guild.id, member.playerId));
            }
        }

        // Disbanded guilds and departed members are deleted from the same kind
        // of in-memory record set as BuildCharacterSave uses, for the same
        // reason: an earlier world save can still be queued.
        if (!m_guildKeys)
        {
            m_guildKeys = ScanGuildKeys();
        }
        AppendKeyDeletes(tx, kGuildFamilies, *m_guildKeys, written, {});
        m_guildKeys->insert(written.begin(), written.end());
        return tx;
    }

    MMOPersistenceSystem::KeySet MMOPersistenceSystem::ScanGuildKeys()
    {
        KeySet keys;
        for (size_t family = 0; family < kGuildFamilies.size(); ++family)
        {
            const auto suffixes = ListKeySuffixes(kGuildFamilies[family].list, {}, kGuildFamilies[family].prefix);
            for (const std::string& suffix : suffixes.value_or(std::vector<std::string>{}))
            {
                keys.emplace(family, suffix);
            }
        }
        return keys;
    }

    void MMOPersistenceSystem::SaveWorldAsync(const WorldSaveData& data)
    {
        if (!m_initialized)
        {
            return;
        }
        m_db->AsyncTransaction(BuildWorldSave(data));
        SPARK_LOG_DEBUG(Spark::LogCategory::Game, "Async save world: %zu guilds", data.guilds.size());
    }

    bool MMOPersistenceSystem::SaveWorldSync(const WorldSaveData& data)
    {
        if (!m_initialized)
        {
            return false;
        }
        const QueryResult result = m_db->AsyncTransaction(BuildWorldSave(data)).get();
        if (!result.success)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "MMOPersistence: world save failed: %s",
                            result.errorMessage.c_str());
            return false;
        }
        return true;
    }

    bool MMOPersistenceSystem::LoadWorld(WorldSaveData& outData)
    {
        outData = WorldSaveData{};
        if (!m_initialized)
        {
            return false;
        }

        WorldSaveData loaded;
        if (const auto counter = GetValue(MMOStmtId::LoadNextGuildId, {}))
        {
            const auto next = ParseInteger<uint32_t>(*counter);
            if (!next || *next == 0)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game, "MMOPersistence: corrupt guild ID counter '%s'",
                                counter->c_str());
                return false;
            }
            loaded.nextGuildId = *next;
        }
        if (!LoadGuilds(loaded))
        {
            return false;
        }

        outData = std::move(loaded);
        Spark::SimpleConsole::GetInstance().LogInfo(
            std::format("[MMO] Loaded {} guilds from database", outData.guilds.size()));
        return true;
    }

    bool MMOPersistenceSystem::LoadGuilds(WorldSaveData& outData)
    {
        const auto guildIds = ListKeySuffixes(MMOStmtId::LoadGuilds, {}, "guild_");
        const auto memberKeys = ListKeySuffixes(MMOStmtId::LoadGuildMembers, {}, "gm_");
        if (!guildIds || !memberKeys)
        {
            return false;
        }

        std::map<uint32_t, Guild> guilds;
        for (const std::string& suffix : *guildIds)
        {
            Guild guild;
            const auto id = ParseInteger<uint32_t>(suffix);
            const auto text = id && *id != 0 ? GetValue(MMOStmtId::LoadGuildValue, {MakeInt(*id)}) : std::nullopt;
            if (!id || !text || !ParseGuildRecord(*text, guild))
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game, "MMOPersistence: corrupt guild record 'guild_%s'",
                                suffix.c_str());
                return false;
            }
            guild.id = *id;
            guilds.emplace(*id, std::move(guild));
        }

        for (const std::string& suffix : *memberKeys)
        {
            GuildMember member;
            const auto ids = ParseIdPair<uint32_t, uint32_t>(suffix);
            const auto text =
                ids ? GetValue(MMOStmtId::LoadGuildMemberValue, {MakeInt(ids->first), MakeInt(ids->second)})
                    : std::nullopt;
            if (!ids || !text || !ParseGuildMemberRecord(*text, member))
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game, "MMOPersistence: corrupt guild member record 'gm_%s'",
                                suffix.c_str());
                return false;
            }
            const auto guild = guilds.find(ids->first);
            if (guild == guilds.end())
            {
                SPARK_LOG_WARN(Spark::LogCategory::Game,
                               "MMOPersistence: member record 'gm_%s' has no guild; the next world save deletes it",
                               suffix.c_str());
                continue;
            }
            member.playerId = ids->second;
            guild->second.members.push_back(std::move(member));
        }

        for (auto& [id, guild] : guilds)
        {
            std::sort(guild.members.begin(), guild.members.end(),
                      [](const GuildMember& a, const GuildMember& b) { return a.playerId < b.playerId; });
            outData.guilds.push_back(std::move(guild));
        }
        return true;
    }

    // =========================================================================
    // Status & Debug
    // =========================================================================

    int MMOPersistenceSystem::GetPendingWrites() const
    {
        return m_db ? m_db->GetPendingQueryCount() : 0;
    }

    std::string MMOPersistenceSystem::GetStatusString() const
    {
        std::ostringstream ss;
        ss << "=== MMO Persistence ===\n";
        ss << "Database: " << (m_initialized ? "Connected" : "Offline") << "\n";
        ss << "Total saves: " << m_totalSaves << "\n";
        ss << "Total loads: " << m_totalLoads << "\n";
        ss << "Pending writes: " << GetPendingWrites() << "\n";
        ss << "Auto-save interval: " << m_autoSaveInterval << "s\n";
        ss << "Next auto-save in: " << static_cast<int>(m_autoSaveTimer) << "s\n";
        return ss.str();
    }

    void MMOPersistenceSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (ImGui::TreeNode("MMO Persistence System"))
        {
            ImGui::Text("Status: %s", m_initialized ? "Connected" : "Offline");
            ImGui::Text("Total Saves: %d | Total Loads: %d", m_totalSaves, m_totalLoads);
            ImGui::Text("Pending Writes: %d", GetPendingWrites());
            ImGui::Text("Auto-save: %.0fs interval (%.0fs until next)", m_autoSaveInterval, m_autoSaveTimer);
            if (m_db)
            {
                ImGui::Text("DB Pool Size: %d", m_db->GetPoolSize());
            }
            ImGui::TreePop();
        }
#endif
    }

} // namespace MMO
