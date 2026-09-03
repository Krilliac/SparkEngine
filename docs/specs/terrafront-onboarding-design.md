# TERRAFRONT Onboarding — Login → Character → World Entry — Design

Date: 2026-07-06
Status: Approved (design). Adds the player onboarding pipeline to the TERRAFRONT MMOFPS.

## Context & problem

TERRAFRONT (`GameModules/SparkGameMMOFPS`) drops the player straight into the war — there is **no menu, login, account, character record, name, or flow state machine**. Player identity is the ephemeral network client id (`Core/TFTypes.h:55` `using PlayerId = uint32_t; // == network client id`). Even progression is session-scoped and identity-less (`Game/TFProgressionSystem.cpp:408` self-documents `"PlayerIds are session-scoped in W2"`). On disconnect the server erases everything (`Net/TFServerSim.cpp:513-527`).

A complete reference flow exists in the sibling module `SparkGameMMO` and is the adaptation source (not greenfield): `Account/MMOAccountSystem.h` (register/login, `HashPassword`/`GenerateSalt`, char slots), `Persistence/MMOPersistenceSystem.h` (`CharacterSaveData`, character CRUD over `AsyncDatabasePool`), and `UI/MMOLoginUI.h` (`enum LoginUIState{Login, CharacterSelect, CharacterCreate, EnteringWorld, InGame}` + `EnterWorldCallback(accountId, characterId)`).

## Decisions (approved)

- **Persistence: DB-driven** via the engine's `AsyncDatabasePool` (`SparkEngine/Source/Engine/Persistence/AsyncDatabase.h` — DB-shaped interface; JSON-key-value file backing today, real-SQLite-swappable later), with a character schema adapted from `SparkGameMMO`.
- **Auth: real username + password** with salted hashing (ported from `MMOAccountSystem`).
- **Networking: server-authoritative over net messages** — account/character systems live server-side; the client UI drives them via new `TFMsg` messages over the existing transport, identical for standalone (loopback router, `kTFLocalHostPlayer 0xFFFFFF01`) and networked (socket).
- **Character model: faction + name + persistent progression**; **class is chosen per-spawn** (the existing deploy screen), matching the MMOFPS/Planetside model and TERRAFRONT's free class-switching. **Appearance customization is deferred** (no appearance-render pipeline — the pawn is one faction-tinted model).

## Goals

1. Launching TERRAFRONT presents a **Login screen** (login or register) instead of dropping into the war.
2. After login, a **Character Select** screen lists the account's characters (name/faction/rank), with Create and Delete.
3. **Character Create** lets the player pick a faction + name (validated), creating a persisted character.
4. Selecting a character + Enter World spawns the player into the war **as that character** (its faction is authoritative), and the character's **progression (XP/rank/flux) persists across sessions**.
5. The whole flow is **server-authoritative** and works in standalone (listen-host/loopback) and networked modes via one code path.

## Non-goals

- Appearance/cosmetic customization (deferred — no render support).
- Class locked at character creation (class stays per-spawn).
- Account recovery/email, real SQLite backend (the JSON-KV `AsyncDatabasePool` backing is used; SQLite is a later swap), matchmaking/realm-list across multiple servers.
- Deep security hardening (rate-limiting, lockout beyond a basic bad-password count) — basic salted hashing only.

## Architecture — six units

### Unit 1 — `TFDatabase` (persistence, engine-DB-backed)
`GameModules/SparkGameMMOFPS/Source/Persistence/TFDatabase.{h,cpp}`. Thin wrapper over `Spark::AsyncDatabasePool` exposing synchronous-enough CRUD for the onboarding flow (the flow is request/reply, not hot-path). Two record types:

```cpp
struct TFAccountRecord {
    uint64_t    id;            // stable account id
    std::string username;      // unique, lowercased key
    std::string salt;          // hex
    std::string passwordHash;  // hex (salted)
    int64_t     createdAtMs;
    int64_t     lastLoginMs;
};
struct TFCharacterRecord {
    uint64_t    id;            // stable character id
    uint64_t    accountId;
    std::string name;          // unique across all characters
    FactionId   faction;       // MRA/AUC/HLX (fixed at creation)
    uint32_t    xp;
    uint16_t    rank;
    uint32_t    flux;
    int64_t     createdAtMs;
    int64_t     lastPlayedMs;
};
```
Operations: `CreateAccount`, `FindAccountByUsername`, `TouchLogin`; `CreateCharacter`, `ListCharacters(accountId)`, `FindCharacter(id)`, `DeleteCharacter(id)`, `SaveCharacterProgress(id, xp, rank, flux, lastPlayedMs)`. Uniqueness (username, character name) enforced here. DB path `Saves/terrafront.db` (the JSON-KV file). Ids are monotonic (stored counter in the db).

