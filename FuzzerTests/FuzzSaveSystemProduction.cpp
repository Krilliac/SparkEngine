/**
 * @file FuzzSaveSystemProduction.cpp
 * @brief libc++-compiled production adapter for the .spark_save libFuzzer harness.
 *
 * The fuzz input is handed to the shipped Spark::DecodeSaveFileBytes, the parser
 * SaveSystem::ReadFromFile runs for Load, QuickLoad, the retained-copy fallback and
 * the pre-write retained-copy check. A violation of the reader's contract aborts so
 * libFuzzer records a crash rather than a silent pass:
 *  - the decode is transactional: a rejected file leaves the caller's SaveData
 *    exactly as it was,
 *  - an accepted file is inside every shared representation limit (entity and
 *    custom-state counts, uint16 string lengths), is migrated to
 *    kCurrentSaveVersion, carries finite metadata numbers, no explicit
 *    NameComponent and no duplicate component type per entity,
 *  - every accepted Transform parent is -1 or the index of a different saved
 *    entity that has a Transform, decided by an independent decimal parse here
 *    rather than the codec's own validator,
 *  - DecodeSaveMetadataBytes (the GetSaveSlots path) accepts the same file and
 *    reports the same metadata, so a listed slot and a loaded slot never disagree,
 *  - EncodeSaveFileBytes re-encodes the accepted snapshot (DeserializeWorld runs the
 *    same validation, so a file that fails here could never be restored), and that
 *    image decodes to an equal snapshot. Metadata floats are re-rendered with six
 *    significant digits by the writer, so they must agree to that precision and be
 *    stable after one more round trip; every other field must be identical.
 *
 * SaveRepresentationLimits' 512 MiB, 1,000,000-entity and 100,000-entry caps lie far
 * above the smoke's -max_len; entity-count-overclaim.save and
 * metadata-length-overclaim.save pin that a declared count or length is never
 * trusted before the bytes that back it, and -rss_limit_mb fails the smoke if it is.
 */

#include "FuzzSaveSystemProduction.h"

