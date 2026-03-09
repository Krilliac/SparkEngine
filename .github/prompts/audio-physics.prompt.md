# Audio & Physics

Context: `#prompt:copilot-instructions` for project overview.

## Audio System

`AudioEngine` (`SparkEngine/Source/Audio/AudioEngine.h`) — XAudio2-based with 3D spatial audio.

### Core Types

```cpp
struct AudioSource {
    IXAudio2SourceVoice* Voice;
    XMFLOAT3 Position;       // 3D world position
    XMFLOAT3 Velocity;       // For Doppler effects
    float Volume, Pitch;
    bool Is3D, IsLooping, IsPlaying;
    SoundEffect* Sound;
    uint32_t SourceID;       // Console tracking ID
};
```

### Features

- **3D spatial audio**: Listener position/orientation, distance attenuation, Doppler
- **Voice pooling**: Object pool for efficient source management (100+ concurrent sources)
- **Volume channels**: Master, SFX, Music — independently adjustable
- **Formats**: WAV, MP3 via `SoundEffect` (`Audio/SoundEffect.h`)
- **Thread safety**: Mutex-protected internal state

### Usage Pattern

```cpp
auto& audio = *context.GetAudio();
audio.Initialize();
auto sfx = audio.LoadSound("Assets/Sounds/explosion.wav");
audio.Play3D(sfx, position, velocity, volume, pitch);
```

### Console Commands

| Command | Description |
|---------|-------------|
| `audio_master_volume <0-1>` | Set master volume |
| `audio_sfx_volume <0-1>` | Set SFX volume |
| `audio_music_volume <0-1>` | Set music volume |
| `audio_debug` | Toggle audio debug overlay |
| `sound_play <name>` | Play sound by name |
| `audio_doppler_scale <f>` | Adjust Doppler intensity |
| `audio_listener_position` | Show current listener position |

---

## Physics System

`PhysicsSystem` (`SparkEngine/Source/Physics/PhysicsSystem.h`) — Bullet Physics 3 wrapper with DirectXMath-native API.

### Architecture

| Class | Purpose |
|-------|---------|
| `PhysicsBody` | Wraps `btRigidBody`, exposes `XMFLOAT3`/`XMMATRIX` API |
| `PhysicsConstraint` | Wraps `btTypedConstraint` (hinge, slider, fixed) |
| `PhysicsSystem` | World manager: lifecycle, step, queries, callbacks |

### Body Types & Shapes

```cpp
enum PhysicsBodyType { Static, Kinematic, Dynamic };
enum CollisionShapeType { Box, Sphere, Capsule, Cylinder, Cone, Mesh, ConvexHull, Heightfield, Compound };
```

### Queries

- `Raycast(origin, direction, maxDistance)` — single hit
- `RaycastAll(origin, direction, maxDistance)` — all hits
- `SphereOverlap(center, radius)` — overlap test
- `BoxOverlap(center, halfExtents)` — overlap test

### Callbacks

```cpp
physics.SetCollisionCallback([](PhysicsBody* a, PhysicsBody* b, const ContactPoint& cp) {
    // Collision response
});
physics.SetTriggerCallback([](PhysicsBody* trigger, PhysicsBody* other, bool entered) {
    // Trigger enter/exit
});
```

### Thread Safety

**PhysicsSystem is NOT thread-safe.** Call all methods from the main game thread. Simulation runs synchronously inside `Update(deltaTime)`.

### Collision System

`CollisionSystem` (`Physics/CollisionSystem.h`) — Standalone geometric collision tests (no Bullet dependency).

- `SphereVsSphere`, `SphereVsBox`, `BoxVsBox`
- `RayVsSphere`, `RayVsBox`, `RayVsTriangle`
- Vector utilities: `Vector3Normalize`, `Vector3Dot`, `Vector3Cross`, `Vector3Reflect`, `Vector3Lerp`
- Uses DirectXMath SIMD. All methods are stateless and thread-safe.

### Console Commands

| Command | Description |
|---------|-------------|
| `physics_debug` | Toggle physics debug visualization |
| `gravity <x> <y> <z>` | Set world gravity |
| `physics_step` | Single-step physics simulation |
| `raycast_test` | Fire test raycast from camera |
| `collision_stats` | Show collision pair counts |
| `collision_test` | Run collision system diagnostics |
