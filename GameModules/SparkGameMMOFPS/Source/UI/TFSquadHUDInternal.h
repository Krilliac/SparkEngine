/**
 * @file TFSquadHUDInternal.h
 * @brief Shared internals for the TFSquadHUD*.cpp split parts: the squad
 *        accent color used by both the squadmate list (TFSquadHUD.cpp) and
 *        the waypoint world beacon (TFSquadHUDBeacon.cpp). Include only from
 *        the TFSquadHUD translation units.
 */
#pragma once

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

namespace Terrafront
{
    namespace SquadHudDetail
    {

#ifdef SPARK_HAS_IMGUI

        /// Squad accent — matches the TFNameplates squadmate tint so "squad
        /// green" reads as one color across plates, list, and beacon.
        constexpr ImU32 SquadCol(int a8)
        {
            return IM_COL32(110, 235, 140, a8);
        }

#endif // SPARK_HAS_IMGUI

    } // namespace SquadHudDetail
} // namespace Terrafront
