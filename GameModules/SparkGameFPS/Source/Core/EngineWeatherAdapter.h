#pragma once

#include "Game/FPSWeatherPort.h"

namespace Spark
{
    class IWeatherService;
}

namespace SparkGameFPS
{
    /// Non-owning Core/Main adapter from the public SDK weather service to
    /// the module-owned weather vocabulary.
    class EngineWeatherAdapter final : public IFPSWeatherPort
    {
      public:
        explicit EngineWeatherAdapter(Spark::IWeatherService* weather) : m_weather(weather) {}

        /// [game thread] Forwards the module vocabulary to the public SDK weather service.
        bool SetWeather(WeatherPreset preset, float intensity, float transitionSeconds) override;

      private:
        Spark::IWeatherService* m_weather{nullptr};
    };
} // namespace SparkGameFPS
