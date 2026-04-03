# Day/Night Cycle and Weather

SparkEngine includes dynamic time-of-day and weather systems that affect lighting, [rendering](Rendering-and-Graphics), and [gameplay](Gameplay-Systems). Both systems output parameters that feed into the lighting pipeline, post-processing, particle system, and audio engine, enabling rich atmospheric environments.

**Source:** `SparkEngine/Source/Engine/World/TimeOfDaySystem.h`, `SparkEngine/Source/Graphics/WeatherSystem.h`

**CMake toggles:** `ENABLE_DAY_NIGHT=ON`, `ENABLE_WEATHER=ON`

---

## Architecture Overview

```
                        +-----------------------+
                        |    DayNightCycle       |
                        | (Engine/World/)        |
                        +-----------+-----------+
                                    |
            Sun direction, ambient color, sky tint, intensity
                                    |
                    +---------------+---------------+
                    |                               |
            +-------v-------+             +---------v--------+
            | Lighting      |             | Skybox / Sky     |
            | System        |             | Rendering        |
            +-------+-------+             +---------+--------+
                    |                               |
                    +------+--------+------+--------+
                           |        |      |
                    +------v--+ +---v---+ +v--------+
                    | Shadows | | Post- | | Particle|
                    |         | | Proc  | | System  |
                    +---------+ +---+---+ +----+----+
                                    |          |
                        +-----------v----------v---------+
                        |        WeatherSystem            |
                        | (Graphics/WeatherSystem.h)      |
                        | Fog, precipitation, wind, lightning |
                        +---+----------+----------+------+
                            |          |          |
                     +------v---+ +----v----+ +---v------+
                     | FogSystem| | Physics | | Audio    |
                     |          | | (wind)  | | (thunder)|
                     +----------+ +---------+ +----------+
```

The Day/Night Cycle and Weather systems are designed to be independent but complementary. The day/night cycle drives the sun position and base lighting colors, while the weather system modifies those values with atmospheric effects such as fog, rain, and overcast skies.

---

## Day/Night Cycle

### Namespace and Header

```cpp
#include "Engine/World/TimeOfDaySystem.h"

// All types live in the Spark namespace
namespace Spark {
    class DayNightCycle;
    enum class DayPeriod;
    struct Color;
    struct Vec3;
    struct TimeOfDayChangedEvent;
}
```

### Enabling in CMake

```cmake
set(ENABLE_DAY_NIGHT ON CACHE BOOL "Enable day/night cycle system")
```

### Features

- Configurable day length (real-time seconds per in-game day)
- Sun position calculated from a circular arc across the sky
- Sky color transitions (dawn, morning, midday, afternoon, dusk, evening, night)
- Ambient light intensity changes following a smooth curve
- Moon and star visibility at night (via `IsNight()` query)
- Configurable sun arc axis for different latitude simulations
- Period-change callbacks and EventBus integration
- Human-readable time string output (e.g., `"14:30"`)

### Helper Types

#### `Spark::Color`

A simple RGBA color structure used for platform-independent color representation.

```cpp
struct Color
{
    float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;

    static Color Lerp(const Color& a, const Color& b, float t);
};
```

| Field | Type    | Default | Description                    |
|-------|---------|---------|--------------------------------|
| `r`   | `float` | `1.0f`  | Red channel [0, 1]             |
| `g`   | `float` | `1.0f`  | Green channel [0, 1]           |
| `b`   | `float` | `1.0f`  | Blue channel [0, 1]            |
| `a`   | `float` | `1.0f`  | Alpha channel [0, 1]           |

#### `Spark::Vec3`

A lightweight 3D vector for sun direction output. This is platform-independent and does not depend on DirectXMath.

```cpp
struct Vec3
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
};
```

### `DayPeriod` Enum

The day is divided into eight distinct named periods:

