/**
 * @file FuzzEditorPrefabProduction.cpp
 * @brief libc++-compiled production adapter for the .sparkprefab libFuzzer harness.
 *
 * The fuzz input is handed to the shipped SparkEditor::PrefabTextFormat::Parse, the
 * parser PrefabAsset::TryLoad runs on every project prefab and its retained backup.
 * A violation of the parser's contract aborts so libFuzzer records a crash rather
 * than a silent pass:
 *  - every rejection (damaged, too old, or newer) carries a non-empty reason, since
 *    TryLoad shows it to the user as the only explanation,
 *  - an accepted prefab has a non-empty name and non-empty component and property
 *    names (Render requires all three),
 *  - every line of an accepted document is accounted for: the header, name and
 *    count lines, two lines per component, one line per stored property and, for
 *    version 2, the closing `end`, followed only by blank lines. A property line
 *    that was parsed but not stored (a duplicate key that overwrote or was dropped)
 *    fails this count, decided here from the raw text rather than by the parser,
 *  - Render re-emits an accepted prefab as a current-version document that parses
 *    back to the same name, components and values (NaN compares equal to NaN), and
 *    re-rendering that is byte-identical, as the header promises for re-saves.
 *
 * The 4 MiB file cap and the 4096-component / 4096-property caps are enforced by
 * PrefabAsset::TryLoad and Parse respectively; the smoke's -max_len stays below them.
 */

#include "FuzzEditorPrefabProduction.h"

#include "Prefabs/PrefabTextFormat.h"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <variant>
#include <vector>

namespace
{
    namespace Format = SparkEditor::PrefabTextFormat;

