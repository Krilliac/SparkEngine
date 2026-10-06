/**
 * @file FPSQuickLoad.cpp
 * @brief Transactional load of an FPS save slot together with its local profile block.
 */

#include "FPSQuickLoad.h"

#include "Engine/SaveSystem/SaveSystem.h"

#include <unordered_map>

namespace Spark
{
    FPSQuickLoadStatus LoadSlotWithProfile(SaveSystem& saveSystem, const std::string& slotName, ::World& world,
                                           FPSLocalProfile& outProfile, std::string& outProfileError)
    {
        FPSLocalProfile staged;
        std::string profileError;
        bool profileRejected = false;
        const auto validateProfile = [&](const std::unordered_map<std::string, std::string>& customState)
        {
            profileRejected = !staged.ReadFrom(customState, profileError);
            return !profileRejected;
        };

        std::unordered_map<std::string, std::string> customState;
        if (!saveSystem.Load(slotName, world, customState, validateProfile))
        {
            if (profileRejected)
            {
                outProfileError = profileError;
                return FPSQuickLoadStatus::ProfileRejected;
            }
            return FPSQuickLoadStatus::LoadFailed;
        }

        outProfile = staged;
        return FPSQuickLoadStatus::Loaded;
    }
} // namespace Spark