```cpp
enum class DayPeriod
{
    Night,      // 0:00 - 5:00
    Dawn,       // 5:00 - 7:00
    Morning,    // 7:00 - 10:00
    Midday,     // 10:00 - 14:00
    Afternoon,  // 14:00 - 17:00
    Dusk,       // 17:00 - 19:00
    Evening,    // 19:00 - 21:00
    LateNight   // 21:00 - 24:00
};
```

| Time Range    | Period      | Ambient Intensity | Character           |
|---------------|-------------|-------------------|---------------------|
| 0:00 - 5:00   | Night       | 0.1               | Dark, moon/stars    |
| 5:00 - 7:00   | Dawn        | 0.1 - 0.4         | Warm orange sunrise |
| 7:00 - 10:00  | Morning     | 0.4 - 1.0         | Dawn to day blend   |
| 10:00 - 14:00 | Midday      | 1.0               | Full daylight       |
| 14:00 - 17:00 | Afternoon   | 1.0               | Full daylight       |
| 17:00 - 19:00 | Dusk        | 1.0 - 0.4         | Warm orange sunset  |
| 19:00 - 21:00 | Evening     | 0.4 - 0.1         | Dusk to night blend |
| 21:00 - 24:00 | Late Night  | 0.1               | Dark, moon/stars    |

### Color Palette

The system uses four reference color sets that are interpolated based on the current hour:

| Color Set  | Ambient (R,G,B)       | Sun (R,G,B)            | Sky (R,G,B)             |
|------------|-----------------------|------------------------|-------------------------|
| **Night**  | (0.05, 0.05, 0.15)   | (0.1, 0.1, 0.2)       | (0.02, 0.02, 0.08)     |
| **Dawn**   | (0.4, 0.3, 0.2)      | (1.0, 0.6, 0.3)       | (0.7, 0.5, 0.4)        |
| **Day**    | (0.6, 0.6, 0.7)      | (1.0, 0.95, 0.85)     | (0.4, 0.6, 0.9)        |
| **Dusk**   | (0.4, 0.25, 0.15)    | (1.0, 0.5, 0.2)       | (0.8, 0.4, 0.3)        |

### Sun Position Calculation

The sun follows a circular arc across the sky using a Y-up coordinate system:

```
          Noon (12:00)
            * (Zenith)
           /|\
          / | \
    6AM  /  |  \  18:00
   -----*---+---*----- Horizon
        |   |   |
        |   * (Nadir)
        Midnight (0:00)
```

The angle is computed as:

```
angle = (currentHour / 24.0) * 2 * PI - PI/2
```

Direction components:

```
sunDirection.x = sunArcX * cos(angle)
sunDirection.y = sin(angle)           // positive = above horizon
sunDirection.z = sunArcZ * cos(angle)
```

The sun intensity is derived from the height above the horizon:

```
sunIntensity = clamp(sunDirection.y * 2.0, 0.0, 1.0)
```

This means the sun reaches full intensity when it is at 30 degrees above the horizon or higher.

### `DayNightCycle` Class API

```cpp
class DayNightCycle
{
public:
    DayNightCycle() = default;

    // --- Frame update ---
    void Update(float dt);

    // --- Time control ---
    void SetTimeOfDay(float hour);       // Set time [0, 24)
    float GetTimeOfDay() const;          // Get time [0, 24)
    void SetTimeScale(float scale);      // Set multiplier (60 = 1 sec = 1 min)
    float GetTimeScale() const;
    void SetPaused(bool paused);
    bool IsPaused() const;
    int GetDayCount() const;             // Days elapsed since start

    // --- Output values (recomputed each Update) ---
    Vec3 GetSunDirection() const;        // Normalized sun direction (Y-up)
    Color GetAmbientColor() const;       // Current ambient color
    Color GetSunColor() const;           // Current sun/directional light color
    Color GetSkyColor() const;           // Current sky tint
    float GetSunIntensity() const;       // Sun intensity [0, 1]
    float GetAmbientIntensity() const;   // Ambient intensity [0, 1]
    bool IsNight() const;                // sunDirection.y < 0
    bool IsDay() const;                  // sunDirection.y >= 0
    DayPeriod GetDayPeriod() const;      // Current named period
    static const char* GetPeriodName(DayPeriod period);
    std::string GetTimeString() const;   // e.g., "14:30"

    // --- Configuration ---
    void SetSunArcAxis(float x, float z);
    void SetOnPeriodChanged(std::function<void(DayPeriod, DayPeriod)> callback);
    void SetEventBus(EventBus* bus);
};
```

