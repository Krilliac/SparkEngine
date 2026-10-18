/**
 * @file MMOCharacterRecord.cpp
 * @brief Character row codec (see MMOCharacterRecord.h).
 */

#include "MMOCharacterRecord.h"

#include "Utils/StringUtils.h"

#include <charconv>
#include <cmath>
#include <format>
#include <string_view>
#include <system_error>
#include <vector>

namespace MMO
{
    namespace
    {
        constexpr size_t kLegacyFieldCount = 14;
        constexpr size_t kFieldCount = 15;

        /// Reverse SQLiteConnection::Execute's quoting: strip one enclosing pair of
        /// apostrophes and turn every doubled apostrophe back into one.
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

        std::vector<std::string> Split(const std::string& text, char separator)
        {
            std::vector<std::string> parts;
            size_t begin = 0;
            for (size_t end = text.find(separator); end != std::string::npos; end = text.find(separator, begin))
            {
                parts.push_back(text.substr(begin, end - begin));
                begin = end + 1;
            }
            parts.push_back(text.substr(begin));
            return parts;
        }

        /// Whole-string decimal; no sign on an unsigned type, no trailing text, no overflow.
        template <typename T> bool ParseInteger(std::string_view text, T& out)
        {
            T value{};
            const char* last = text.data() + text.size();
            const auto [ptr, ec] = std::from_chars(text.data(), last, value);
            if (text.empty() || ec != std::errc() || ptr != last)
            {
                return false;
            }
            out = value;
            return true;
        }

        /// Whole-token finite float: the shortest form std::format writes (subnormals included,
        /// which std::stof refused with out_of_range) and nothing it never writes.
        bool ParseFiniteFloat(std::string_view text, float& out)
        {
            const std::optional<float> value = Spark::StringUtils::ParseFloatingExact<float>(text);
            if (!value || !std::isfinite(*value))
            {
                return false;
            }
            out = *value;
            return true;
        }

        bool IsStorableName(const std::string& name)
        {
            return !name.empty() && name.find('|') == std::string::npos;
        }
    } // namespace

    std::optional<std::string> EncodeCharacterRecord(const CharacterRecordFields& fields)
    {
        const float floats[] = {fields.posX,      fields.posY, fields.posZ,    fields.rotY,    fields.health,
                                fields.maxHealth, fields.mana, fields.maxMana, fields.playTime};
        for (const float value : floats)
        {
            if (!std::isfinite(value))
            {
                return std::nullopt;
            }
        }
        if (!IsStorableName(fields.name))
        {
            return std::nullopt;
        }
        // std::format writes the shortest decimal that reads back to the same float.
        return std::format("{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}", fields.name, fields.accountId, fields.level,
                           fields.xp, fields.areaId, fields.posX, fields.posY, fields.posZ, fields.rotY, fields.health,
                           fields.maxHealth, fields.mana, fields.maxMana, fields.playTime, fields.currency);
    }

    bool DecodeCharacterRecord(const std::string& stored, CharacterRecordFields& out)
    {
        const std::vector<std::string> tokens = Split(UnquoteStoredString(stored), '|');
        if (tokens.size() != kFieldCount && tokens.size() != kLegacyFieldCount)
        {
            return false;
        }
        CharacterRecordFields fields;
        fields.name = UnquoteStoredString(tokens[0]);
        if (!IsStorableName(fields.name))
        {
            return false;
        }
        // The current layout stores accountId after the name; legacy 14-field rows do not.
        size_t next = 1;
        if (tokens.size() == kFieldCount && !ParseInteger(tokens[next++], fields.accountId))
        {
            return false;
        }
        if (!ParseInteger(tokens[next], fields.level) || !ParseInteger(tokens[next + 1], fields.xp) ||
            !ParseInteger(tokens[next + 2], fields.areaId))
        {
            return false;
        }
        next += 3;
        float* const floats[] = {&fields.posX,      &fields.posY, &fields.posZ,    &fields.rotY,    &fields.health,
                                 &fields.maxHealth, &fields.mana, &fields.maxMana, &fields.playTime};
        for (float* value : floats)
        {
            if (!ParseFiniteFloat(tokens[next++], *value))
            {
                return false;
            }
        }
        if (!ParseInteger(tokens[next], fields.currency))
        {
            return false;
        }
        out = std::move(fields);
        return true;
    }
} // namespace MMO