#include "Engine/SaveSystem/SaveFileCodec.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;
    constexpr const char* kSourceName = "fuzz-input.spark_save";

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzSaveSystem: DecodeSaveFileBytes violated: %s\n", what);
        std::abort();
    }

    Spark::SaveData MakeSentinel()
    {
        Spark::SaveData sentinel;
        sentinel.metadata.saveName = "sentinel";
        sentinel.metadata.playTime = 12.5f;
        sentinel.metadata.slotName = "sentinel-slot";
        Spark::SerializedEntity entity;
        entity.entityID = 77;
        entity.name = "sentinel-entity";
        entity.components.push_back({"Transform", {{"x", "1"}}});
        sentinel.entities.push_back(entity);
        sentinel.customState.emplace("sentinel-key", "sentinel-value");
        return sentinel;
    }

    bool SameFloat(float left, float right)
    {
        return left == right || (std::isnan(left) && std::isnan(right));
    }

    /// The writer prints metadata floats with the default six significant digits.
    bool SameAtWriterPrecision(float original, float reloaded)
    {
        if (original == reloaded)
            return true;
        const double tolerance = 1e-5 * std::fabs(static_cast<double>(original));
        return std::fabs(static_cast<double>(original) - static_cast<double>(reloaded)) <= tolerance;
    }

    template <typename FloatEqual>
    bool SameMetadata(const Spark::SaveMetadata& left, const Spark::SaveMetadata& right, FloatEqual floatEqual)
    {
        return left.saveName == right.saveName && left.sceneName == right.sceneName &&
               left.playerClass == right.playerClass && left.version == right.version &&
               left.timestamp == right.timestamp && left.screenshotPath == right.screenshotPath &&
               left.playerKills == right.playerKills && left.playerDeaths == right.playerDeaths &&
               left.slotName == right.slotName && floatEqual(left.playTime, right.playTime) &&
               floatEqual(left.playerHealth, right.playerHealth) && floatEqual(left.playerArmor, right.playerArmor) &&
               floatEqual(left.playerPosition.x, right.playerPosition.x) &&
               floatEqual(left.playerPosition.y, right.playerPosition.y) &&
               floatEqual(left.playerPosition.z, right.playerPosition.z);
    }

    bool SamePayload(const Spark::SaveData& left, const Spark::SaveData& right)
    {
        if (left.customState != right.customState || left.entities.size() != right.entities.size())
            return false;
        for (std::size_t index = 0; index < left.entities.size(); ++index)
        {
            const Spark::SerializedEntity& a = left.entities[index];
            const Spark::SerializedEntity& b = right.entities[index];
            if (a.entityID != b.entityID || a.name != b.name || a.components.size() != b.components.size())
                return false;
            for (std::size_t component = 0; component < a.components.size(); ++component)
            {
                if (a.components[component].typeName != b.components[component].typeName ||
                    a.components[component].properties != b.components[component].properties)
                {
                    return false;
                }
            }
        }
        return true;
    }

    bool FitsWireString(const std::string& text)
    {
        return text.size() <= std::numeric_limits<std::uint16_t>::max();
    }

    /// Decimal parse written independently of the codec: optional '-', then digits only.
    std::optional<long long> ParseDecimal(const std::string& text)
    {
        std::size_t position = 0;
        const bool negative = !text.empty() && text[0] == '-';
        if (negative)
            position = 1;
        if (position == text.size())
            return std::nullopt;

        unsigned long long magnitude = 0;
        for (; position < text.size(); ++position)
        {
            const char digit = text[position];
            if (digit < '0' || digit > '9')
                return std::nullopt;
            if (magnitude > (std::numeric_limits<unsigned long long>::max() - 9u) / 10u)
                return std::nullopt;
            magnitude = magnitude * 10u + static_cast<unsigned>(digit - '0');
        }
        if (negative)
            return magnitude <= 1u ? std::optional<long long>(-static_cast<long long>(magnitude)) : std::nullopt;
        if (magnitude > static_cast<unsigned long long>(std::numeric_limits<long long>::max()))
            return std::nullopt;
        return static_cast<long long>(magnitude);
    }

    const Spark::SerializedComponent* TransformOf(const Spark::SerializedEntity& entity)
    {
        for (const Spark::SerializedComponent& component : entity.components)
        {
            if (component.typeName == "Transform")
                return &component;
        }
        return nullptr;
    }

    void CheckAcceptedShape(const Spark::SaveData& data)
    {
        using Limits = Spark::SaveRepresentationLimits;
        if (data.entities.size() > Limits::maxEntities)
            InvariantFailure("entity cap");
        if (data.customState.size() > Limits::maxCustomStateEntries)
            InvariantFailure("custom-state cap");
        if (data.metadata.version != Spark::kCurrentSaveVersion)
            InvariantFailure("an accepted save was not migrated to kCurrentSaveVersion");

        const Spark::SaveMetadata& metadata = data.metadata;
        for (const float value : {metadata.playTime, metadata.playerHealth, metadata.playerArmor,
                                  metadata.playerPosition.x, metadata.playerPosition.y, metadata.playerPosition.z})
        {
            if (!std::isfinite(value))
                InvariantFailure("non-finite metadata number");
        }

        for (const auto& [key, value] : data.customState)
        {
            if (!FitsWireString(key) || !FitsWireString(value))
                InvariantFailure("custom-state string over the uint16 wire limit");
        }

        for (std::size_t index = 0; index < data.entities.size(); ++index)
        {
            const Spark::SerializedEntity& entity = data.entities[index];
            if (!FitsWireString(entity.name))
                InvariantFailure("entity name over the uint16 wire limit");

            std::unordered_set<std::string> types;
            for (const Spark::SerializedComponent& component : entity.components)
            {
                if (!FitsWireString(component.typeName))
                    InvariantFailure("component type over the uint16 wire limit");
                if (component.typeName == "NameComponent")
                    InvariantFailure("explicit NameComponent record");
                if (!types.insert(component.typeName).second)
                    InvariantFailure("duplicate component type in one entity");
                for (const auto& [key, value] : component.properties)
                {
                    if (!FitsWireString(key) || !FitsWireString(value))
                        InvariantFailure("property string over the uint16 wire limit");
                }
            }

            const Spark::SerializedComponent* transform = TransformOf(entity);
            if (!transform)
                continue;
            const auto parentProperty = transform->properties.find(Spark::kTransformParentProperty);
            if (parentProperty == transform->properties.end())
                continue;
            const std::optional<long long> parent = ParseDecimal(parentProperty->second);
            if (!parent)
                InvariantFailure("malformed Transform parent index");
            if (*parent == -1)
                continue;
            const auto parentIndex = static_cast<std::size_t>(*parent);
            if (parentIndex >= data.entities.size() || parentIndex == index || !TransformOf(data.entities[parentIndex]))
            {
                InvariantFailure("Transform parent is not a distinct saved entity with a Transform");
            }
        }
    }

    void CheckMetadataPathAgrees(const std::vector<std::uint8_t>& bytes, const Spark::SaveData& accepted)
    {
        Spark::SaveMetadata metadata;
        if (!Spark::DecodeSaveMetadataBytes(bytes, kSourceName, metadata))
            InvariantFailure("DecodeSaveMetadataBytes rejects a file DecodeSaveFileBytes accepts");
        if (!SameMetadata(metadata, accepted.metadata, SameFloat))
            InvariantFailure("DecodeSaveMetadataBytes reports different metadata");
    }

    void CheckRoundTrip(const Spark::SaveData& accepted)
    {
        std::string encoded;
        if (!Spark::EncodeSaveFileBytes(accepted, encoded))
            InvariantFailure("an accepted save cannot be re-encoded, so DeserializeWorld would reject it");

        const std::vector<std::uint8_t> image(encoded.begin(), encoded.end());
        Spark::SaveData reloaded;
        if (!Spark::DecodeSaveFileBytes(image, kSourceName, reloaded))
            InvariantFailure("EncodeSaveFileBytes produced an image DecodeSaveFileBytes rejects");
        if (!SamePayload(accepted, reloaded) ||
            !SameMetadata(accepted.metadata, reloaded.metadata, SameAtWriterPrecision))
            InvariantFailure("Decode(Encode(save)) differs from save");

        // Property and custom-state maps are unordered, so the image bytes may legitimately
        // differ between encodes; the decoded snapshot may not.
        std::string reencoded;
        Spark::SaveData settled;
        if (!Spark::EncodeSaveFileBytes(reloaded, reencoded) ||
            !Spark::DecodeSaveFileBytes(std::vector<std::uint8_t>(reencoded.begin(), reencoded.end()), kSourceName,
                                        settled) ||
            !SamePayload(reloaded, settled) || !SameMetadata(reloaded.metadata, settled.metadata, SameFloat))
        {
            InvariantFailure("the decoded snapshot is not stable after one more round trip");
        }
    }
} // namespace

// This C ABI is the only boundary between the libstdc++ libFuzzer executable
// and the libc++-compiled production codec and logger sources.
extern "C" int SparkFuzzDecodeSaveFile(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes)
        return 0;
    if (data == nullptr && size != 0)
        return 0;

    const std::vector<std::uint8_t> bytes =
        size == 0 ? std::vector<std::uint8_t>() : std::vector<std::uint8_t>(data, data + size);
    const Spark::SaveData sentinel = MakeSentinel();
    Spark::SaveData decoded = sentinel;
    if (!Spark::DecodeSaveFileBytes(bytes, kSourceName, decoded))
    {
        if (!SamePayload(decoded, sentinel) || !SameMetadata(decoded.metadata, sentinel.metadata, SameFloat))
            InvariantFailure("a rejected file modified the caller's SaveData");
        return 0;
    }

    CheckAcceptedShape(decoded);
    CheckMetadataPathAgrees(bytes, decoded);
    CheckRoundTrip(decoded);
    return 0;
}