### Member Variables

| Member            | Type       | Default  | Description                           |
|-------------------|------------|----------|---------------------------------------|
| `m_currentHour`   | `float`    | `12.0f`  | Current time of day [0, 24)           |
| `m_timeScale`     | `float`    | `60.0f`  | Time multiplier (60 = 1 sec = 1 min)  |
| `m_paused`        | `bool`     | `false`  | Whether time is paused                |
| `m_dayCount`      | `int`      | `0`      | Number of full days elapsed           |
| `m_sunArcX`       | `float`    | `1.0f`   | Sun east-west arc axis component      |
| `m_sunArcZ`       | `float`    | `0.0f`   | Sun north-south arc axis component    |
| `m_sunDirection`   | `Vec3`     | `{0,1,0}`| Computed sun direction                |
| `m_sunIntensity`   | `float`    | `1.0f`   | Computed sun intensity [0, 1]         |
| `m_ambientIntensity`| `float`  | `1.0f`   | Computed ambient intensity [0, 1]     |
| `m_onPeriodChanged`| callback  | `nullptr`| Period transition callback             |
| `m_eventBus`      | `EventBus*`| `nullptr`| EventBus for publishing events        |

### `TimeOfDayChangedEvent`

Published via the `EventBus` whenever the day period changes:

```cpp
struct TimeOfDayChangedEvent {
    float previousHour;
    float currentHour;
    int dayCount;
};
```

### Usage Example

```cpp
#include "Engine/World/TimeOfDaySystem.h"
#include "Engine/Events/EventSystem.h"

Spark::EventBus eventBus;
Spark::DayNightCycle cycle;

// Configure
cycle.SetTimeOfDay(6.0f);       // Start at 6 AM
cycle.SetTimeScale(120.0f);     // 1 real second = 2 game minutes
cycle.SetSunArcAxis(1.0f, 0.3f); // Slight tilt for northern latitude feel
cycle.SetEventBus(&eventBus);

// Register period change callback
cycle.SetOnPeriodChanged([](Spark::DayPeriod prev, Spark::DayPeriod curr) {
    std::cout << "Period changed: "
              << Spark::DayNightCycle::GetPeriodName(prev) << " -> "
              << Spark::DayNightCycle::GetPeriodName(curr) << "\n";
});

// Subscribe to events
eventBus.Subscribe<Spark::TimeOfDayChangedEvent>([](const Spark::TimeOfDayChangedEvent& e) {
    std::cout << "Day " << e.dayCount << " | " << e.currentHour << "h\n";
});

// Game loop
while (running)
{
    cycle.Update(deltaTime);

    // Feed into lighting system
    auto sunDir = cycle.GetSunDirection();
    auto sunColor = cycle.GetSunColor();
    float sunIntensity = cycle.GetSunIntensity();
    auto ambientColor = cycle.GetAmbientColor();
    float ambientIntensity = cycle.GetAmbientIntensity();
    auto skyColor = cycle.GetSkyColor();

    lighting.SetDirectionalLight(sunDir, sunColor, sunIntensity);
    lighting.SetAmbientLight(ambientColor, ambientIntensity);
    skybox.SetTint(skyColor);

    // Display time in UI
    std::string timeStr = cycle.GetTimeString(); // "06:00", "14:30", etc.
}
```

### Time Scale Reference

