#include "Core/EngineWeatherAdapter.h"

#include "Graphics/WeatherSystem.h"

namespace SparkGameFPS
{
    bool EngineWeatherAdapter::SetWeather(WeatherPreset preset, float intensity, float transitionSeconds)
    {
        if (!m_weather)
            return false;

        Spark::WeatherType enginePreset = Spark::WeatherType::Clear;
        switch (preset)
        {
        case WeatherPreset::Clear:
            enginePreset = Spark::WeatherType::Clear;
            break;
        case WeatherPreset::Rain:
            enginePreset = Spark::WeatherType::Rain;
            break;
        case WeatherPreset::Snow:
            enginePreset = Spark::WeatherType::Snow;
            break;
        case WeatherPreset::Fog:
            enginePreset = Spark::WeatherType::Fog;
            break;
        case WeatherPreset::Storm:
            enginePreset = Spark::WeatherType::Storm;
            break;
        }

        m_weather->SetWeather(enginePreset, intensity, transitionSeconds);
        return true;
    }
} // namespace SparkGameFPS
