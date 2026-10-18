/**
 * @file FuzzRuntimePrefabProduction.cpp
 * @brief libc++-compiled production adapter for the runtime prefab libFuzzer harness.
 *
 * Spark::ECS::RuntimePrefab::Deserialize (Engine/ECS/RuntimePrefab.h) reads a PRFB prefab
 * (magic, version, name, then components of a type name and a property map) from a
 * Spark::BinaryReader over untrusted bytes. The adapter decodes the fuzz bytes into a
 * sentinel prefab that already holds a component and a parent link. A violated contract
 * aborts so libFuzzer records a crash:
 *  - a rejected stream leaves the prefab exactly as it was (name, components, parent),
 *  - an accepted stream leaves the reader without an error, consumed at least the 12-byte
 *    header-plus-count prefix and no more than the input, and each component cost at least
 *    the 8 bytes of its type-name length and property count,
 *  - the parent link is never touched,
 *  - re-encoding an accepted prefab (Serialize) is never larger than what was consumed, and it
 *    decodes, consuming exactly its own length, into an identical prefab,
 *  - decoding the same bytes twice gives the same prefab and the same consumed length.
 */

#include "FuzzRuntimePrefabProduction.h"

#include "Engine/ECS/RuntimePrefab.h"
#include "Utils/Serializer.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;
    // Smallest accepted stream: magic, version, empty name length, empty name, component count.
    constexpr std::size_t kMinimumSerializedPrefabBytes = 4u + 4u + 4u + 4u;
    constexpr std::size_t kMinComponentBytes = 4u + 4u; // type-name length, property count

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzRuntimePrefab: RuntimePrefab::Deserialize violated: %s\n", what);
        std::abort();
    }

    using Spark::ECS::PrefabComponentData;
    using Spark::ECS::RuntimePrefab;

    bool SameComponents(const std::vector<PrefabComponentData>& a, const std::vector<PrefabComponentData>& b)
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            if (a[i].typeName != b[i].typeName || a[i].properties != b[i].properties)
            {
                return false;
            }
        }
        return true;
    }

    bool SamePrefab(const RuntimePrefab& a, const RuntimePrefab& b)
    {
        return a.GetName() == b.GetName() && SameComponents(a.GetComponents(), b.GetComponents());
    }

    struct Decoded
    {
        bool accepted = false;
        bool readerError = false;
        std::size_t consumed = 0;
    };

    Decoded Decode(RuntimePrefab& prefab, const std::uint8_t* data, std::size_t size)
    {
        Spark::BinaryReader reader(data, size);
        Decoded result;
        result.accepted = prefab.Deserialize(reader);
        result.readerError = reader.HasError();
        result.consumed = reader.Tell();
        return result;
    }

    std::unique_ptr<RuntimePrefab> MakeSentinel(const RuntimePrefab* parent)
    {
        auto sentinel = std::make_unique<RuntimePrefab>("sentinel-prefab");
        sentinel->AddComponent("SentinelComponent", {{"sentinel-key", "sentinel-value"}});
        sentinel->SetParent(parent);
        return sentinel;
    }
} // namespace

extern "C" int SparkFuzzDeserializeRuntimePrefab(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0))
    {
        return 0;
    }
    // BinaryReader reads through the pointer; give the empty input a valid one.
    static const std::uint8_t kNoBytes = 0;
    const std::uint8_t* bytes = size == 0 ? &kNoBytes : data;

    const RuntimePrefab parent("parent-prefab");
    const auto untouched = MakeSentinel(&parent);
    auto prefab = MakeSentinel(&parent);
    const Decoded first = Decode(*prefab, bytes, size);

    if (prefab->GetParent() != &parent)
    {
        InvariantFailure("the parent link changed");
    }
    if (!first.accepted)
    {
        if (!SamePrefab(*prefab, *untouched))
        {
            InvariantFailure("a rejected stream modified the prefab");
        }
        return 0;
    }

    if (first.readerError)
    {
        InvariantFailure("an accepted stream left the reader in its error state");
    }
    if (first.consumed < kMinimumSerializedPrefabBytes || first.consumed > size)
    {
        InvariantFailure("an accepted stream consumed an impossible number of bytes");
    }
    if (prefab->GetComponents().size() > (first.consumed - kMinimumSerializedPrefabBytes) / kMinComponentBytes)
    {
        InvariantFailure("more components were decoded than the consumed bytes can describe");
    }

    auto again = MakeSentinel(&parent);
    const Decoded second = Decode(*again, bytes, size);
    if (!second.accepted || second.consumed != first.consumed || !SamePrefab(*prefab, *again))
    {
        InvariantFailure("decoding the same bytes twice gave different prefabs");
    }

    Spark::BinaryWriter writer;
    prefab->Serialize(writer);
    const std::vector<std::uint8_t>& encoded = writer.GetBuffer();
    if (encoded.size() > first.consumed)
    {
        InvariantFailure("the re-encoded prefab is larger than the bytes it was decoded from");
    }

    auto reloaded = MakeSentinel(&parent);
    const Decoded third = Decode(*reloaded, encoded.data(), encoded.size());
    if (!third.accepted || third.readerError)
    {
        InvariantFailure("a re-encoded prefab no longer decodes");
    }
    if (third.consumed != encoded.size())
    {
        InvariantFailure("a re-encoded prefab did not decode to its exact length");
    }
    if (!SamePrefab(*prefab, *reloaded))
    {
        InvariantFailure("Serialize -> Deserialize changed the prefab");
    }
    return 0;
}
