# Day/Night Cycle and Weather

SparkEngine includes dynamic time-of-day and weather systems that affect lighting, rendering, and gameplay.

**Source:** `SparkEngine/Source/Engine/World/DayNightCycle.h`, `SparkEngine/Source/Graphics/WeatherSystem.h`

## Day/Night Cycle

`ENABLE_DAY_NIGHT=ON`

The `DayNightCycle` system simulates a full day with dynamic lighting transitions:

### Features

- Configurable day length (real-time seconds per in-game day)
- Sun position calculated from time of day
- Sky color transitions (dawn → day → dusk → night)
- Ambient light intensity changes
- Moon and star visibility at night

### Time of Day

The system uses a 24-hour clock (0.0 = midnight, 12.0 = noon):

| Time Range | Period |
|-----------|--------|
| 0.0 – 5.0 | Night |
| 5.0 – 7.0 | Dawn |
| 7.0 – 17.0 | Day |
| 17.0 – 19.0 | Dusk |
| 19.0 – 24.0 | Night |

### Events

The system publishes `TimeOfDayChangedEvent` at significant transitions:

```cpp
struct TimeOfDayChangedEvent {
    float previousHour;
    float currentHour;
    int dayCount;
};
```

## Weather System

`ENABLE_WEATHER=ON`

Dynamic weather with configurable types and smooth transitions:

### Weather Types

| Type | Visual Effects |
|------|---------------|
| Clear | Blue sky, full sun |
| Cloudy | Overcast sky, reduced sunlight |
| Rain | Particle rain, wet surfaces, reduced visibility |
| Storm | Heavy rain, lightning, thunder audio, wind |
| Snow | Particle snow, frost effects |
| Fog | Dense fog, severely reduced visibility |

### Transitions

Weather changes smoothly between types over a configurable transition duration. The system can run on a schedule or be triggered manually.

### Events

```cpp
struct WeatherChangedEvent {
    int previousType;
    int newType;
    float intensity;   // 0.0 to 1.0
};
```

### Effects on Gameplay

- Reduced visibility in fog, rain, and storms
- Wet surfaces affect physics friction
- Wind affects projectile trajectories
- Lightning provides brief illumination

## Console Commands

```
time_set <hour>          # Set time of day (0-24)
time_speed <multiplier>  # Set time speed (1.0 = real-time)
time_pause               # Pause time progression
weather_set <type>       # Set weather (clear/cloudy/rain/storm/snow/fog)
weather_intensity <val>  # Set weather intensity (0.0-1.0)
weather_transition <sec> # Set transition duration
```

## See Also

- [[Rendering and Graphics]] — Volumetric lighting and fog
- [[Event System]] — Weather and time-of-day events
- [[Physics]] — Weather effects on physics
