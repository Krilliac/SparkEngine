/**
 * @file Test_engine-misc_SnapshotCount.cpp
 * @brief Regression test for the snapshot component-count contract.
 *
 * CoreComponentSerializers previously wrote a placeholder instance-count of 0
 * before its records (the append-only BinaryWriter cannot back-patch), so on
 * restore DeserializeXxx read count==0, restored nothing, and left the record
 * bytes in the stream — desyncing the outer type loop and corrupting the scene.
 *
 * This test registers a probe serializer that follows the fixed contract
 * ("write the real record count, then that many records") and drives it through
 * SceneSnapshotSerializer::Serialize/Deserialize, proving that a non-zero count
 * round-trips every record. A written count of 0-with-records (the original bug)
 * would fail the size assertion below.
 */

#include "../TestFramework.h"
#include "Engine/Editor/SceneSnapshotSerializer.h"

#include <entt/entt.hpp>

#include <cstdint>
#include <vector>

namespace
{
    std::vector<uint32_t> g_written; ///< Records the probe serializes out.
    std::vector<uint32_t> g_read;    ///< Records restored on deserialize.

    constexpr uint32_t kProbeTypeId = 0x5A5A0011u;
    bool g_registered = false;

    void EnsureProbeRegistered()
    {
        if (g_registered)
            return;
        g_registered = true;

        Spark::Editor::ComponentSerializerEntry entry;
        entry.typeName = "HardenSnapshotCountProbe";
        entry.typeId = kProbeTypeId;
        entry.serialize = [](Spark::Editor::SnapshotWriter& w, const void*) -> uint32_t
        {
            w.WriteU32(static_cast<uint32_t>(g_written.size()));
            for (uint32_t v : g_written)
                w.WriteU32(v);
            return static_cast<uint32_t>(g_written.size());
        };
        entry.deserialize = [](Spark::Editor::SnapshotReader& r, void*, uint32_t) -> bool
        {
            uint32_t count = r.ReadU32();
            g_read.clear();
            for (uint32_t i = 0; i < count; ++i)
                g_read.push_back(r.ReadU32());
            return r.IsValid();
        };
        Spark::Editor::ComponentSerializerRegistry::Instance().Register(entry);
    }
} // namespace

TEST(SnapshotCount_WritesRealCountAndRoundTrips)
{
    EnsureProbeRegistered();
    g_written = {11u, 22u, 33u, 44u};
    g_read.clear();

    // Pass a real (empty) registry: SceneSnapshotSerializer runs EVERY globally
    // registered serializer, and the real CoreComponentSerializers dereference the
    // registry (registry.valid(...)) — a nullptr would crash them. An empty registry
    // makes them emit nothing; only our probe writes records.
    entt::registry reg;
    auto data = Spark::Editor::SceneSnapshotSerializer::Serialize(&reg, 0);
    ASSERT_TRUE(data.size() >= 16);

    entt::registry restoreReg;
    bool ok = Spark::Editor::SceneSnapshotSerializer::Deserialize(data, &restoreReg);
    EXPECT_TRUE(ok);

    // With the correct count written, every record is restored in order. Under
    // the original bug (count written as 0), g_read would be empty.
    ASSERT_EQ(g_read.size(), g_written.size());
    for (size_t i = 0; i < g_written.size(); ++i)
        EXPECT_EQ(g_read[i], g_written[i]);
}

TEST(SnapshotCount_EmptySetWritesZeroCount)
{
    EnsureProbeRegistered();
    g_written.clear();
    g_read = {999u}; // sentinel that must be overwritten (cleared) by deserialize

    entt::registry reg;
    auto data = Spark::Editor::SceneSnapshotSerializer::Serialize(&reg, 0);
    entt::registry restoreReg;
    bool ok = Spark::Editor::SceneSnapshotSerializer::Deserialize(data, &restoreReg);
    EXPECT_TRUE(ok);
    EXPECT_EQ(g_read.size(), static_cast<size_t>(0));
}
