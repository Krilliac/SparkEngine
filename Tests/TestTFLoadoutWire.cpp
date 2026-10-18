/**
 * @file TestTFLoadoutWire.cpp
 * @brief TF-110: the server's TF_LoadoutChange decode (Net/TFLoadoutWire.h)
 *        rejects a WeaponId that resolves to no weapon instead of treating it
 *        as the class default.
 *
 * Header-only (TestTFAbilityWire.cpp pattern): the resolver stands in for
 * TFDataTables::GetWeapon, which TFServerSim::RouteClientMessage wraps the
 * same way. Before TF-110 the route mapped an unresolved id to an empty key,
 * and TFProgressionSystem::ValidLoadoutSlotKey accepts an empty key as the
 * class default, so a forged id was silently saved instead of rejected.
 */
#include "TestFramework.h"

#include "Net/TFLoadoutWire.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>

using namespace Terrafront;

namespace
{
    // Weapon table stand-in: ids 3, 7 and 11 exist; everything else is unknown.
    const std::string* ResolveTestWeapon(uint16_t id)
    {
        static const std::map<uint16_t, std::string> kWeapons = {
            {3, "tr_rifle"},
            {7, "tr_pistol"},
            {11, "repair_tool"},
        };
        const auto it = kWeapons.find(id);
        return it != kWeapons.end() ? &it->second : nullptr;
    }

    TF_LoadoutChange MakeChange(uint16_t primary, uint16_t secondary, uint16_t tool)
    {
        TF_LoadoutChange lc{};
        lc.primary = primary;
        lc.secondary = secondary;
        lc.tool = tool;
        return lc;
    }
} // namespace

TEST(TF110_LoadoutWire_UnknownWeaponIdIsRejectedNotDefaulted)
{
    // A forged out-of-range primary with otherwise valid slots.
    const auto primaryForged = DecodeLoadoutChange(MakeChange(0x7FFE, 7, 11), ResolveTestWeapon);
    EXPECT_FALSE(primaryForged.has_value());

    // Each slot is checked, not only the primary.
    EXPECT_FALSE(DecodeLoadoutChange(MakeChange(3, 0x7FFE, 11), ResolveTestWeapon).has_value());
    EXPECT_FALSE(DecodeLoadoutChange(MakeChange(3, 7, 0x1234), ResolveTestWeapon).has_value());
}

TEST(TF110_LoadoutWire_InvalidWeaponMeansClassDefault)
{
    const auto keys =
        DecodeLoadoutChange(MakeChange(kInvalidWeapon, kInvalidWeapon, kInvalidWeapon), ResolveTestWeapon);
    ASSERT_TRUE(keys.has_value());
    EXPECT_TRUE(keys->primary.empty());
    EXPECT_TRUE(keys->secondary.empty());
    EXPECT_TRUE(keys->tool.empty());
}

TEST(TF110_LoadoutWire_ResolvesAllThreeSlots)
{
    const auto keys = DecodeLoadoutChange(MakeChange(3, 7, 11), ResolveTestWeapon);
    ASSERT_TRUE(keys.has_value());
    EXPECT_EQ(keys->primary, std::string("tr_rifle"));
    EXPECT_EQ(keys->secondary, std::string("tr_pistol"));
    EXPECT_EQ(keys->tool, std::string("repair_tool"));

    // Mixed: one explicit default slot alongside resolved ones.
    const auto mixed = DecodeLoadoutChange(MakeChange(3, kInvalidWeapon, 11), ResolveTestWeapon);
    ASSERT_TRUE(mixed.has_value());
    EXPECT_EQ(mixed->primary, std::string("tr_rifle"));
    EXPECT_TRUE(mixed->secondary.empty());
    EXPECT_EQ(mixed->tool, std::string("repair_tool"));
}
