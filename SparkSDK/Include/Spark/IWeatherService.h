/**
 * @file IWeatherService.h
 * @brief Host-owned weather commands for game modules (SDK 10).
 */
#pragma once

#include <cstdint>

namespace Spark
{
    /// Stable command vocabulary; values do not alias the private WeatherType enum.
    enum class WeatherPreset : uint32_t
    {
        Clear = 0,
        Rain = 1,
        Snow = 2,
        Fog = 3,
        Storm = 4,
    };

    /**
     * Borrowed from IEngineContext; never delete the service. Valid until context
     * destruction, including subsystem replacement. Call only on the game thread,
     * serialized with host weather updates and subsystem registration/teardown.
     */
    class IWeatherService
    {
      public:
        virtual ~IWeatherService() = default;

        /**
         * Queue a host weather transition. Returns false without mutation for an
         * unknown preset, nonfinite scalar, or unavailable weather subsystem.
         * Finite negative intensity selects the preset default; other intensities
         * clamp to [0,1]. Transition duration has the host minimum of 0.01 seconds.
         * Success means accepted, not that the transition has already completed.
         */
        virtual bool SetWeather(WeatherPreset preset, float intensity, float transitionSeconds) = 0;
    };
} // namespace Spark
