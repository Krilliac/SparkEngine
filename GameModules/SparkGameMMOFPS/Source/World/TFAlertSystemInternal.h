/**
 * @file TFAlertSystemInternal.h
 * @brief Shared internals for the TFAlertSystem*.cpp split parts: the faction
 *        wire-index inverse and the playable-faction predicate used by both
 *        the server/scoring part and the HUD part. Include only from the
 *        TFAlertSystem translation units.
 */
#pragma once

#include "Core/TFTypes.h"

#include <cstddef>

namespace Terrafront
{
    namespace AlertDetail
    {

        /// Faction wire index (0..2) -> FactionId. Inverse of FactionIdx.
        inline FactionId FactionOfIdx(size_t idx)
        {
            return static_cast<FactionId>(idx + 1);
        }

        inline bool PlayableFaction(FactionId f)
        {
            return f == FactionId::MRA || f == FactionId::AUC || f == FactionId::HLX;
        }

    } // namespace AlertDetail
} // namespace Terrafront