| `SetTimeScale()` value | Meaning                          | Full day-night cycle |
|------------------------|----------------------------------|----------------------|
| 1.0                    | Real-time (1 sec = 1 sec)        | 24 hours real-time   |
| 60.0 (default)         | 1 real sec = 1 game minute       | 24 minutes           |
| 120.0                  | 1 real sec = 2 game minutes      | 12 minutes           |
| 360.0                  | 1 real sec = 6 game minutes      | 4 minutes            |
| 3600.0                 | 1 real sec = 1 game hour         | 24 seconds           |

### Integrating with Shadows

When the sun is below the horizon (`IsNight()` returns true), you can switch from a directional sun shadow map to a moon-based or ambient-only shadow configuration:

```cpp
if (cycle.IsNight())
{
    shadowSystem.DisableDirectionalShadow();
    // Optionally enable a dim moonlight shadow
}
else
{
    shadowSystem.SetDirectionalLightDirection(cycle.GetSunDirection());
    shadowSystem.EnableDirectionalShadow();
}
```

---

## Weather System

### Namespace and Header

```cpp
#include "Graphics/WeatherSystem.h"

// All types live in the Spark namespace
namespace Spark {
    class WeatherSystem;
    enum class WeatherType;
    enum class WindDirection;
    struct WeatherState;
    struct WeatherChangedEvent;
}
```

### Enabling in CMake

```cmake
set(ENABLE_WEATHER ON CACHE BOOL "Enable weather system")
```

### `WeatherType` Enum

```cpp
enum class WeatherType
{
    Clear,   // Clear sky, no precipitation
    Cloudy,  // Overcast sky, no precipitation
    Rain,    // Rainfall with wet surfaces
    Snow,    // Snowfall with ground accumulation
    Fog,     // Dense fog reducing visibility
    Storm,   // Heavy rain with lightning and wind
    Count
};
```

### `WindDirection` Enum

```cpp
enum class WindDirection
{
    North, South, East, West,
    NorthEast, NorthWest, SouthEast, SouthWest
};
```

### `WeatherState` Struct

The complete description of atmospheric conditions. The WeatherSystem interpolates between two `WeatherState` instances during transitions.

```cpp
struct WeatherState
{
    WeatherType type = WeatherType::Clear;

    // Precipitation
    float intensity = 0.0f;          // Overall intensity [0, 1]
    float precipitationRate = 0.0f;  // Particles/second for rain/snow
    float precipitationSize = 1.0f;  // Scale of precipitation particles

    // Wind
    XMFLOAT3 windDirection = {1.0f, 0.0f, 0.0f};
    float windSpeed = 0.0f;          // Wind speed in m/s
    float windGustiness = 0.0f;      // Random wind variation [0, 1]

    // Atmosphere
    float fogDensity = 0.0f;         // Volumetric fog density [0, 1]
    float fogStartDistance = 50.0f;  // Fog start in meters
    float fogEndDistance = 500.0f;   // Fog end in meters
    XMFLOAT4 fogColor = {0.7f, 0.7f, 0.8f, 1.0f};

    // Lighting modifiers
    float ambientMultiplier = 1.0f;
    float directionalMultiplier = 1.0f;
    XMFLOAT4 skyTint = {1.0f, 1.0f, 1.0f, 1.0f};

    // Storm-specific
    float lightningFrequency = 0.0f; // Flashes per minute
    float thunderDelay = 2.0f;       // Seconds between lightning and thunder

    // Surface effects
    float wetness = 0.0f;            // Surface wetness for rain [0, 1]
    float snowCoverage = 0.0f;       // Snow ground coverage [0, 1]
};
```

### Weather Preset Defaults

The `GetWeatherPreset()` function returns a default `WeatherState` for each weather type:

