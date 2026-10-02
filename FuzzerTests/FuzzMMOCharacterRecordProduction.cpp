/**
 * @file FuzzMMOCharacterRecordProduction.cpp
 * @brief libc++-compiled production adapter for the SparkGameMMO character row harness.
 *
 * MMOPersistenceSystem::LoadCharacter and ListCharacters decode the stored
 * "character_<id>" row with MMO::DecodeCharacterRecord before a player enters the
 * world, and BuildCharacterSave writes it back with MMO::EncodeCharacterRecord. The
 * input is the stored row. A violated contract aborts so libFuzzer records a crash:
 *  - a rejected row leaves the caller's record untouched,
 *  - an accepted row has 14 (legacy) or 15 fields after the store's quoting is
 *    undone, a non-empty name without '|' or an apostrophe, finite floats, and every
 *    integer field is a whole decimal token that std::from_chars reads to the decoded
 *    value (no sign on an unsigned field, no trailing text: an independent check),
 *  - an accepted row re-encodes, and the encoded row decodes to bit-identical fields,
 *    so a load -> save -> load cycle never moves or renames the character.
 */

#include "FuzzMMOCharacterRecordProduction.h"

#include "Persistence/MMOCharacterRecord.h"

#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace
{
    constexpr std::size_t kMaxInputBytes = 4096;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzMMOCharacterRecord: %s\n", what);
        std::abort();
    }

    MMO::CharacterRecordFields MakeSentinel()
    {
        MMO::CharacterRecordFields sentinel;
        sentinel.name = "Sentinel";
        sentinel.accountId = 424242;
        sentinel.level = -7;
        sentinel.posX = 3.25f;
        sentinel.currency = 99;
        return sentinel;
    }

    bool SameBits(float left, float right)
    {
        return std::bit_cast<std::uint32_t>(left) == std::bit_cast<std::uint32_t>(right);
    }

    bool Same(const MMO::CharacterRecordFields& a, const MMO::CharacterRecordFields& b)
    {
        return a.name == b.name && a.accountId == b.accountId && a.level == b.level && a.xp == b.xp &&
               a.areaId == b.areaId && SameBits(a.posX, b.posX) && SameBits(a.posY, b.posY) &&
               SameBits(a.posZ, b.posZ) && SameBits(a.rotY, b.rotY) && SameBits(a.health, b.health) &&
               SameBits(a.maxHealth, b.maxHealth) && SameBits(a.mana, b.mana) && SameBits(a.maxMana, b.maxMana) &&
               SameBits(a.playTime, b.playTime) && a.currency == b.currency;
    }

    /// The store's quoting as the persistence layer documents it: one enclosing apostrophe pair
    /// stripped, doubled apostrophes collapsed.
    std::string Unquote(std::string_view text)
    {
        if (text.size() >= 2 && text.front() == '\'' && text.back() == '\'')
        {
            text = text.substr(1, text.size() - 2);
        }
        std::string out;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            out += text[i];
            if (text[i] == '\'' && i + 1 < text.size() && text[i + 1] == '\'')
            {
                ++i;
            }
        }
        return out;
    }

    std::vector<std::string_view> Fields(std::string_view text)
    {
        std::vector<std::string_view> fields;
        std::size_t begin = 0;
        for (std::size_t end = text.find('|'); end != std::string_view::npos; end = text.find('|', begin))
        {
            fields.push_back(text.substr(begin, end - begin));
            begin = end + 1;
        }
        fields.push_back(text.substr(begin));
        return fields;
    }

    template <typename T> void RequireWholeInteger(std::string_view token, T decoded)
    {
        T value{};
        const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
        if (token.empty() || error != std::errc{} || end != token.data() + token.size() || value != decoded)
        {
            InvariantFailure("an accepted integer field is not a whole decimal token of the decoded value");
        }
    }

    void CheckAccepted(const std::string& row, const MMO::CharacterRecordFields& fields)
    {
        const std::string whole = Unquote(row);
        const std::vector<std::string_view> tokens = Fields(whole);
        if (tokens.size() != 14 && tokens.size() != 15)
        {
            InvariantFailure("accepted a row without exactly 14 or 15 fields");
        }
        if (fields.name.empty() || fields.name.find('|') != std::string::npos)
        {
            InvariantFailure("accepted a name a saved row could not hold");
        }
        std::size_t next = 1;
        if (tokens.size() == 15)
        {
            RequireWholeInteger(tokens[next++], fields.accountId);
        }
        RequireWholeInteger(tokens[next], fields.level);
        RequireWholeInteger(tokens[next + 1], fields.xp);
        RequireWholeInteger(tokens[next + 2], fields.areaId);
        RequireWholeInteger(tokens.back(), fields.currency);
        for (const float value : {fields.posX, fields.posY, fields.posZ, fields.rotY, fields.health, fields.maxHealth,
                                  fields.mana, fields.maxMana, fields.playTime})
        {
            if (!std::isfinite(value))
            {
                InvariantFailure("accepted a non-finite location or stat");
            }
        }

        const std::optional<std::string> encoded = MMO::EncodeCharacterRecord(fields);
        if (!encoded)
        {
            InvariantFailure("an accepted row cannot be saved again");
        }
        MMO::CharacterRecordFields reloaded = MakeSentinel();
        if (!MMO::DecodeCharacterRecord(*encoded, reloaded))
        {
            InvariantFailure("the encoder wrote a row the decoder rejects");
        }
        if (!Same(fields, reloaded))
        {
            InvariantFailure("load -> save -> load changed the character");
        }
    }
} // namespace

extern "C" int SparkFuzzDecodeMMOCharacterRecord(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0))
    {
        return 0;
    }
    const std::string row(reinterpret_cast<const char*>(data), size);
    MMO::CharacterRecordFields fields = MakeSentinel();
    if (!MMO::DecodeCharacterRecord(row, fields))
    {
        if (!Same(fields, MakeSentinel()))
        {
            InvariantFailure("a rejected row modified the caller's record");
        }
        return 0;
    }
    CheckAccepted(row, fields);
    return 0;
}
