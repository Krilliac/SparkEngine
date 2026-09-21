#include "TestFramework.h"

#include "Game/FPSWeatherPort.h"
#include "Game/FPSWeatherIntegration.h"
#include "Core/EngineWeatherAdapter.h"
#include "Graphics/WeatherSystem.h"

namespace
{
    class FakeWeatherPort final : public SparkGameFPS::IFPSWeatherPort
    {
      public:
        bool SetWeather(SparkGameFPS::WeatherPreset preset, float intensity, float transitionSeconds) override
        {
            ++calls;
            lastPreset = preset;
            lastIntensity = intensity;
            lastTransitionSeconds = transitionSeconds;
            return enabled;
        }

        bool enabled{true};
        int calls{0};
        SparkGameFPS::WeatherPreset lastPreset{SparkGameFPS::WeatherPreset::Clear};
        float lastIntensity{-1.0f};
        float lastTransitionSeconds{-1.0f};
    };
} // namespace

TEST(FPSWeatherPort_FakePreservesNarrowContract)
{
    FakeWeatherPort port;

    EXPECT_TRUE(port.SetWeather(SparkGameFPS::WeatherPreset::Storm, 0.8f, 3.0f));
    EXPECT_EQ(port.calls, 1);
    EXPECT_EQ(static_cast<int>(port.lastPreset), static_cast<int>(SparkGameFPS::WeatherPreset::Storm));
    EXPECT_NEAR(port.lastIntensity, 0.8f, 1.0e-6f);
    EXPECT_NEAR(port.lastTransitionSeconds, 3.0f, 1.0e-6f);
}

TEST(FPSWeatherPort_OptionalCapabilityCanBeUnavailable)
{
    FakeWeatherPort port;
    port.enabled = false;

    EXPECT_FALSE(port.SetWeather(SparkGameFPS::WeatherPreset::Clear, 1.0f, 0.0f));
    EXPECT_EQ(port.calls, 1);
}

TEST(FPSWeatherPort_ProductionAdapterMapsToEngineWeather)
{
    Spark::WeatherSystem weather;
    SparkGameFPS::EngineWeatherAdapter adapter(&weather);

    ASSERT_TRUE(adapter.SetWeather(SparkGameFPS::WeatherPreset::Storm, 0.8f, 3.0f));
    EXPECT_EQ(static_cast<int>(weather.GetTargetState().type), static_cast<int>(Spark::WeatherType::Storm));
    EXPECT_NEAR(weather.GetTargetState().intensity, 0.8f, 1.0e-6f);
    EXPECT_TRUE(weather.IsTransitioning());
}

TEST(FPSWeatherPort_ProductionAdapterReportsMissingEngineCapability)
{
    SparkGameFPS::EngineWeatherAdapter adapter(nullptr);
    EXPECT_FALSE(adapter.SetWeather(SparkGameFPS::WeatherPreset::Clear, 1.0f, 0.0f));
}

TEST(FPSWeatherIntegration_InitializesExactlyOnceAndClears)
{
    FakeWeatherPort port;
    SparkGameFPS::FPSWeatherIntegration integration;
    integration.Bind(&port);

    EXPECT_TRUE(integration.Initialize());
    EXPECT_TRUE(integration.Initialize());
    EXPECT_EQ(port.calls, 1);

    integration.Clear();
    EXPECT_FALSE(integration.Initialize());
    EXPECT_FALSE(integration.SetWeather(SparkGameFPS::WeatherPreset::Rain, 0.5f, 1.0f));
    EXPECT_EQ(port.calls, 1);
}

TEST(FPSWeatherIntegration_RejectsWrongThreadWithoutMutation)
{
    FakeWeatherPort port;
    SparkGameFPS::FPSWeatherIntegration integration;
    integration.Bind(&port);

    bool initializedOnWorker = true;
    std::thread worker([&] { initializedOnWorker = integration.Initialize(); });
    worker.join();

    EXPECT_FALSE(initializedOnWorker);
    EXPECT_EQ(port.calls, 0);
}
