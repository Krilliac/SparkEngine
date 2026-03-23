# 12 — Gameplay Systems

Covers all gameplay-oriented subsystems: destruction, dialogue, events, loading, localization, modding, replay, save, UI, VR, 2D physics, coroutines, cinematic sequencer, and mobile platform.

---

## Destruction System

**Location:** `SparkEngine/Source/Engine/Destruction/`

Destructible environment for FPS gameplay:

```cpp
DestructionSystem& destruction = DestructionSystem::GetInstance();
destruction.Initialize();
destruction.SetWorld(world);

// Apply damage (spawns debris, plays effects)
destruction.ApplyDamage(entityId, 50.0f, hitPoint, hitDirection);
destruction.SetMaxDebris(500);  // Performance limit
```

### Built-in Fracture Patterns

| Pattern | Pieces | Sound | Particles |
|---------|--------|-------|-----------|
| `wooden_crate` | 4 planks | Wood break | Wood splinters |
| `metal_barrel` | Shell + fragments | Metal break | Sparks |
| `concrete_wall` | 8 chunks | Concrete crash | Concrete dust |

### Custom Patterns

```cpp
FracturePattern pattern;
pattern.name = "glass_window";
pattern.pieces = {
    {meshPath: "shard1.obj", mass: 0.1f, offset: {0, 0, 0}},
    {meshPath: "shard2.obj", mass: 0.1f, offset: {0.2, 0, 0}},
};
pattern.breakSound = "glass_shatter";
pattern.particleEffect = "glass_particles";
destruction.RegisterPattern(pattern);
```

---

## Dialogue System

**Location:** `SparkEngine/Source/Engine/Dialogue/`

Two complementary dialogue systems:

### Tree-Based Dialogue (DialogueSystem)

Branching dialogue trees for story-driven content:

```cpp
DialogueSystem dialogue;
dialogue.LoadTree("guard_talk", "Data/Dialogue/guard.json");

dialogue.RegisterCondition("hasItem", [](const std::string& param) {
    return inventory.Has(param);
});

dialogue.StartConversation("guard_talk");
dialogue.Update(deltaTime);

if (dialogue.IsConversationActive()) {
    auto text = dialogue.GetCurrentText();
    auto choices = dialogue.GetAvailableChoices();
    dialogue.SelectChoice(0);
}
```

Node types: `Text`, `Choice`, `Branch` (conditions), `Event`, `End`

### Dynamic Response System (DRS)

CryEngine-style signal-based reactive dialogue:

```cpp
auto& drs = DynamicResponseSystem::GetInstance();
drs.SetVariable("player_health_low", 0.0f);

ResponseRule rule;
rule.signalName = "OnPlayerSpotted";
rule.conditions = {{"player_health_low", Op::Equal, 0.0f}};
rule.actions = {{Type::Speak, "There he is!"}};
rule.priority = 10;
rule.cooldown = 5.0f;
drs.RegisterRule(rule);

drs.SendSignal("OnPlayerSpotted", npcEntity);
drs.Update(deltaTime);
```

Action types: `Speak`, `SetVariable`, `SendSignal`, `Wait`, `Custom`

---

## Event System

**Location:** `SparkEngine/Source/Engine/Events/`

Engine-wide pub/sub event bus with deferred dispatch:

```cpp
// Subscribe
auto handle = EventBus::Global().Subscribe<EntityDamagedEvent>(
    [](const EntityDamagedEvent& e) {
        ApplyDamageEffects(e.entityId, e.damage);
    });

// Publish (immediate)
EventBus::Global().Publish(EntityDamagedEvent{
    .entityId = 1, .damage = 50.0f, .attackerId = 2
});

// Deferred (thread-safe queuing, main-thread dispatch)
QueuedEventBus queue;
queue.QueueEvent(CollisionEvent{entityA, entityB, 42.0f});
queue.DispatchAll(EventBus::Global());  // On main thread
```

### Built-in Event Types