| Parameter              | Clear | Cloudy | Rain   | Snow   | Fog    | Storm  |
|------------------------|-------|--------|--------|--------|--------|--------|
| `intensity`            | 0.0   | 0.3    | 0.7    | 0.6    | 0.8    | 1.0    |
| `precipitationRate`    | 0     | 0      | 500    | 300    | 0      | 1000   |
| `precipitationSize`    | 1.0   | 1.0    | 0.8    | 1.5    | 1.0    | 1.2    |
| `windSpeed` (m/s)      | 0     | 0      | 5      | 3      | 0      | 15     |
| `windGustiness`        | 0     | 0      | 0.3    | 0.2    | 0      | 0.8    |
| `fogDensity`           | 0     | 0.05   | 0.1    | 0.15   | 0.6    | 0.2    |
| `fogStartDistance`     | 50    | 50     | 50     | 50     | 10     | 50     |
| `fogEndDistance`       | 500   | 500    | 500    | 500    | 100    | 500    |
| `ambientMultiplier`    | 1.0   | 0.7    | 0.5    | 0.8    | 0.6    | 0.3    |
| `directionalMultiplier`| 1.0   | 0.5    | 0.3    | 0.6    | 0.2    | 0.1    |
| `skyTint` (R,G,B)      | white | 0.8,0.8,0.85 | 0.6,0.6,0.7 | 0.9,0.9,0.95 | 0.7,0.7,0.75 | 0.4,0.4,0.5 |
| `lightningFrequency`  | 0     | 0      | 0      | 0      | 0      | 8.0    |
| `thunderDelay`         | 2.0   | 2.0    | 2.0    | 2.0    | 2.0    | 3.0    |
| `wetness`              | 0     | 0      | 0.8    | 0      | 0      | 1.0    |
| `snowCoverage`         | 0     | 0      | 0      | 0.7    | 0      | 0      |

### `WeatherSystem` Class API

```cpp
class WeatherSystem
{
public:
    using WeatherCallback = std::function<void(WeatherType oldType, WeatherType newType)>;

    WeatherSystem();

    // --- Weather control ---
    void SetWeather(WeatherType type, float intensity = -1.0f, float transitionTime = 3.0f);
    void SetWind(float x, float y, float z, float speed);
    void SetIntensity(float intensity);

    // --- Frame update ---
    void Update(float dt);

    // --- Accessors ---
    const WeatherState& GetCurrentState() const;
    const WeatherState& GetTargetState() const;
    bool IsTransitioning() const;
    float GetTransitionProgress() const;  // [0, 1]
    float GetEffectiveWindSpeed() const;  // windSpeed + gusts
    float GetLightningFlash() const;      // [0, 1]

    // --- Configuration ---
    void SetOnWeatherChanged(WeatherCallback callback);
    void SetEventBus(EventBus* bus);
    void SetFogSystem(Graphics::FogSystem* fog);

    // --- Utility ---
    static const char* GetWeatherTypeName(WeatherType type);
};
```

### Transitions

Weather changes smoothly between types using a configurable transition duration. The interpolation uses a SmoothStep function for ease-in/ease-out behavior:

```
smoothstep(t) = t * t * (3 - 2 * t)
```

All fields in `WeatherState` are linearly interpolated, including fog parameters, precipitation rates, lighting multipliers, wind speed, and surface effects. The `type` field snaps at the halfway point of the transition.

```
Time ─────────────────────────────────────────>
      SetWeather(Rain)
            |
            v
Current: [Clear]──────interpolate──────>[Rain]
Progress:  0.0    0.25    0.5    0.75    1.0
                                          |
                                  callback fires
                            WeatherChangedEvent published
```

### Wind Simulation

Wind gusts are simulated using overlapping sine waves for organic variation:

```
gust = sin(timer * 2.3) * 0.5 + sin(timer * 5.7) * 0.3
effectiveGust = gust * windGustiness * windSpeed
totalWindSpeed = windSpeed + effectiveGust
```

The effective wind speed is available via `GetEffectiveWindSpeed()` and can be fed into physics for projectile deflection and particle drift.

