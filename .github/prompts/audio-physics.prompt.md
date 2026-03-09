# Audio & Physics

Context: `#prompt:copilot-instructions` for project overview. Console commands: see `console-scripting` prompt.

## Audio System

`AudioEngine` (`SparkEngine/Source/Audio/AudioEngine.h`) — XAudio2 with 3D spatial audio. Thread-safe (mutex-protected).

### Core Type

```cpp
struct AudioSource {
    IXAudio2SourceVoice* Voice;
    XMFLOAT3 Position, Velocity;
    float Volume, Pitch;
    bool Is3D, IsLooping, IsPlaying;
    SoundEffect* Sound;
    uint32_t SourceID;
};
```

### Features

- 3D spatial: listener position/orientation, distance attenuation, Doppler
- Voice pooling: 100+ concurrent sources
- Volume channels: Master, SFX, Music
- Formats: WAV, MP3 via `SoundEffect` (`Audio/SoundEffect.h`)

### Usage

```cpp
auto& audio = *context.GetAudio();
audio.Initialize();
auto sfx = audio.LoadSound("Assets/Sounds/explosion.wav");
audio.Play3D(sfx, position, velocity, volume, pitch);
```

---

## Physics System

`PhysicsSystem` (`SparkEngine/Source/Physics/PhysicsSystem.h`) — Bullet Physics 3 with DirectXMath-native API. **NOT thread-safe** (main thread only).

### Architecture

| Class | Purpose |
|-------|---------|
| `PhysicsBody` | Wraps `btRigidBody`, `XMFLOAT3`/`XMMATRIX` API |
| `PhysicsConstraint` | Wraps `btTypedConstraint` (hinge, slider, fixed) |
| `PhysicsSystem` | World manager: lifecycle, step, queries, callbacks |

### Body Creation

```cpp
PhysicsBodyDesc desc;
desc.type             = PhysicsBodyType::Dynamic;  // Static, Kinematic, Dynamic
desc.position         = {0, 5, 0};
desc.mass             = 10.0f;
desc.shape.type       = CollisionShapeType::Box;   // Box, Sphere, Capsule, Cylinder, Cone, Mesh, ConvexHull, Heightfield, Compound
desc.shape.dimensions = {1, 1, 1};
auto body = physics.CreateBody(desc);
```

### Queries & Callbacks

```cpp
physics.Raycast(origin, direction, maxDistance);     // single hit
physics.RaycastAll(origin, direction, maxDistance);   // all hits
physics.SphereOverlap(center, radius);
physics.BoxOverlap(center, halfExtents);

physics.SetCollisionCallback([](PhysicsBody* a, PhysicsBody* b, const ContactPoint& cp) { });
physics.SetTriggerCallback([](PhysicsBody* trigger, PhysicsBody* other, bool entered) { });
```

### Collision System (`Physics/CollisionSystem.h`)

Standalone geometric tests (no Bullet dependency), all stateless and thread-safe, uses DirectXMath SIMD.

- Tests: `SphereVsSphere`, `SphereVsBox`, `BoxVsBox`, `RayVsSphere`, `RayVsBox`, `RayVsTriangle`
- Utilities: `Vector3Normalize`, `Vector3Dot`, `Vector3Cross`, `Vector3Reflect`, `Vector3Lerp`