| Category | Events |
|----------|--------|
| Gameplay | EntityDamagedEvent, EntityKilledEvent, ItemPickedUpEvent, QuestCompletedEvent |
| Weather | WeatherChangedEvent, TimeOfDayChangedEvent |
| Lifecycle | EngineStartEvent, EngineShutdownEvent, FrameBeginEvent, FrameEndEvent |
| Scene | SceneLoadedEvent, SceneUnloadedEvent |
| Entity | EntityCreatedEvent, EntityDestroyedEvent, EntityDeathEvent |
| Physics | TriggerEnterEvent, TriggerExitEvent, CollisionEvent |
| Input | InputActionEvent, MouseMoveEvent |
| Graphics | WindowResizeEvent, QualityChangedEvent |
| Audio | SoundPlayedEvent |
| Memory | MemoryPressureEvent |

---

## Loading Screen

**Location:** `SparkEngine/Source/Engine/Loading/`

Async asset loading with progress tracking:

```cpp
LoadingScreen loader;
loader.SetBackgroundImage("Data/Textures/loading_bg.png");
loader.AddLoadingTip("Press SPACE to jump");

loader.BeginLoading("Level 1");
loader.AddTask("meshes", 0.4f, []() { LoadAllMeshes(); });
loader.AddTask("textures", 0.3f, []() { LoadAllTextures(); });
loader.AddTask("audio", 0.2f, []() { LoadAllAudio(); });
loader.AddTask("navmesh", 0.1f, []() { BuildNavMesh(); });

loader.OnProgress([](float progress, const std::string& msg) {
    UpdateProgressBar(progress);
});

loader.Execute();
```

States: `Idle → Loading → Completed | Failed | Cancelled`

---

## Localization System

**Location:** `SparkEngine/Source/Engine/Localization/`

Runtime language switching without restart:

```cpp
auto& loc = LocalizationSystem::Get();
loc.LoadLanguage("en", "Data/Localization/en.json");
loc.LoadLanguage("fr", "Data/Localization/fr.json");
loc.SetCurrentLanguage("en");

std::string text = loc.GetString("menu.play");          // "Play"
std::string fmt = loc.Format("hud.ammo", 30, 120);      // "Ammo: 30/120"

loc.OnLanguageChanged([](const std::string& lang) {
    RefreshAllUI();
});
```

JSON format: `{"menu.play": "Play", "hud.ammo": "Ammo: {0}/{1}"}`

---

## Modding System

**Location:** `SparkEngine/Source/Engine/Modding/`

### ModSystem — Mod Loading

```cpp
ModSystem modSystem;
modSystem.ScanForMods("Data/Mods/");
modSystem.EnableMod("awesome_weapons");
modSystem.SetLoadOrder({"base_mod", "awesome_weapons", "balance_patch"});
modSystem.LoadEnabledMods();

modSystem.OnModLoaded([](const std::string& id) {
    std::cout << "Mod loaded: " << id << "\n";
});
```

Mod structure: `mod.json` (metadata) + Scripts/ + Assets/ + Data/ + Preview.png

### VirtualFileSystem — Mount-Priority VFS

```cpp
auto& vfs = VirtualFileSystem::GetInstance();
vfs.Mount("engine", std::make_unique<LocalFileProvider>("Data/Engine"), 0);
vfs.Mount("game", std::make_unique<LocalFileProvider>("Data/Game"), 100);
vfs.Mount("mods", std::make_unique<LocalFileProvider>("Data/Mods"), 300);

auto data = vfs.ReadFile("textures/player.png");  // Returns highest-priority version
```

Priority: `ENGINE (0) < GAME (100) < DLC (200) < MOD (300)`

---

## Replay System

**Location:** `SparkEngine/Source/Engine/Replay/`

Match replay recording and playback for FPS:

```cpp
ReplaySystem& replay = ReplaySystem::GetInstance();
replay.SetRecordInterval(0.05f);  // 20 fps snapshots
replay.StartRecording();

// Per-frame during gameplay
replay.RecordFrame(entities, timestamp);
replay.RecordEvent({timestamp, "PlayerKill", killerId, victimId});

replay.StopRecording();

// Playback
replay.StartPlayback();
replay.SetPlaybackSpeed(0.5f);    // Slow motion
replay.SeekTo(120.0f);            // Jump to 2 minutes
replay.SetCamera(PlaybackCamera::KillCam);
replay.StartKillCam(3.0f, focusEntity);  // Rewind 3s

// Save/Load
replay.SaveToFile("replays/match_001.replay");
replay.LoadFromFile("replays/match_001.replay");
```

