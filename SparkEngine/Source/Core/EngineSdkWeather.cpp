#include "EngineSdkWeather.h"

#include "EngineContext.h"
#include "Graphics/WeatherSystem.h"

#include <cmath>

bool EngineSdkWeather::SetWeather(Spark::WeatherPreset preset, float intensity, float transitionSeconds)
{
    if (!std::isfinite(intensity) || !std::isfinite(transitionSeconds))
    {
        return false;
    }

    Spark::WeatherType type;
    switch (preset)
    {
    case Spark::WeatherPreset::Clear:
        type = Spark::WeatherType::Clear;
        break;
    case Spark::WeatherPreset::Rain:
        type = Spark::WeatherType::Rain;
        break;
    case Spark::WeatherPreset::Snow:
        type = Spark::WeatherType::Snow;
        break;
    case Spark::WeatherPreset::Fog:
        type = Spark::WeatherType::Fog;
        break;
    case Spark::WeatherPreset::Storm:
        type = Spark::WeatherType::Storm;
        break;
    default:
        return false;
    }

    // The registry is the single source of truth. A retained service cannot keep
    // a subsystem alive or accidentally call a previously unregistered instance.
    auto* weather = m_context.GetWeather();
    if (!weather)
    {
        return false;
    }
    weather->SetWeather(type, intensity, transitionSeconds);
    return true;
}