### Lightning System

During storms, lightning flashes occur at intervals determined by `lightningFrequency` (flashes per minute):

```
interval = 60.0 / lightningFrequency
```

When a flash triggers, the `m_lightningFlash` value is set to 1.0 and decays at a rate of 8.0 per second. The current flash intensity is available via `GetLightningFlash()` and can be used to:

- Temporarily boost the ambient light intensity
- Flash the screen white in post-processing
- Trigger a thunder audio cue after `thunderDelay` seconds

### FogSystem Integration

When a `FogSystem` pointer is provided via `SetFogSystem()`, the weather system automatically syncs fog parameters every frame:

```cpp
// Automatic sync performed in Update():
fogSystem->SetDensity(currentState.fogDensity);
fogSystem->SetLinearRange(currentState.fogStartDistance, currentState.fogEndDistance);
fogSystem->SetColor(currentState.fogColor);
fogSystem->SetEnabled(currentState.fogDensity > 0.0f);
```

This eliminates the need to manually pipe fog parameters from the weather system to the fog system.

### `WeatherChangedEvent`

Published via the `EventBus` when a weather transition completes:

```cpp
struct WeatherChangedEvent {
    int previousType;   // Cast from WeatherType
    int newType;        // Cast from WeatherType
    float intensity;    // Final intensity [0.0 to 1.0]
};
```

### Usage Example

```cpp
#include "Graphics/WeatherSystem.h"
#include "Graphics/FogSystem.h"
#include "Engine/Events/EventSystem.h"

Spark::EventBus eventBus;
Spark::Graphics::FogSystem fogSystem;
Spark::WeatherSystem weather;

// Wire up dependencies
weather.SetEventBus(&eventBus);
weather.SetFogSystem(&fogSystem);

// Register callback
weather.SetOnWeatherChanged([](Spark::WeatherType oldType, Spark::WeatherType newType) {
    std::cout << "Weather: " << Spark::WeatherSystem::GetWeatherTypeName(oldType)
              << " -> " << Spark::WeatherSystem::GetWeatherTypeName(newType) << "\n";
});

// Start raining at 80% intensity with 5-second transition
weather.SetWeather(Spark::WeatherType::Rain, 0.8f, 5.0f);

// Game loop
while (running)
{
    weather.Update(deltaTime);

    const auto& state = weather.GetCurrentState();

    // Apply to lighting
    lighting.SetAmbientMultiplier(state.ambientMultiplier);
    lighting.SetDirectionalMultiplier(state.directionalMultiplier);
    skybox.SetTint(state.skyTint);

    // Feed precipitation to particle system
    if (state.precipitationRate > 0.0f)
    {
        particleSystem.SetRainRate(state.precipitationRate);
        particleSystem.SetRainSize(state.precipitationSize);
    }

    // Apply surface wetness to materials
    materialSystem.SetGlobalWetness(state.wetness);
    materialSystem.SetSnowCoverage(state.snowCoverage);

    // Handle lightning flash
    float flash = weather.GetLightningFlash();
    if (flash > 0.0f)
    {
        postProcessing.SetFlashIntensity(flash);
        if (flash > 0.9f) // Just triggered
        {
            audioSystem.PlayThunder(state.thunderDelay);
        }
    }

    // Wind affects physics
    float windSpeed = weather.GetEffectiveWindSpeed();
    physics.SetWindForce(state.windDirection, windSpeed);
}
```

### Combining Day/Night and Weather

The two systems can work together. Weather modifies the base lighting that the day/night cycle produces:

```cpp
// In the render loop:
Spark::Color ambient = cycle.GetAmbientColor();
float ambientIntensity = cycle.GetAmbientIntensity();
const auto& weatherState = weather.GetCurrentState();

// Weather modulates the day/night ambient
float finalAmbient = ambientIntensity * weatherState.ambientMultiplier;
Spark::Color finalSky = {
    cycle.GetSkyColor().r * weatherState.skyTint.x,
    cycle.GetSkyColor().g * weatherState.skyTint.y,
    cycle.GetSkyColor().b * weatherState.skyTint.z,
    1.0f
};
```

