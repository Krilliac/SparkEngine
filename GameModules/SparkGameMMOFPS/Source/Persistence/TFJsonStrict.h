/** @file TFJsonStrict.h @brief Lexical strictness missing from the DOM JSON parser. */
#pragma once

#include "Utils/JsonUtils.h"

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_set>
#include <vector>

namespace Terrafront::JsonStrict
{
    /**
     * Reject duplicate object keys and non-integral numeric syntax before the
     * DOM parser rounds numbers to double or overwrites duplicate fields.
     * Named fields in @p fractionalFields may use a decimal/exponent token.
     */
    inline bool ValidateLexemes(std::string_view text, std::initializer_list<std::string_view> fractionalFields,
                                std::string& detail)
    {
        struct Container
        {
            std::optional<std::unordered_set<std::string>> keys;
            std::string valueKey;
        };

        std::vector<Container> containers;
        std::string pendingValueKey;
        const auto fractionAllowed = [&](std::string_view key)
        {
            return std::any_of(fractionalFields.begin(), fractionalFields.end(),
                               [key](std::string_view allowed) { return key == allowed; });
        };

        for (size_t i = 0; i < text.size();)
        {
            if (text[i] == '"')
            {
                const size_t start = i++;
                while (i < text.size() && text[i] != '"')
                    i += text[i] == '\\' && i + 1 < text.size() ? 2 : 1;
                if (i < text.size())
                    ++i;

                size_t after = i;
                while (after < text.size() &&
                       (text[after] == ' ' || text[after] == '\t' || text[after] == '\r' || text[after] == '\n'))
                    ++after;
                if (after < text.size() && text[after] == ':')
                {
                    Spark::Json::Value key;
                    if (containers.empty() || !containers.back().keys.has_value() ||
                        !Spark::Json::ParseStrict(text.substr(start, i - start), &key) || !key.IsString() ||
                        !containers.back().keys->insert(key.AsString()).second)
                    {
                        detail = "object contains a duplicate or invalid field name";
                        return false;
                    }
                    pendingValueKey = key.AsString();
                }
                continue;
            }

            if (text[i] == '{' || text[i] == '[')
            {
                Container container;
                if (text[i] == '{')
                    container.keys.emplace();
                container.valueKey = pendingValueKey;
                pendingValueKey.clear();
                containers.push_back(std::move(container));
                ++i;
                continue;
            }
            if (text[i] == '}' || text[i] == ']')
            {
                if (!containers.empty())
                    containers.pop_back();
                pendingValueKey.clear();
                ++i;
                continue;
            }

            const bool startsNumber = text[i] == '-' || (text[i] >= '0' && text[i] <= '9');
            if (!startsNumber)
            {
                if (text[i] == ',')
                    pendingValueKey.clear();
                ++i;
                continue;
            }

            const size_t start = i++;
            while (i < text.size())
            {
                const char c = text[i];
                if (!((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-'))
                    break;
                ++i;
            }
            const std::string_view number = text.substr(start, i - start);
            const std::string_view context =
                !pendingValueKey.empty() ? std::string_view(pendingValueKey)
                                         : (containers.empty() ? std::string_view{} : containers.back().valueKey);
            if (number.front() == '-')
            {
                detail = "numeric fields must be non-negative";
                return false;
            }
            if (number.find_first_of(".eE") != std::string_view::npos && !fractionAllowed(context))
            {
                detail = "integer field uses fractional or exponent syntax";
                return false;
            }
            pendingValueKey.clear();
        }
        return true;
    }
} // namespace Terrafront::JsonStrict
