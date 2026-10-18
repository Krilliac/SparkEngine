/** @file EngineSdkWeather.h
 * @brief Context-owned implementation of the public weather command service.
 */
#pragma once

#include <Spark/IWeatherService.h>

class EngineContext;

class EngineSdkWeather final : public Spark::IWeatherService
{
  public:
    explicit EngineSdkWeather(EngineContext& context) : m_context(context) {}
    bool SetWeather(Spark::WeatherPreset preset, float intensity, float transitionSeconds) override;

  private:
    EngineContext& m_context;
};
