/**
 * @file TFSquadSystemInternal.h
 * @brief Shared internals for the TFSquadSystem*.cpp split parts: the roster
 *        vector add/remove helpers shared by the server registry
 *        (RemoveFromSquad) and the client mirror (ClientHandleEcho). Include
 *        only from the TFSquadSystem translation units.
 */
#pragma once

#include "Core/TFTypes.h"

#include <algorithm>
#include <vector>

namespace Terrafront
{
    namespace SquadDetail
    {

        inline void AddUnique(std::vector<PlayerId>& v, PlayerId p)
        {
            if (std::find(v.begin(), v.end(), p) == v.end())
                v.push_back(p);
        }

        inline void Remove(std::vector<PlayerId>& v, PlayerId p)
        {
            v.erase(std::remove(v.begin(), v.end(), p), v.end());
        }

    } // namespace SquadDetail
} // namespace Terrafront