    constexpr std::size_t kMaxInputBytes = 64u * 1024u;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzEditorPrefab: PrefabTextFormat::Parse violated: %s\n", what);
        std::abort();
    }

    /// Lines as the parser's reader sees them: split on '\n', no empty segment after a
    /// final '\n', and one trailing '\r' removed.
    std::vector<std::string_view> SplitLines(std::string_view text)
    {
        std::vector<std::string_view> lines;
        std::size_t start = 0;
        while (start < text.size())
        {
            std::size_t end = text.find('\n', start);
            const std::size_t next = end == std::string_view::npos ? text.size() : end + 1;
            if (end == std::string_view::npos)
                end = text.size();
            std::string_view line = text.substr(start, end - start);
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            lines.push_back(line);
            start = next;
        }
        return lines;
    }

    /// Count of document lines before the trailing run of blank (space/tab-only) lines.
    std::size_t ContentLineCount(const std::vector<std::string_view>& lines)
    {
        std::size_t count = lines.size();
        while (count > 0 && lines[count - 1].find_first_not_of(" \t") == std::string_view::npos)
            --count;
        return count;
    }

    int DeclaredVersion(const std::vector<std::string_view>& lines)
    {
        constexpr std::string_view kHeader = "SPARKPREFAB ";
        if (lines.empty() || !lines.front().starts_with(kHeader))
            InvariantFailure("an accepted document has no SPARKPREFAB header");
        const std::string_view digits = lines.front().substr(kHeader.size());
        int version = 0;
        const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), version);
        if (ec != std::errc{} || end != digits.data() + digits.size())
            InvariantFailure("an accepted document has a malformed version");
        return version;
    }

    void CheckEveryLineIsAccounted(const std::string& text, const Format::ParsedPrefab& parsed)
    {
        const std::vector<std::string_view> lines = SplitLines(text);
        const int version = DeclaredVersion(lines);
        if (version < SparkEditor::PrefabAsset::kOldestSupportedPrefabVersion ||
            version > SparkEditor::PrefabAsset::kPrefabFormatVersion)
        {
            InvariantFailure("a version outside the supported window was accepted");
        }

        std::size_t expected = 3 + (version >= 2 ? 1 : 0);
        for (const SparkEditor::SerializedComponent& component : parsed.components)
            expected += 2 + component.properties.size();
        if (ContentLineCount(lines) != expected)
            InvariantFailure("an accepted line was not stored (duplicate or dropped property)");
    }

    bool SameFloat(float left, float right)
    {
        return left == right || (std::isnan(left) && std::isnan(right));
    }

    bool SameDouble(double left, double right)
    {
        return left == right || (std::isnan(left) && std::isnan(right));
    }

    bool SameValue(const SparkEditor::PrefabPropertyValue& left, const SparkEditor::PrefabPropertyValue& right)
    {
        if (left.index() != right.index())
            return false;
        return std::visit(
            [&right](const auto& value)
            {
                using T = std::decay_t<decltype(value)>;
                const T& other = std::get<T>(right);
                if constexpr (std::is_same_v<T, float>)
                    return SameFloat(value, other);
                else if constexpr (std::is_same_v<T, double>)
                    return SameDouble(value, other);
                else if constexpr (std::is_same_v<T, DirectX::XMFLOAT3>)
                    return SameFloat(value.x, other.x) && SameFloat(value.y, other.y) && SameFloat(value.z, other.z);
                else if constexpr (std::is_same_v<T, DirectX::XMFLOAT4>)
                    return SameFloat(value.x, other.x) && SameFloat(value.y, other.y) && SameFloat(value.z, other.z) &&
                           SameFloat(value.w, other.w);
                else
                    return value == other;
            },
            left);
    }

    bool SamePrefab(const Format::ParsedPrefab& left, const Format::ParsedPrefab& right)
    {
        if (left.name != right.name || left.components.size() != right.components.size())
            return false;
        for (std::size_t index = 0; index < left.components.size(); ++index)
        {
            const SparkEditor::SerializedComponent& a = left.components[index];
            const SparkEditor::SerializedComponent& b = right.components[index];
            if (a.typeName != b.typeName || a.properties.size() != b.properties.size())
                return false;
            for (const auto& [name, value] : a.properties)
            {
                const auto other = b.properties.find(name);
                if (other == b.properties.end() || !SameValue(value, other->second))
                    return false;
            }
        }
        return true;
    }

    void CheckNames(const Format::ParsedPrefab& parsed)
    {
        if (parsed.name.empty())
            InvariantFailure("an accepted prefab has an empty name");
        for (const SparkEditor::SerializedComponent& component : parsed.components)
        {
            if (component.typeName.empty())
                InvariantFailure("an accepted component has an empty type name");
            for (const auto& [name, value] : component.properties)
            {
                if (name.empty())
                    InvariantFailure("an accepted property has an empty name");
            }
        }
    }

    void CheckRoundTrip(const Format::ParsedPrefab& parsed)
    {
        const std::string rendered = Format::Render(parsed.name, parsed.components);
        Format::ParsedPrefab reparsed;
        std::string reason;
        if (Format::Parse(rendered, reparsed, reason) != Format::ParseResult::Ok)
            InvariantFailure("Render produced a document Parse rejects");
        if (!SamePrefab(parsed, reparsed))
            InvariantFailure("Parse(Render(prefab)) differs from prefab");
        if (Format::Render(reparsed.name, reparsed.components) != rendered)
            InvariantFailure("re-rendering an unchanged prefab is not byte-identical");
    }
} // namespace

// This C ABI is the only boundary between the libstdc++ libFuzzer executable
// and the libc++-compiled production parser.
extern "C" int SparkFuzzParseEditorPrefab(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes)
        return 0;
    if (data == nullptr && size != 0)
        return 0;

    const std::string text = size == 0 ? std::string() : std::string(reinterpret_cast<const char*>(data), size);
    Format::ParsedPrefab parsed;
    std::string reason;
    const Format::ParseResult result = Format::Parse(text, parsed, reason);
    if (result != Format::ParseResult::Ok)
    {
        if (reason.empty())
            InvariantFailure("a rejected document has no reason");
        return 0;
    }

    CheckNames(parsed);
    CheckEveryLineIsAccounted(text, parsed);
    CheckRoundTrip(parsed);
    return 0;
}
