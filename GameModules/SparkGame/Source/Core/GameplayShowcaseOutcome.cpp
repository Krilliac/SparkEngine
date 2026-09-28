/**
 * @file GameplayShowcaseOutcome.cpp
 * @brief GameplayShowcase::GetOutcome / FormatOutcome — the showcase_outcome record
 *
 * Kept apart from GameplayShowcase.cpp, which already holds the subsystem demos; this is the same class.
 */

#include "GameplayShowcase.h"

#include "Engine/ECS/Components.h"
#include "Engine/World/TimeOfDaySystem.h"
#include "Graphics/WeatherSystem.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>

ShowcaseOutcome GameplayShowcase::GetOutcome() const
{
    ShowcaseOutcome outcome;
    outcome.coroutineStage = m_coroutineStage;
    outcome.totalDamageEvents = m_totalDamageEvents;
    outcome.totalKillEvents = m_totalKillEvents;
    outcome.totalWeatherChanges = m_totalWeatherChanges;
    outcome.exhibitPlaced = static_cast<uint32_t>(m_exhibitEntities.size());
    outcome.spawnedCount = static_cast<uint32_t>(m_spawnedEntities.size());

    World* world = m_context ? m_context->GetWorld() : nullptr;
    if (world)
    {
        if (m_coroutineTarget && world->GetRegistry().valid(static_cast<EntityID>(*m_coroutineTarget)))
        {
            if (const auto* health = world->GetComponent<HealthComponent>(static_cast<EntityID>(*m_coroutineTarget)))
                outcome.coroutineTargetHealth = static_cast<int>(std::lround(health->health));
        }

        // A prop counts as resolved when its mesh file exists where the asset pipeline opens a relative mesh
        // path: under the working directory, which is the installed bin directory in a packaged run.
        for (uint32_t entityId : m_exhibitEntities)
        {
            const auto entity = static_cast<EntityID>(entityId);
            if (!world->GetRegistry().valid(entity))
                continue;
            const auto* renderer = world->GetComponent<MeshRenderer>(entity);
            std::error_code error;
            if (renderer && !renderer->meshPath.empty() &&
                std::filesystem::is_regular_file(std::filesystem::path(renderer->meshPath), error))
                ++outcome.exhibitResolved;
        }
    }

    if (const auto* weather = m_context ? m_context->GetWeather() : nullptr)
        outcome.weatherName = Spark::WeatherSystem::GetWeatherTypeName(weather->GetCurrentState().type);

    if (const auto* timeOfDay = m_context ? m_context->GetTimeOfDay() : nullptr)
        outcome.hourHundredths = static_cast<int>(std::lround(timeOfDay->GetTimeOfDay() * 100.0f));

    return outcome;
}

std::string GameplayShowcase::FormatOutcome(const ShowcaseOutcome& outcome)
{
    std::string stage = outcome.coroutineStage;
    std::replace(stage.begin(), stage.end(), ' ', '_');
    const std::string targetHealth =
        outcome.coroutineTargetHealth < 0 ? std::string("n/a") : std::to_string(outcome.coroutineTargetHealth);
    const std::string hour = outcome.hourHundredths < 0
                                 ? std::string("n/a")
                                 : std::format("{}.{:02}", outcome.hourHundredths / 100, outcome.hourHundredths % 100);
    return std::format("SPARK_SHOWCASE_OUTCOME stage={} target_hp={} damage_events={} kill_events={} "
                       "weather_changes={} weather={} hour={} exhibit={}/{} spawned={}",
                       stage, targetHealth, outcome.totalDamageEvents, outcome.totalKillEvents,
                       outcome.totalWeatherChanges, outcome.weatherName, hour, outcome.exhibitPlaced,
                       outcome.exhibitResolved, outcome.spawnedCount);
}
