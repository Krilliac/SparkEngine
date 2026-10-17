/**
 * @file FuzzReflectedSceneProduction.cpp
 * @brief libc++-compiled production adapter for the reflected JSON scene libFuzzer harness.
 *
 * The first input byte selects the deserialization mode (low bit clear:
 * Permissive, the mode LoadWorld uses for authored scenes; set: StrictRecovery,
 * the mode editor crash recovery uses). TrustedSnapshot is never selected: it is
 * documented as in-process-only and skips the untrusted-input caps. The rest of
 * the input is the scene document, loaded into a fresh World with the shipped
 * Spark::DeserializeInto. A violation of the reader's contract aborts so
 * libFuzzer records a crash rather than a silent pass:
 *  - an accepted document creates exactly one entity per element of its
 *    'entities' array (checked against an independent re-parse of the text),
 *  - every Transform parent link in the loaded World names a live entity other
 *    than itself, is mirrored in that parent's children list, and no parent
 *    chain cycles (a bounded walk here, independent of World::SetParent),
 *  - a loaded World is persistable: TrySerializeWorld succeeds (the reader
 *    accepts only finite values, so nothing it installs may block a save),
 *  - the writer is canonical: t1 = SerializeWorld(loaded) loads Permissively
 *    into a fresh World whose SerializeWorld output equals t1 byte for byte, as
 *    ReflectedSceneSerializer.h promises.
 *
 * The 64 MiB byte cap and the 4,000,000-value budget lie far above the smoke's
 * -max_len; SerializationHardening_* tests in Tests/TestSerializationHardening.cpp
 * pin both, and this target proves the reader stays inside its invariants for
 * arbitrary documents below them.
 */

#include "FuzzReflectedSceneProduction.h"

#include "Engine/ECS/Components.h"
#include "SceneManager/ReflectedSceneSerializer.h"
#include "Utils/Assert.h"

#include <nlohmann_json.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzReflectedScene: DeserializeInto violated: %s\n", what);
        std::abort();
    }

    std::size_t CountAlive(const entt::registry& registry)
    {
        std::size_t count = 0;
        for ([[maybe_unused]] auto&& [entity] : registry.storage<entt::entity>()->each())
            ++count;
        return count;
    }

    void CheckHierarchy(const World& world)
    {
        const entt::registry& registry = world.GetRegistry();
        const std::size_t alive = CountAlive(registry);
        for (auto&& [entity] : registry.storage<entt::entity>()->each())
        {
            const Transform* transform = registry.try_get<Transform>(entity);
            if (!transform)
                continue;

            for (const entt::entity child : transform->children)
            {
                const Transform* childTransform = registry.valid(child) ? registry.try_get<Transform>(child) : nullptr;
                if (!childTransform || childTransform->parent != entity)
                    InvariantFailure("a children entry does not point back at its parent");
            }

            // Bounded walk: a chain longer than the live entity count must revisit one.
            entt::entity cursor = entity;
            for (std::size_t steps = 0;; ++steps)
            {
                const Transform* link = registry.try_get<Transform>(cursor);
                if (!link || link->parent == entt::null)
                    break;
                if (steps >= alive)
                    InvariantFailure("Transform parent chain cycles");
                if (link->parent == cursor || !registry.valid(link->parent))
                    InvariantFailure("Transform parent names itself or an entity that is not in the World");
                const Transform* parentTransform = registry.try_get<Transform>(link->parent);
                if (!parentTransform || std::find(parentTransform->children.begin(), parentTransform->children.end(),
                                                  cursor) == parentTransform->children.end())
                {
                    InvariantFailure("Transform parent link is not mirrored in the parent's children");
                }
                cursor = link->parent;
            }
        }
    }

    std::size_t DocumentEntityCount(const std::string& text)
    {
        try
        {
            const nlohmann::json root =
                nlohmann::json::parse(text, static_cast<std::size_t>(Spark::kMaxSceneDocumentValues));
            if (!root.is_object() || !root.contains("entities") || !root["entities"].is_array())
                InvariantFailure("accepted a document without an 'entities' array");
            return root["entities"].size();
        }
        catch (const std::exception&)
        {
            InvariantFailure("accepted a document that does not re-parse");
        }
    }
} // namespace

// Fatal sink for World's SPARK_REQUIRE/ASSERT_ALWAYS checks. The production body
// in Utils/Assert.cpp formats diagnostics, runs the crash handler and aborts
// (suppression is off by default); linking it would pull the crash handler and
// console into this target, so the adapter keeps only the part that matters to
// libFuzzer: report the failed precondition and abort.
void Assert::Fail(const char* expr, const char* file, int line, const char* /*fmt*/, ...)
{
    std::fprintf(stderr, "SparkFuzzReflectedScene: precondition '%s' failed at %s:%d\n", expr ? expr : "?",
                 file ? file : "?", line);
    std::abort();
}

// This C ABI is the only boundary between the libstdc++ libFuzzer executable
// and the libc++-compiled production reader, reflection registry and logger.
extern "C" int SparkFuzzDeserializeReflectedScene(const std::uint8_t* data, std::size_t size, std::uint32_t maxDepth)
{
    if (maxDepth != nlohmann::json::max_parse_depth)
        InvariantFailure("the harness depth budget no longer matches json::max_parse_depth");
    if (size > kMaxInputBytes || size == 0 || data == nullptr)
        return 0;

    const Spark::SceneDeserializeMode mode =
        (data[0] & 1u) != 0 ? Spark::SceneDeserializeMode::StrictRecovery : Spark::SceneDeserializeMode::Permissive;
    const std::string text(reinterpret_cast<const char*>(data) + 1, size - 1);

    World loaded;
    if (!Spark::DeserializeInto(loaded, text, mode))
        return 0;

    if (loaded.GetEntityCount() != DocumentEntityCount(text))
        InvariantFailure("entity count differs from the document's 'entities' array");
    CheckHierarchy(loaded);

    std::string persisted;
    if (!Spark::TrySerializeWorld(loaded, persisted))
        InvariantFailure("a loaded World cannot be saved (TrySerializeWorld failed)");

    const std::string first = Spark::SerializeWorld(loaded);
    World reloaded;
    if (!Spark::DeserializeInto(reloaded, first, Spark::SceneDeserializeMode::Permissive))
        InvariantFailure("SerializeWorld output of a loaded World does not load back");
    if (Spark::SerializeWorld(reloaded) != first)
        InvariantFailure("SerializeWorld is not canonical across a load (text changed on the second pass)");
    return 0;
}
