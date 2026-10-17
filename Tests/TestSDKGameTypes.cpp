/**
 * @file TestSDKGameTypes.cpp
 * @brief Compile the shared gameplay types using only public SDK headers.
 */

#include "TestFramework.h"
#include <Spark/GameTypes.h>
#include <type_traits>

// These are persisted and used by both the runtime and FPS module. Moving the
// declarations to the SDK must not change their namespace, representation or IDs.
static_assert(std::is_same_v<std::underlying_type_t<SparkEditor::PlayerClass>, int>);
static_assert(std::is_same_v<std::underlying_type_t<SparkEditor::ClassAbility>, int>);
static_assert(std::is_same_v<std::underlying_type_t<SparkEditor::VehicleType>, int>);
static_assert(static_cast<int>(SparkEditor::PlayerClass::SCOUT) == 0);
static_assert(static_cast<int>(SparkEditor::PlayerClass::RECON) == 3);
static_assert(static_cast<int>(SparkEditor::PlayerClass::TITAN) == 5);
static_assert(static_cast<int>(SparkEditor::PlayerClass::COUNT) == 6);
static_assert(static_cast<int>(SparkEditor::ClassAbility::CLOAK) == 7);
static_assert(static_cast<int>(SparkEditor::VehicleType::HELICOPTER) == 10);
static_assert(static_cast<int>(SparkEditor::WeaponType::COUNT) == 103);

TEST(SDKGameTypes_PublicGameplayIdentifiers)
{
    EXPECT_EQ(static_cast<int>(SparkEditor::MovementState::DEAD), 15);
    EXPECT_EQ(static_cast<int>(SparkEditor::HealthState::DEAD), 8);
    EXPECT_EQ(static_cast<int>(SparkEditor::InteractiveObjectType::VEHICLE_SPAWN), 40);
    EXPECT_EQ(static_cast<int>(SparkEditor::DamageType::TRUE_DAMAGE), 15);
}