Camera modes: `FreeCam`, `FollowCam`, `FirstPerson`, `KillCam`

Entity state flags: `Alive`, `Visible`, `Firing`, `Crouching`, `Sprinting`, `Reloading`, `InVehicle`, `Scoped`

---

## Save System

**Location:** `SparkEngine/Source/Engine/SaveSystem/`

ECS-aware game state persistence with compressed JSON:

```cpp
SaveSystem& ss = SaveSystem::GetInstance();
ss.Initialize("Saves");

SaveMetadata meta;
meta.saveName = "Before Boss";
meta.sceneName = "Level03";
meta.playTime = totalPlaySeconds;

ss.Save("slot1", world, meta);
ss.QuickSave(world, meta);
ss.AutoSave(world, meta);

if (ss.SaveExists("slot1"))
    ss.Load("slot1", world);

auto slots = ss.GetSaveSlots();  // For UI
ss.DeleteSave("slot1");
```

### Custom Component Registration

```cpp
ComponentSerializerRegistry::GetInstance().Register(
    "LootComponent",
    [](const void* comp) -> SerializedComponent {
        const auto* loot = static_cast<const LootComponent*>(comp);
        SerializedComponent sc;
        sc.typeName = "LootComponent";
        sc.properties["itemID"] = std::to_string(loot->itemID);
        return sc;
    },
    [](World& world, EntityID e, const SerializedComponent& d) {
        LootComponent loot;
        loot.itemID = std::stoi(d.properties.at("itemID"));
        world.AddComponent<LootComponent>(e, loot);
    });
```

Built-in serializers for: Transform, NameComponent, HealthComponent, RigidBodyComponent, MeshRenderer, Camera, AudioSourceComponent, LightComponent, AnimationController, AIComponent, etc.

---

## UI System

**Location:** `SparkEngine/Source/Engine/UI/`

Runtime game UI framework (separate from Dear ImGui editor):

```cpp
UISystem ui;
ui.Initialize(1920, 1080);

auto* hud = ui.GetCanvas().CreatePanel("HUD");
hud->SetAnchor(Anchor::TopLeft);

auto* healthLabel = hud->CreateLabel("health_text", "Health: 100");
healthLabel->SetPosition(20, 20);
healthLabel->SetFontSize(24);

auto* healthBar = hud->CreateProgressBar("health_bar");
healthBar->SetPosition(20, 50);
healthBar->SetSize(200, 20);
healthBar->SetValue(0.8f);

auto* btn = hud->CreateButton("fire", "Fire!");
btn->OnClick([]() { Fire(); });

ui.Update(deltaTime);
ui.Render();
ui.HandleClick(mouseX, mouseY);
```

Widgets: `UILabel`, `UIButton`, `UIProgressBar`, `UIImageWidget`, `UIPanel`, `UICanvas`

Anchors: `TopLeft`, `TopCenter`, `TopRight`, `MiddleLeft`, `Center`, `MiddleRight`, `BottomLeft`, `BottomCenter`, `BottomRight`, `Stretch`

### Data Binding (UIFactory)

```cpp
float health = 100.0f;
UIFactory::GetInstance().RegisterBinding("player_health",
    std::make_shared<UIFloatBinding>(&health, [](float h) {
        UpdateHealthBar(h);
    }));
UIFactory::GetInstance().UpdateAllBindings();
```

---

## VR System

**Location:** `SparkEngine/Source/Engine/VR/`

OpenXR-ready framework (stub — requires OpenXR SDK for actual hardware):

