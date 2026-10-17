#include "TestFramework.h"

#include "Core/EngineContext.h"
#include "Graphics/WeatherSystem.h"
#include <Spark/IWeatherService.h>

#include <limits>

TEST(SdkWeatherService_AllPresetsReachHost)
{
    EngineContext context;
    Spark::WeatherSystem weather;
    context.SetWeather(&weather);
    auto* service = context.GetWeatherService();
    ASSERT_TRUE(service != nullptr);
    const Spark::WeatherPreset presets[] = {Spark::WeatherPreset::Clear, Spark::WeatherPreset::Rain,
                                            Spark::WeatherPreset::Snow, Spark::WeatherPreset::Fog,
                                            Spark::WeatherPreset::Storm};
    const Spark::WeatherType types[] = {Spark::WeatherType::Clear, Spark::WeatherType::Rain, Spark::WeatherType::Snow,
                                        Spark::WeatherType::Fog, Spark::WeatherType::Storm};
    for (int i = 0; i < 5; ++i)
    {
        ASSERT_TRUE(service->SetWeather(presets[i], 0.7f, 2.0f));
        EXPECT_EQ(static_cast<int>(weather.GetTargetState().type), static_cast<int>(types[i]));
        EXPECT_NEAR(weather.GetTargetState().intensity, 0.7f, 1.0e-6f);
        weather.Update(1.0f);
        EXPECT_TRUE(weather.IsTransitioning());
        weather.Update(1.0f);
        EXPECT_FALSE(weather.IsTransitioning());
        EXPECT_EQ(static_cast<int>(weather.GetCurrentState().type), static_cast<int>(types[i]));
    }
}

TEST(SdkWeatherService_RetainedServiceTracksRegistryLifetime)
{
    EngineContext context;
    EXPECT_TRUE(context.GetWeatherService() == nullptr);
    Spark::WeatherSystem first;
    Spark::WeatherSystem second;
    context.SetWeather(&first);
    auto* service = context.GetWeatherService();
    ASSERT_TRUE(service != nullptr);
    context.SetWeather(nullptr);
    EXPECT_TRUE(context.GetWeatherService() == nullptr);
    EXPECT_FALSE(service->SetWeather(Spark::WeatherPreset::Storm, 1.0f, 1.0f));
    EXPECT_FALSE(first.IsTransitioning());
    context.RegisterSystem<Spark::WeatherSystem>(&second);
    EXPECT_TRUE(context.GetWeatherService() == service);
    ASSERT_TRUE(service->SetWeather(Spark::WeatherPreset::Rain, 0.3f, 1.0f));
    EXPECT_FALSE(first.IsTransitioning());
    EXPECT_EQ(static_cast<int>(second.GetTargetState().type), static_cast<int>(Spark::WeatherType::Rain));
}

TEST(SdkWeatherService_InvalidCommandsDoNotReplacePendingTransition)
{
    EngineContext context;
    Spark::WeatherSystem weather;
    context.SetWeather(&weather);
    auto* service = context.GetWeatherService();
    ASSERT_TRUE(service != nullptr);
    ASSERT_TRUE(service->SetWeather(Spark::WeatherPreset::Snow, 0.4f, 2.0f));
    weather.Update(1.0f);
    EXPECT_FALSE(service->SetWeather(static_cast<Spark::WeatherPreset>(99), 1.0f, 1.0f));
    EXPECT_FALSE(service->SetWeather(Spark::WeatherPreset::Storm, std::numeric_limits<float>::quiet_NaN(), 1.0f));
    EXPECT_FALSE(service->SetWeather(Spark::WeatherPreset::Storm, 1.0f, std::numeric_limits<float>::infinity()));
    EXPECT_FALSE(service->SetWeather(Spark::WeatherPreset::Storm, -std::numeric_limits<float>::infinity(), 1.0f));
    EXPECT_EQ(static_cast<int>(weather.GetTargetState().type), static_cast<int>(Spark::WeatherType::Snow));
    EXPECT_NEAR(weather.GetTargetState().intensity, 0.4f, 1.0e-6f);
    weather.Update(1.0f);
    EXPECT_FALSE(weather.IsTransitioning());
    EXPECT_EQ(static_cast<int>(weather.GetCurrentState().type), static_cast<int>(Spark::WeatherType::Snow));
}

TEST(SdkWeatherService_PreservesHostFiniteScalarSemantics)
{
    EngineContext context;
    Spark::WeatherSystem weather;
    context.SetWeather(&weather);
    auto* service = context.GetWeatherService();
    ASSERT_TRUE(service != nullptr);
    ASSERT_TRUE(service->SetWeather(Spark::WeatherPreset::Rain, -1.0f, -2.0f));
    EXPECT_NEAR(weather.GetTargetState().intensity, Spark::GetWeatherPreset(Spark::WeatherType::Rain).intensity,
                1.0e-6f);
    weather.Update(0.005f);
    EXPECT_TRUE(weather.IsTransitioning());
    weather.Update(0.005f);
    EXPECT_FALSE(weather.IsTransitioning());
    ASSERT_TRUE(service->SetWeather(Spark::WeatherPreset::Rain, 2.0f, 0.0f));
    EXPECT_NEAR(weather.GetTargetState().intensity, 1.0f, 1.0e-6f);
    ASSERT_TRUE(service->SetWeather(Spark::WeatherPreset::Rain, 0.0f, 0.0f));
    EXPECT_NEAR(weather.GetTargetState().intensity, 0.0f, 1.0e-6f);
}