### Unit 2 — `TFAccountSystem` (server-side)
`Source/Account/TFAccountSystem.{h,cpp}`. `Register(username, password) -> {ok, accountId, error}` (username uniqueness, min length, salt+hash), `Login(username, password) -> {ok, accountId, error}` (verify hash), and a per-connection session map `ClientID -> {accountId, loggedIn}`. Salted hashing ported from `MMOAccountSystem::HashPassword`/`GenerateSalt`. A logged-in session is required before any character op. Basic bad-password counter (log only, no lockout in v1).

### Unit 3 — `TFCharacterSystem` (server-side)
`Source/Account/TFCharacterSystem.{h,cpp}`. `List(accountId)`, `Create(accountId, name, faction) -> {ok, charId, error}` (slot cap `kTFMaxCharSlots=5`, name uniqueness/length, valid faction), `Delete(accountId, charId)` (ownership check), `EnterWorld(clientId, charId) -> {ok, error}` — validates the character belongs to the session's account, binds `session.activeCharacter = charId`, sets that client's authoritative faction from the character, and marks the session "in-world". On XP change (`TF_XPEvent` path), `TFCharacterSystem` persists xp/rank/flux back via `TFDatabase::SaveCharacterProgress` (replacing the session-scoped `TFProgressionSystem` keying). On disconnect, flush the active character's progress.

