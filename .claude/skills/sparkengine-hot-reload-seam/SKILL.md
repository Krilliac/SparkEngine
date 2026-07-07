---
name: sparkengine-hot-reload-seam
description: >-
  Reference for how SparkEngine's AngelScript hot-reload and client/server
  script context separation actually work, and a checklist for exposing a new
  scripted type/function so it survives a reload. TRIGGER when the user says
  "script hot-reload", "reload the .as script", "why didn't my AngelScript
  change take effect", "[server]/[client] script tag", "SetScriptContext",
  "AttachScript returns false", "script state got wiped on reload", or "I added
  a new function/type callable from AngelScript". DO NOT TRIGGER for C++ game
  module hot-reload (that is the `ModuleHotReload` C++ class, a different code
  path, not this skill), shader hot-reload (the `ShaderHotReload` C++ class), or
  AngelScript sandbox/security questions (see the sandbox code directly) — those
  are separate seams.
trilobite_compatible: true
trilobite_role: reference
---

# SparkEngine hot-reload seam (AngelScript scripts + client/server contexts)

Runbook for the AngelScript scripting reload path in SparkEngine. Use it to
answer "what happens to my script state when the file reloads", "why is my
`[server]` class not attaching", and "I exposed a new native function/type — what
do I have to do so it keeps working across a reload".

**Jargon defined once:**
- **Module** — one compiled AngelScript translation unit. Named after the `.as`
  file's stem (e.g. `EnemyAI.as` -> module `EnemyAI`). Holds the compiled script
  classes.
- **Script instance** — one live object of a script class, bound to one ECS
  `EntityID`. Holds an `asIScriptObject`, an `asIScriptContext`, and cached
  pointers to `Start()/Update(float)/OnCollision(EntityID)`.
- **Engine (native) API** — the C++ functions/types registered onto the
  AngelScript engine (`print`, `getTransform`, `Vector3`, `Transform`, ...).
  Registered **once** at `Initialize()`, and persists for the engine's lifetime.
- **Script context** — `Shared` / `Server` / `Client`. A multiplayer authority
  tag on a script class *and* a current-mode setting on the engine.

## When NOT to use this skill — pick the right sibling

| You are dealing with… | Go to instead |
|---|---|
| Reloading a compiled **C++ game module DLL** (`GameModules/`) | `SparkEngine/Source/Core/ModuleHotReload.cpp` |
| Reloading **shaders** (HLSL/`.hlsl`) at runtime | `SparkEngine/Source/Graphics/ShaderHotReload.cpp` (+ `ShaderCompilation*.cpp`) |
| AngelScript **sandbox / security levels / whitelists** | `SparkEngine/Source/Engine/Scripting/ScriptSandbox.{h,cpp}` |
| **Visual scripting** graph -> AngelScript codegen | `SparkEngine/Source/Engine/Scripting/VisualScriptCompiler.cpp` |

This skill is scoped to **AngelScript source (`.as`) hot-reload and script
execution contexts only.**

---

## The two halves of AngelScript hot-reload (and the gap between them)

Hot-reload is split across **two independent pieces** that are *not* wired
together in the current tree. Know both, and know that the seam between them is
currently **open** (candidate, not proven end-to-end).

### Half 1 — the file watcher: `ScriptHotReloadManager`
`SparkEngine/Source/Engine/Scripting/ScriptHotReload.{h,cpp}`

- Polls tracked files on the **main thread** via `PollChanges()` (call once per
  frame). It compares `std::filesystem::last_write_time` **and** `file_size`
  against a stored snapshot; a change in either marks the file dirty.
- **Debounces** with a default of **300 ms** (`m_debounceMs`, settable via
  `SetDebounceMs`) so a mid-save editor write doesn't trigger a half-written
  recompile. The debounce timer resets each time the file changes again.
- Watches extensions **`.as`** and **`.angelscript`** by default
  (`SetWatchExtensions` to override).
- It does **not** compile anything itself. When a debounced change fires, it
  calls whatever you handed to `SetRecompileCallback(...)`, which must return a
  `RecompileResult`. Errors are pushed to `SetErrorCallback` and a bounded
  ring of the last 10 errors (`MaxRecentErrors`).
- Console: the `script_hotreload_status` command
  (`SubsystemConsoleCommandsExt.cpp`) prints `Console_GetStatus()` if a
  `ScriptHotReloadManager` is registered as an engine system.

### Half 2 — the actual reload: `AngelScriptEngine::HotReloadModule`
`SparkEngine/Source/Engine/Scripting/AngelScriptEngine.cpp` (private method, ~line 585)

