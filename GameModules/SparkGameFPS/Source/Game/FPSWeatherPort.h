/**
 * @file FPSWeatherPort.h
 * @brief Narrow, module-owned weather capability used by SparkGameFPS.
 *
 * The port deliberately contains only stable FPS vocabulary.  Engine-owned
 * weather types stay behind the Core/Main adapter boundary.
 */
#pragma once

namespace SparkGameFPS
{
    enum class WeatherPreset
    {
        Clear,
        Rain,
        Snow,
        Fog,
        Storm,
    };

    class IFPSWeatherPort
    {
      public:
        virtual ~IFPSWeatherPort() = default;

        /// Apply a preset; returns false when the optional engine capability is unavailable.
        /// [game thread] Callers must invoke this only from the game/update thread.
        virtual bool SetWeather(WeatherPreset preset, float intensity, float transitionSeconds) = 0;
    };
} // namespace SparkGameFPS
