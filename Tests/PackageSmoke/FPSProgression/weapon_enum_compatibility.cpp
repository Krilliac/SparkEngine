/** @file weapon_enum_compatibility.cpp
 * @brief The legacy engine header and public SDK must name the same weapon enum.
 */
#include <Spark/WeaponTypes.h>
#include "Enums/GameSystemEnums.h"

#include <type_traits>

static_assert(std::is_same_v<std::underlying_type_t<SparkEditor::WeaponType>, int>);
static_assert(static_cast<int>(SparkEditor::WeaponType::COUNT) == 103);