Given a module name, it does a **compile-first, swap-second** dance:

1. Collect every entity script whose `moduleName` matches (entity + className).
2. **Pre-validate**: compile the new source into a throwaway staging module
   named `"<module>$hotreload_stage"`, then `Discard()` it. This is a compile
   *probe only*.
3. If the probe **fails**, abort: sets last-error, returns `false`, and **leaves
   every live script untouched**. Nothing is detached. This is the key safety
   property — a syntax typo during editing never wipes running scripts.
4. If the probe **succeeds**: `DetachScript` each collected entity, recompile the
   file under its canonical module name via `CompileScriptFile`, then
   `AttachScript` each entity again with the same `className`.

### The open seam (state this honestly)

There is **no production call site** connecting Half 1's `SetRecompileCallback`
to Half 2's `HotReloadModule`, and no production code registers a
`ScriptHotReloadManager` or calls `HotReloadModule` outside its own class. The
only production reference is the read-only `script_hotreload_status` console
command. `HotReloadModule`'s behavior is exercised by a **mock reimplementation**
in `Tests/TestAngelScriptEngine.cpp` (that file is a standalone mock — header
comment: "Standalone reimplementation for CI testing without the AngelScript
SDK"), not by the real class.

> Status: **open / candidate.** Both halves exist and work in isolation; the
> automatic file-watch -> recompile -> re-attach pipeline is **not currently
> wired**. If you need it live, wire the watcher's recompile callback to call
> `HotReloadModule(moduleStem)` and register the manager as an engine system.
> Do not tell the user "just save the file and it reloads" — verify the wiring
> first with the grep in Provenance.

---

## What state is preserved vs reset across a reload

The header is explicit (`AngelScriptEngine.h`, `HotReloadModule` `@note`):
**per-instance script state is NOT preserved. Constructors re-run and all fields
reset. There are no `Serialize()`/`Deserialize()` hooks.**

| Thing | Across `HotReloadModule` |
|---|---|
| Script object fields / member variables | **RESET** — the default factory `ClassName @ClassName()` re-runs from scratch |
| `asIScriptObject` + `asIScriptContext` | **Recreated** (old ones `Release()`d in `CleanupScriptInstance`) |
| Cached `Start()/Update(float)/OnCollision(EntityID)` pointers | **Rebuilt** by `CacheScriptMethods` |
| Entity -> script binding (same `EntityID`, same `className`) | **Re-established** (detach then re-attach) |
| `Start()` "already ran" flag | **Lost** — after reload `CallStart` will run `Start()` again if you call it |
| Module -> file path map (`m_moduleFilePaths`) | **Survives** (re-set by `CompileScriptFile`) |
| Class context tags (`m_classContexts`) | **Refreshed** — `RecordModuleContexts` re-reads tags; a class that *dropped* its `[server]`/`[client]` tag reverts to `Shared` |
| Native/engine API (functions, `Vector3`, `Transform`) | **Untouched** — registered once at `Initialize()`; a module reload never re-registers them |

**Gotchas:**
- **No state migration.** If a script accumulates runtime state (health, timers,
  AI blackboard) in member fields, that state is gone after a reload. Persist it
  in the C++/ECS side if it must survive.
- **Re-attach requires an exact-signature default factory.** `AttachScript`
  looks up `"<Class> @<Class>()"`. If your edit renamed the class or removed the
  default constructor, re-attach fails and the entity ends up scriptless.
- **Method lookups are exact-decl.** `CacheScriptMethods` uses
  `GetMethodByDecl("void Start()")`, `"void Update(float)"`,
  `"void OnCollision(EntityID)"`. A signature that drifts (e.g.
  `Update(double)`) silently won't be cached and won't be called.
- **Constructors run under the sandbox.** The factory call installs the sandbox
  line callback, so a runaway/expensive constructor can be aborted
  (`asEXECUTION_ABORTED` + `WasTerminated()`).

---

## Client/server script context separation

### The model
- `enum class AngelScriptEngine::ScriptContext : uint8_t { Shared, Server, Client }`.
- The **engine** has a current context `m_scriptContext` (default `Shared`),
  set with `SetScriptContext(...)`.
- Each **script class** is tagged in `.as` source with a metadata annotation
  written **immediately before** the class declaration:
  ```angelscript
  [server] class SpawnController { /* ... */ }
  [client] class Hud            { /* ... */ }
  [shared] class Pickup         { /* ... */ }   // or no tag at all -> Shared
  ```
  Tags are matched **case-insensitively** (`RecordModuleContexts`). Untagged
  classes are `Shared`.
- After a module builds, `RecordModuleContexts` reads
  `builder.GetMetadataForType(...)` and stores the result keyed as
  `"module::class"` in `m_classContexts`. `Shared` results are *erased* from the
  map (Shared is the implicit default).

### The enforcement rule (READ THIS — real behavior differs from the naive reading)
`AttachScript` calls `IsClassContextAllowed(classContext)`, which is exactly:

```cpp
if (classContext == ScriptContext::Shared) return true;   // Shared/untagged: always
return classContext == m_scriptContext;                    // tagged: must match engine mode
```

So a **tagged** class attaches **only when the engine's current context is set to
that same tag**. Consequences:

| Engine `m_scriptContext` | `[shared]`/untagged class | `[server]` class | `[client]` class |
|---|---|---|---|
| `Shared` (**default**) | attaches | **REFUSED** | **REFUSED** |
| `Server` | attaches | attaches | REFUSED |
| `Client` | attaches | REFUSED | attaches |

> Note the top-left/middle: under the **default** `Shared` engine context, a
> `[server]` or `[client]` class is **refused**, because `Server != Shared`. The
> mock in `Tests/TestAngelScriptEngine.cpp` has a looser "current==Shared runs
> everything" rule (`ShouldRunScript`) — **the real engine does not.** Do not
> rely on the mock's behavior.

When refused, `AttachScript` returns `false` and logs an info-level message like
`"Skipped attaching 'X': its [server] context conflicts with the current script
execution context."` — it is **not** an error, so it's easy to miss.

### Why it exists / what breaks if you get it wrong
This is the multiplayer authority boundary: it keeps client-only logic (HUD,
input feel, cosmetics) off the dedicated server, and server-only logic
(spawning, authoritative damage, loot rolls) off clients. Failure modes:
- **Forgot to call `SetScriptContext`.** Engine stays `Shared`, so **every**
  `[server]`/`[client]` class silently fails to attach — the game runs with only
  untagged scripts. `SetScriptContext(...)` is currently **not called anywhere in
  production** (verify with the grep in Provenance), so this is the likely
  default-state footgun.
- **Set the wrong mode.** A dedicated server left in `Client` mode would refuse
  its `[server]` authority scripts; a client in `Server` mode could run
  server-authoritative logic locally (cheating / desync surface).

---

## Checklist: exposing a new AngelScript-callable type or function

All native registration lives in `AngelScriptEngine.cpp` inside the
`#ifdef SPARK_ANGELSCRIPT_SUPPORT` block and runs **once** from `Initialize()`
via `RegisterEngineAPI()` (order: `RegisterMathTypes` -> `RegisterComponentTypes`
-> `RegisterGlobalFunctions` -> `AutoRegisterReflectedTypes`). Because it runs at
init and the engine is never torn down by a module reload, **anything you
register here automatically survives hot-reload** — the reload only rebuilds the
`.as` module, not the native API.

### Adding a global function callable from scripts
1. Write the native C++ function (convention: `ASYourFn`) and declare its
   prototype in `AngelScriptEngine.h` (see the `ASPrint`, `ASGetTransform`,
   ... block near the bottom).
2. Register it in `RegisterGlobalFunctions()` (or `AutoRegisterReflectedTypes()`
   for reflection helpers) via the guarded helper — **do not** call
   `m_engine->RegisterGlobalFunction` directly:
   ```cpp
   RegisterGuardedFunction("float getStamina(EntityID)", "getStamina",
                           asFUNCTION(ASGetStamina));
   ```
3. The **second argument (`scriptVisibleName`) must exactly match** the function
   name in the declaration string. It is the key the sandbox uses for
   whitelist/blacklist lookups (`m_sandbox->IsFunctionAllowed`).
4. **Sandbox gotcha:** in **Strict** security mode only whitelisted names are
   registered. If your function isn't on the allow list it is silently *not
   bound*, and a script calling it fails at `BuildModule()` with an
   undefined-symbol compile error. Add it via `ConfigureSandboxSecurity(level,
   {"getStamina", ...})` **before** `Initialize()` (the sandbox is consulted once,
   at registration time; configuring after `Initialize()` cannot retroactively
   register or unregister anything).

### Adding a value type (POD, like `Vector3`)
In `RegisterMathTypes()`:
```cpp
m_engine->RegisterObjectType("MyVec", sizeof(MyVec),
    asOBJ_VALUE | asOBJ_POD | asGetTypeTraits<MyVec>());
m_engine->RegisterObjectProperty("MyVec", "float x", asOFFSET(MyVec, x));
```

### Adding a reference type (engine-owned, like `Transform`)
In `RegisterComponentTypes()`:
```cpp
m_engine->RegisterObjectType("MyComp", 0, asOBJ_REF | asOBJ_NOCOUNT);
m_engine->RegisterObjectProperty("MyComp", "Vector3 value", asOFFSET(MyComp, value));
```
`asOBJ_NOCOUNT` means AngelScript won't refcount it — only expose engine-owned
objects returned by a native accessor (e.g. `getTransform`) this way.

### Prefer reflection over hand-registration for components
`getComponentField` / `setComponentField` / `hasComponent` are **generic and
reflection-driven** (`AutoRegisterReflectedTypes`, backed by `ComponentFactory`
and `TypeRegistry`). If your new component type is already reflected, scripts can
read/write **any** of its fields by name **without touching AngelScriptEngine at
all**. Only add a hand-written binding when you need a typed, ergonomic accessor.

### Make sure it survives a reload (script-side checklist)
Since the native side persists, the only reload risks are in the `.as` file:
- [ ] Class keeps a **default factory** `ClassName @ClassName()` (no
      argument-taking-only constructor).
- [ ] Lifecycle methods keep **exact** signatures: `void Start()`,
      `void Update(float)`, `void OnCollision(EntityID)`.
- [ ] Class **name is unchanged** (re-attach uses the stored `className`).
- [ ] Any `[server]`/`[client]` tag is intentional and the engine is in the
      matching `SetScriptContext(...)` mode where the script must attach.
- [ ] No reliance on member state persisting across reload (it won't).

---

## Fast reference — key symbols and files

| Symbol / file | Role |
|---|---|
| `SparkEngine/Source/Engine/Scripting/ScriptHotReload.{h,cpp}` | File watcher (`PollChanges`, debounce, callbacks) |
| `AngelScriptEngine::HotReloadModule` (`AngelScriptEngine.cpp:~585`) | Stage-compile, detach, recompile, re-attach |
| `AngelScriptEngine::AttachScript` (`~800`) | Instantiates a class onto an entity; enforces context |
| `AngelScriptEngine::RecordModuleContexts` (`~740`) | Reads `[server]/[client]/[shared]` metadata after build |
| `AngelScriptEngine::IsClassContextAllowed` (`~421`) | The Shared-always / tag-must-match-engine rule |
| `AngelScriptEngine::RegisterGuardedFunction` (`~1096`) | Sandbox-gated global-function registration |
| `AngelScriptEngine::CacheScriptMethods` (`~1251`) | Exact-decl lookup of Start/Update/OnCollision |
| `AngelScriptEngine::SetScriptContext / GetScriptContext` (`.h`) | Set/read engine's Shared/Server/Client mode |

---

## Provenance and maintenance

Verified against the repo on **2026-07-07** (branch `Working`) by reading the
source; all line numbers are approximate anchors, not exact addresses.

Re-verify with these one-liners (run from `D:\SparkEngine`):

```bash
# Reload logic (staging compile, detach, re-attach) still lives here:
grep -n "hotreload_stage\|DetachScript(binding\|AttachScript(binding" \
  SparkEngine/Source/Engine/Scripting/AngelScriptEngine.cpp

# The real context-enforcement rule (Shared-always / tag==engine):
grep -n "IsClassContextAllowed\|classContext == m_scriptContext" \
  SparkEngine/Source/Engine/Scripting/AngelScriptEngine.cpp

# Metadata tags recognised for [server]/[client]/[shared]:
grep -n "\"server\"\|\"client\"\|\"shared\"" \
  SparkEngine/Source/Engine/Scripting/AngelScriptEngine.cpp

# CONFIRM the open seam is still open (expect: only a console status ref;
# NO production SetRecompileCallback->HotReloadModule wiring, NO SetScriptContext call):
grep -rn "HotReloadModule\|SetRecompileCallback\|SetScriptContext" \
  --include=*.cpp SparkEngine/Source SparkEditor/Source SparkConsole/src GameModules \
  | grep -v "AngelScriptEngine.cpp:"

# Default watch extensions + debounce:
grep -n "\.angelscript\|m_debounceMs" \
  SparkEngine/Source/Engine/Scripting/ScriptHotReload.h

# Native API registration entry point + guarded helper:
grep -n "RegisterEngineAPI\|RegisterGuardedFunction\|RegisterObjectType" \
  SparkEngine/Source/Engine/Scripting/AngelScriptEngine.cpp
```

If the last grep starts returning a production call that hands the watcher's
recompile callback to `HotReloadModule` (or calls `SetScriptContext` at
startup), update the "open seam" and "forgot to call SetScriptContext" notes —
the pipeline would then be wired and those caveats retired.
