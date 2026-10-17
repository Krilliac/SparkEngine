/**
 * @file MMOCharacterRecord.h
 * @brief Pipe-delimited character row codec behind MMOPersistenceSystem's character records.
 *
 * MMOPersistenceSystem::LoadCharacter and ListCharactersForAccount decode the
 * "character_<id>" row of the AsyncDatabase store with DecodeCharacterRecord, and
 * BuildCharacterSave writes it with EncodeCharacterRecord. The codec is pure (no
 * database, console or engine context) so the SEC-120 fuzz target
 * (FuzzerTests/FuzzMMOCharacterRecord.cpp) drives exactly the decoder the server runs.
 * Thread affinity: none.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace MMO
{
    /// The identity, location and stat fields a character row stores.
    struct CharacterRecordFields
    {
        std::string name;
        uint32_t accountId = 0;
        int level = 1;
        int xp = 0;
        uint32_t areaId = 1;
        float posX = 0.0f;
        float posY = 1.0f;
        float posZ = 0.0f;
        float rotY = 0.0f;
        float health = 100.0f;
        float maxHealth = 100.0f;
        float mana = 50.0f;
        float maxMana = 50.0f;
        float playTime = 0.0f;
        int currency = 0;
    };

    /**
     * @brief Encode a row as "name|accountId|level|xp|areaId|posX|posY|posZ|rotY|health|maxHealth|mana|maxMana|
     *        playTime|currency", every float in its shortest exact decimal form.
     * @return nullopt when the row could not be decoded back to the same fields: an empty name, a name holding
     *         '|' or an apostrophe, or a non-finite float.
     */
    [[nodiscard]] std::optional<std::string> EncodeCharacterRecord(const CharacterRecordFields& fields);

    /**
     * @brief Decode a stored row: the current 15-field layout, or the legacy 14-field layout without accountId.
     *
     * Rows written by InsertCharacter carry the name as a quoted SQL literal and rows written by an older
     * UpdateCharacter may be quoted whole; both forms are accepted. Every numeric field must be a whole, in-range
     * decimal (no sign on an unsigned field, no trailing text) and every float finite.
     * @return false on any other input; @p out is replaced only on success.
     */
    [[nodiscard]] bool DecodeCharacterRecord(const std::string& stored, CharacterRecordFields& out);
} // namespace MMO
