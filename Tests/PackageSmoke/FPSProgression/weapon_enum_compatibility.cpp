/** @file weapon_enum_compatibility.cpp
 * @brief The public SDK weapon and gameplay headers must name the same weapon enum.
 */
#include <Spark/WeaponTypes.h>
#include <Spark/GameTypes.h>

#include <type_traits>

static_assert(std::is_same_v<std::underlying_type_t<SparkEditor::WeaponType>, int>);
static_assert(static_cast<int>(SparkEditor::WeaponType::COUNT) == 103);
