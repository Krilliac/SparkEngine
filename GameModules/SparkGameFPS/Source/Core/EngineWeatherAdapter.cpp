#include "Core/EngineWeatherAdapter.h"

#include <Spark/IWeatherService.h>

namespace SparkGameFPS
{
    bool EngineWeatherAdapter::SetWeather(WeatherPreset preset, float intensity, float transitionSeconds)
    {
        if (!m_weather)
            return false;

        Spark::WeatherPreset enginePreset = Spark::WeatherPreset::Clear;
        switch (preset)
        {
        case WeatherPreset::Clear:
            enginePreset = Spark::WeatherPreset::Clear;
            break;
        case WeatherPreset::Rain:
            enginePreset = Spark::WeatherPreset::Rain;
            break;
        case WeatherPreset::Snow:
            enginePreset = Spark::WeatherPreset::Snow;
            break;
        case WeatherPreset::Fog:
            enginePreset = Spark::WeatherPreset::Fog;
            break;
        case WeatherPreset::Storm:
            enginePreset = Spark::WeatherPreset::Storm;
            break;
        default:
            return false;
        }

        return m_weather->SetWeather(enginePreset, intensity, transitionSeconds);
    }
} // namespace SparkGameFPS