### Unit 4 — Net protocol + gating
New `TFMsg` ids appended after `WorldWelcome 0x5411` in `Net/TFNetProtocol.h` (packed PODs + `static_assert` sizes, matching the file's convention). Strings (username/password/char name) are fixed-size `char[N]` fields (e.g. 32/64/24) to keep PODs:
- `TF_LoginRequest{char user[32]; char pass[64];}` / `TF_LoginReply{uint8 ok; uint8 errCode; uint64 accountId;}`
- `TF_RegisterRequest{...same...}` / `TF_RegisterReply{ok,errCode,accountId}`
- `TF_CharListRequest{}` / `TF_CharListReply{uint8 count; TF_CharBrief chars[5];}` where `TF_CharBrief{uint64 id; char name[24]; uint8 faction; uint16 rank;}`
- `TF_CharCreateRequest{char name[24]; uint8 faction;}` / `TF_CharCreateReply{ok,errCode,uint64 charId}`
- `TF_CharDeleteRequest{uint64 charId;}` / `TF_CharDeleteReply{ok,errCode,uint64 charId}`
- `TF_EnterWorldRequest{uint64 charId;}` (reply is the existing `TF_WorldWelcome`, now gated)

**Gating change:** today `SendWorldWelcome(id)` fires from `PollClientJoinsLeaves` the instant a socket connects (`TFServerSim.cpp:508`). Move it: on connect the server now sends nothing gameplay-facing; `TF_WorldWelcome` is sent only from the `TF_EnterWorldRequest` handler after `TFCharacterSystem::EnterWorld` succeeds. Client handlers register in `RegisterClientHandlers` (`Net/TFClientNetHandlers.cpp:275`); server handlers in `TFServerSim::RegisterNetHandlers`. All flow over the loopback router unchanged (`TFClientNetHandlers.cpp:159 RouteLoopback`).

### Unit 5 — `TFLoginFlow` (client UI state machine)
`Source/UI/TFLoginFlow.{h,cpp}`. `enum FlowState{Login, Register, CharSelect, CharCreate, EnteringWorld, InWorld}` (ported from `MMOLoginUI`). Full-screen ImGui modals styled like `TFSpawnScreen` (`SetNextWindowPos/Size(vp)`, `NoDecoration|NoBackground`, dimmed backdrop, centered panel, `TFUi` helpers). Screens: Login (user/pass + Login/Register toggle), CharSelect (grid of up to 5 character cards + Create/Delete/Enter), CharCreate (name field + three faction buttons using `TFUi::FactionCol` like `DrawFactionSplash`). It sends the Unit-4 requests, transitions on replies, and on `TF_WorldWelcome` moves to `InWorld` and hands off to the existing spawn path. While not `InWorld`, it owns the mouse (extends the `uiOpen` suppression in `TFClientNet.cpp:120-122`).

### Unit 6 — Wiring, gating, context (the frozen-contract change)
- Extend `TFGameContext` (`Core/TFTypes.h:115`, currently FROZEN): add `TFAccountSystem* account; TFCharacterSystem* characters; TFLoginFlow* loginFlow;` and a `flowState` accessor (or reuse `TFLoginFlow::State()`), plus `bool InWorld()` (true once a character has entered). Update the FROZEN banner + `DESIGN.md` with a **W5 Onboarding** section documenting the contract extension.
- Construct/publish/boot the three systems in `Core/Main.cpp:76-146` (also FROZEN — documented change).
- `OnImGui` (`Main.cpp:243`): render `TFLoginFlow` when `!InWorld()`, and keep HUD/map/spawn/scoreboard gated behind in-world (they already gate on `HasLocalPlayer()`).
- Gate `TFSpawnScreen` auto-open (`UI/TFSpawnScreen.cpp:146`) behind `InWorld()` — the faction splash is now redundant (faction comes from the character); the deploy panel (spawn point + class-per-spawn) remains.
- On enter-world: server sets `localFaction` from the character; the client's existing `TF_FactionSelect`/spawn pipeline proceeds with the character's faction pre-set.
- Re-point `TFProgressionSystem` to persist through `TFCharacterSystem`/`TFDatabase` keyed by character id instead of session PlayerId.

## Data flow (the round-trip)

```
Client TFLoginFlow(Login)  --TF_LoginRequest-->      TFAccountSystem.Login
                           <--TF_LoginReply(ok,acct)--
  --TF_CharListRequest-->  TFCharacterSystem.List --TF_CharListReply(chars)-->  CharSelect
  --TF_CharCreateRequest-> TFCharacterSystem.Create --TF_CharCreateReply(id)->  (refresh list)
  --TF_EnterWorldRequest-> TFCharacterSystem.EnterWorld -> sets faction+active char
                           <--TF_WorldWelcome(gated)-- InWorld -> existing spawn pipeline
  ...gameplay... XP change -> TFCharacterSystem.SaveCharacterProgress -> TFDatabase (persists)
```
The same handlers run over the loopback (standalone) and socket (networked).

## Error handling

- Reply `errCode` enum: `Ok, BadCredentials, UsernameTaken, UsernameTooShort, NameTaken, NameInvalid, SlotsFull, NotLoggedIn, NoSuchCharacter, NotYourCharacter, ServerError`. The UI maps each to a human message.
- Not-logged-in guard on every character op (server rejects with `NotLoggedIn`).
- DB write failures → `ServerError`, logged; the flow stays on its current screen with the error surfaced.
- Duplicate `TF_EnterWorldRequest` / already-in-world → idempotent (server ignores if the session already has an active character in-world).

## Testing

- **Headless unit (CI-safe, no GPU/net)** — `Tests/TestTFOnboarding.cpp` added to `SparkTests`: (a) register→login round-trip with correct + wrong password (hash verify); (b) character create/list/delete + slot cap + name-uniqueness rejection; (c) persistence round-trip — create account+character, save progress, tear down the `TFDatabase`, reload, assert identical account/character/xp. Drive `TFAccountSystem`/`TFCharacterSystem`/`TFDatabase` directly (no rendering).
- **Loopback flow harness** — a `-exec` config (Tools/tf_onboard.cfg) + console commands (`tf_login`, `tf_char_create`, `tf_enter`) that drive the flow over the standalone loopback and assert (via the exec audit log) that login succeeds, a character is created, and a pawn spawns after enter-world.
- **UI validation** — launch TERRAFRONT and screenshot the Login and Character-Select screens (via `Start-Process` detached to avoid the console leak; `AttachConsole(ATTACH_PARENT_PROCESS)` gotcha).

## Risks & mitigations

- **Frozen-contract edits** (`TFTypes.h`, `Main.cpp`): deliberate, documented in `DESIGN.md` W5; additive only (new pointers/fields), existing systems untouched. This is the largest blast-radius change — keep it purely additive.
- **Progression re-keying**: moving progression from session-PlayerId to character-id could disturb the existing `TFProgressionSystem`. Mitigation: `TFCharacterSystem` becomes the authority for persistence; `TFProgressionSystem` keeps its in-session runtime state but flushes through the character. Guard behind a test.
- **Standalone vs networked identity**: in standalone the loopback client is `kTFLocalHostPlayer`. The session map keys on ClientID uniformly; the loopback id is just another key. Verified against the loopback router path.
- **AsyncDatabasePool JSON-KV backing**: not a real relational DB — uniqueness/queries are done in `TFDatabase` over the KV store, not via SQL. Acceptable at onboarding scale; documented for the SQLite swap.
- **Fixed-size string PODs**: usernames/names longer than the field are rejected client-side before send (avoids truncation surprises).

## Sequencing note

This is one coherent feature (the onboarding pipeline) implemented as the six units above, in order 1→6 (persistence → account → character → net → UI → wiring), each independently testable. It becomes TERRAFRONT wave **W5**.