```cpp
VRSystem vr;
if (!vr.Initialize()) return;  // VR hardware not available

vr.UpdateTracking();

auto headPos = vr.GetHeadPosition();
auto [leftEye, rightEye] = std::tie(vr.GetLeftEye(), vr.GetRightEye());
auto [width, height] = vr.GetRecommendedRenderSize();

auto& leftCtrl = vr.GetLeftController();
if (leftCtrl.connected)
    float trigger = leftCtrl.triggerValue;

vr.TriggerHaptic(true, 0.8f, 0.1f);  // Left, 80% amplitude, 100ms
vr.SetTrackingSpace(VRTrackingSpace::RoomScale);
```

---

## Coroutine System

**Location:** `SparkEngine/Source/Engine/Coroutine/`

Dual-mode: builder pattern + C++20 native coroutines:

### Builder Pattern

```cpp
CoroutineScheduler::GetInstance().StartCoroutine("damage_flash")
    .Do([&]() { hud.SetDamageFlash(1.0f); })
    .WaitForSeconds(0.2f)
    .Do([&]() { hud.FadeDamageFlash(1.0f); });
```

### C++20 Coroutines

```cpp
GameCoroutine SpawnWaves(int count) {
    for (int i = 0; i < count; ++i) {
        SpawnWave(i);
        co_await WaitForSeconds(10.0f);
    }
}
ScheduleCoroutine("waves", SpawnWaves(5));
```

Yield types: `WaitForSeconds`, `WaitForFrames`, `WaitUntil`, `WaitForEndOfFrame`, `WaitForEvent`

### Cancellation

```cpp
CancellationToken token;
// Token auto-cancels when owning entity is destroyed
```

---

## Cinematic Sequencer

**Location:** `SparkEngine/Source/Engine/Cinematic/`

Timeline-based cutscene system:

```cpp
auto seq = SequencerManager::GetInstance().CreateSequence("intro");
auto camTrack = seq->AddCameraTrack("main_camera");

camTrack->AddKeyframe({0.0f, {0,5,0}, {10,0,10}, 60.0f, 0.0f, CameraInterp::CatmullRom});
camTrack->AddKeyframe({5.0f, {5,3,5}, {10,0,10}, 45.0f, 0.0f, CameraInterp::CatmullRom});

auto audioTrack = seq->AddAudioTrack("music");
audioTrack->AddCue({0.0f, "intro_music", 1.0f});

auto subtitleTrack = seq->AddSubtitleTrack("subs");
subtitleTrack->AddCue({1.0f, "Welcome to the world.", 3.0f});

seq->Play();
seq->Update(deltaTime);
```

Track types: `CameraPathTrack`, `EntityTransformTrack`, `EntityPropertyTrack`, `AudioCueTrack`, `EventTrack`, `SubtitleTrack`, `FadeTrack`

Interpolation: `Step`, `Linear`, `CubicBezier`, `CatmullRom`

---

## 2D Physics

**Location:** `SparkEngine/Source/Engine/2D/`

Self-contained 2D physics for 2D games or hybrid 2D/3D scenes:

```cpp
Physics2DWorld world;
world.SetGravity(0, -9.81f);

PhysicsBody2D body;
body.position = {100, 200};
body.shape = Shape2D::Circle;
body.radius = 10.0f;
body.mass = 1.0f;
size_t idx = world.AddBody(body);

world.Step(deltaTime);

RaycastHit2D hit;
world.Raycast({0, 0}, {1, 0}, 500.0f, hit);
```

Uses spatial hashing for broadphase, SAT for box collision, circle intersection for circles, and impulse-based resolution.

---

## Mobile Platform

**Location:** `SparkEngine/Source/Engine/Mobile/`

iOS/Android framework:

```cpp
MobilePlatform mobile;
mobile.Initialize();
mobile.SetQualityPreset(MobileQualityPreset::High);
mobile.SetBatteryAwareScaling(true);
mobile.SetOrientation(ScreenOrientation::LandscapeLeft);

mobile.OnGesture(GestureType::Swipe, [](const Gesture& g) {
    HandleSwipe(g.deltaX, g.deltaY);
});
```

Quality presets: Low (0.5x render), Medium (0.75x), High (1.0x), Auto (battery-aware)