### Effects on Gameplay

| Effect                     | Systems Affected                                     |
|----------------------------|------------------------------------------------------|
| Reduced visibility         | AI sight range, player view distance (fog, rain)     |
| Wet surfaces               | [Physics](Physics) friction coefficients reduced     |
| Wind                       | Projectile trajectories, particle drift              |
| Lightning illumination     | Brief ambient boost, screen flash                    |
| Snow coverage              | Terrain rendering, footstep audio changes            |
| Surface wetness            | PBR material roughness modification                  |
| Overcast sky               | Reduced shadow contrast, softer lighting             |

---

## Console Commands

### Day/Night Commands

| Command                     | Description                              | Example              |
|-----------------------------|------------------------------------------|----------------------|
| `time_set <hour>`           | Set time of day (0-24)                   | `time_set 6.5`       |
| `time_speed <multiplier>`   | Set time speed (1.0 = real-time)         | `time_speed 120`     |
| `time_pause`                | Pause/unpause time progression           | `time_pause`         |

### Weather Commands

| Command                     | Description                              | Example              |
|-----------------------------|------------------------------------------|----------------------|
| `weather_set <type>`        | Set weather type                         | `weather_set rain`   |
| `weather_intensity <val>`   | Set weather intensity (0.0-1.0)          | `weather_intensity 0.5` |
| `weather_transition <sec>`  | Set transition duration in seconds       | `weather_transition 10` |

Valid weather type names for `weather_set`: `clear`, `cloudy`, `rain`, `storm`, `snow`, `fog`.

---

## Performance Considerations

- **Update cost**: Both systems perform only arithmetic and interpolation in their `Update()` calls. There are no allocations, GPU calls, or heavy computations.
- **Particle overhead**: Precipitation particles are the main cost. Use `precipitationRate` scaling to limit particles on lower-end hardware.
- **Fog rendering**: Weather fog delegates to the `FogSystem`, which uses GPU-side volumetric or linear fog. The CPU cost is negligible; the GPU cost depends on the fog rendering technique.
- **Transition frequency**: Avoid triggering very rapid weather transitions (under 0.5 seconds) as the interpolation may cause visual popping.

---

## Troubleshooting

| Problem                              | Cause                                      | Solution                                    |
|--------------------------------------|--------------------------------------------|---------------------------------------------|
| Sun direction not changing           | `Update()` not called, or time is paused   | Ensure `Update(dt)` is called every frame; check `IsPaused()` |
| Weather stuck mid-transition         | Transition time extremely long             | Reduce `transitionTime` parameter           |
| No fog despite fog weather type      | `FogSystem` not connected                  | Call `weather.SetFogSystem(&fogSystem)`      |
| Lightning flash not visible          | `GetLightningFlash()` not consumed         | Read flash value and apply to post-processing |
| Period change callback never fires   | EventBus not set or callback not registered| Call `SetEventBus()` and `SetOnPeriodChanged()` |
| Colors appear wrong at dawn/dusk     | Time scale too fast, skipping transitions  | Lower time scale or add interpolation buffers |

---

## See Also

- [Rendering and Graphics](Rendering-and-Graphics) — Volumetric lighting and fog
- [Event System](Event-System) — Weather and time-of-day events
- [Physics](Physics) — Weather effects on physics
- [Audio](Audio) — Weather-driven audio (thunder, rain, wind)
- [Gameplay Systems](Gameplay-Systems) — Weather impact on gameplay mechanics
- [Terrain and Procedural Generation](Terrain-and-Procedural-Generation) — Terrain interaction with weather
- [Entity-Component-System](Entity-Component-System) — ECS integration for weather-aware entities
