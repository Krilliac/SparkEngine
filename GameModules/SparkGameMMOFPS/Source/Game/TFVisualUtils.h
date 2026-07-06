/**
 * @file TFVisualUtils.h
 * @brief Shared faction structure/pawn/vehicle tint-material lookup (sp4
 *        de-hardcode). Consolidates the 4x-duplicated faction->material
 *        switch previously copy-pasted in TFDeployableSystem.cpp,
 *        TFVehicleSystem.cpp, TFVehicleNet.cpp, TFPlayerSystemClient.cpp.
 *
 * SINGLE POINT OF FAILURE: all faction-tinted visuals (pawns, vehicles,
 * deployables) across 4 independently-owned files now resolve through this
 * one function. The fallback below MUST match today's `default:` case
 * (Structure_Concrete.json) exactly, or every faction's structure silently
 * miscolors the instant data isn't loaded (e.g. headless test harness,
 * or any call before TFDataTables::Initialize() has run).
 */
#pragma once

#include "Core/TFTypes.h"
#include "Data/TFDataTables.h"

namespace Terrafront {

/// Faction tint material path (W1 placeholder scheme). Prefers the data
/// table's per-faction `structureMaterial` field; falls back to the
/// hardcoded Concrete default (byte-identical to the pre-dehardcode
/// `default:` switch arm) when data isn't loaded or the faction is unknown.
inline const std::string& FactionStructureMaterial(TFGameContext& ctx, FactionId f)
{
    static const std::string kFallbackConcrete = "Assets/Materials/MMOFPS/Structure_Concrete.json";
    if (ctx.data && ctx.data->IsLoaded()) {
        if (const FactionDef* fd = ctx.data->GetFaction(f))
            return fd->structureMaterial;
    }
    return kFallbackConcrete;
}

} // namespace Terrafront
