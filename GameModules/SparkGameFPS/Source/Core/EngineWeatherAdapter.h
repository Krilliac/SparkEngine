#pragma once

#include "Game/FPSWeatherPort.h"

namespace Spark
{
    class WeatherSystem;
}

namespace SparkGameFPS
{
    /// Non-owning Core/Main adapter from the engine weather implementation to
    /// the module-owned weather vocabulary.
    class EngineWeatherAdapter final : public IFPSWeatherPort
    {
      public:
        explicit EngineWeatherAdapter(Spark::WeatherSystem* weather) : m_weather(weather) {}

        /// [game thread] Forwards the module vocabulary to the engine weather system.
        bool SetWeather(WeatherPreset preset, float intensity, float transitionSeconds) override;

      private:
        Spark::WeatherSystem* m_weather{nullptr};
    };
} // namespace SparkGameFPS
