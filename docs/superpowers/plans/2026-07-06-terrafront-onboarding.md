# TERRAFRONT W5 Onboarding — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a login → character-create → character-select → enter-world onboarding pipeline to the TERRAFRONT MMOFPS, replacing the drop-straight-into-the-war start.

**Architecture:** Server-authoritative `TFAccountSystem` + `TFCharacterSystem` over a `TFDatabase` (DB-driven persistence via the engine's `AsyncDatabasePool`, JSON-KV backed). A client `TFLoginFlow` ImGui state machine drives them via new `TFMsg` messages over the existing transport (loopback for standalone, socket for networked); `TF_WorldWelcome` is gated behind enter-world. Core logic (DB + account + character) is separated from net/context integration so it unit-tests headlessly.

**Tech Stack:** C++23, MSVC, Dear ImGui, the engine `Spark::Persistence::AsyncDatabasePool`, the header-only `Tests/TestFramework.h`.

## Global Constraints

- Platform Windows/MSVC. Build via `powershell -File "C:\Users\natew\.claude\scripts\build.ps1" "<cmd>"`. Game target: `$env:TF_TARGET='SparkGameMMOFPS'` then the module build; tests target `SparkTests` (exe at **`build/bin/SparkTests.exe`**, NOT build/windows-release); game exe at `build/windows-release/bin/SparkEngine.exe`. build.ps1 may FALSE-fail — verify by exe timestamp + real `error C`/`LNK` lines.
- Namespace `Terrafront` for all module code; engine persistence is `Spark::Persistence`. Message structs live inside `#pragma pack(push,1)` with a frozen `static_assert(sizeof(...)==N)`.
- **FROZEN files** — `Core/TFTypes.h` (the `TFGameContext` contract) and `Core/Main.cpp` (boot) carry FROZEN banners; edits are **additive only** and require a `DESIGN.md` W5 update (Task 6). Do not reorder or remove existing fields/systems.
- Character model: **faction + name + persistent progression**; class stays per-spawn. No appearance customization.
- Auth hashing is ported from `MMOAccountSystem` (salted `std::hash`, demo-grade — matches the reference; not cryptographic).
- Persistence: `AsyncDatabasePool` (`Engine/Persistence/AsyncDatabase.h`). Its `SQLiteConnection` is a JSON-key-value file fallback — **Task 1's round-trip test is the truth-check**; if SQL-over-KV does not actually persist, Task 1 swaps `TFDatabase`'s internal backing to the atomic JSON-file pattern from `TFProgressionSystem.cpp:385-446` behind the SAME `TFDatabase` interface (Tasks 2-7 unaffected).
- SparkTests does NOT compile module `.cpp` by default — new tested module sources (`TFDatabase.cpp`, `TFAccountSystem.cpp`, `TFCharacterSystem.cpp`) are added to the `add_executable(SparkTests …)` list (`Tests/CMakeLists.txt`), and MUST stay minimal-dependency (no full-module includes) so they link standalone.
- Launch any game/editor exe via `Start-Process -WindowStyle Hidden -RedirectStandardOutput/Error` (the engine's `AttachConsole(ATTACH_PARENT_PROCESS)` leaks into the terminal otherwise). No-`-game` runs auto-load all `*Game*.dll` and crash — irrelevant here (we run WITH the game module).
- Commit after each task. Trailer: `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`. Branch `claude/terrafront-buildout`.

## File / responsibility map

| File | Responsibility | Task |
|---|---|---|
| `GameModules/SparkGameMMOFPS/Source/Persistence/TFDatabase.{h,cpp}` | Account/character records + CRUD + uniqueness + ids (DB-backed) | 1 |
| `GameModules/SparkGameMMOFPS/Source/Account/TFAccountSystem.{h,cpp}` | Register/login, salted hashing, per-connection sessions | 2,4 |
| `GameModules/SparkGameMMOFPS/Source/Account/TFCharacterSystem.{h,cpp}` | List/create/delete/enter-world, slot cap, progression persist | 3,4 |
| `GameModules/SparkGameMMOFPS/Source/Net/TFNetProtocol.h` | New TFMsg ids + PODs | 4 |
| `GameModules/SparkGameMMOFPS/Source/Net/TFServerSim.*`, `TFClientNet*.*` | Handlers + WorldWelcome gating | 4 |
| `GameModules/SparkGameMMOFPS/Source/UI/TFLoginFlow.{h,cpp}` | Client ImGui state machine + screens | 5 |
| `Core/TFTypes.h`, `Core/SparkGameMMOFPS.h`, `Core/Main.cpp`, `DESIGN.md` | Additive wiring + W5 doc | 6 |
| `Tests/TestTFOnboarding.cpp`, `Tests/CMakeLists.txt`, `Console/TFCommands.cpp`, `Tools/tf_onboard.cfg` | Tests + acceptance harness | 1-3,7 |

---

## Task 1: TFDatabase (records + CRUD, DB-backed, round-trip tested)

**Files:**
- Create: `GameModules/SparkGameMMOFPS/Source/Persistence/TFDatabase.h/.cpp`
- Create: `Tests/TestTFOnboarding.cpp`
- Modify: `Tests/CMakeLists.txt` (add both `.cpp` to the SparkTests list)

**Interfaces produced (Tasks 2-6 depend on these EXACT signatures):**
```cpp
// TFDatabase.h — namespace Terrafront
#include "Core/TFTypes.h"   // FactionId
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
namespace Terrafront {
struct TFAccountRecord   { uint64_t id=0; std::string username, salt, passwordHash; int64_t createdAtMs=0, lastLoginMs=0; };
struct TFCharacterRecord { uint64_t id=0, accountId=0; std::string name; FactionId faction=FactionId::None;
                           uint32_t xp=0; uint16_t rank=1; uint32_t flux=0; int64_t createdAtMs=0, lastPlayedMs=0; };
class TFDatabase {
 public:
  bool Open(const std::string& path);   // e.g. "Saves/terrafront.db"; false on failure
  void Close();
  // Accounts
  bool CreateAccount(const std::string& username, const std::string& salt, const std::string& hash, TFAccountRecord& out); // false if username taken
  bool FindAccountByUsername(const std::string& username, TFAccountRecord& out); // false if none
  void TouchLogin(uint64_t accountId, int64_t nowMs);
  // Characters
  bool CreateCharacter(uint64_t accountId, const std::string& name, FactionId faction, TFCharacterRecord& out); // false if name taken
  bool FindCharacterByName(const std::string& name, TFCharacterRecord& out);
  std::vector<TFCharacterRecord> ListCharacters(uint64_t accountId);
  bool FindCharacter(uint64_t charId, TFCharacterRecord& out);
  bool DeleteCharacter(uint64_t charId);
  void SaveCharacterProgress(uint64_t charId, uint32_t xp, uint16_t rank, uint32_t flux, int64_t lastPlayedMs);
 private:
  std::unique_ptr<Spark::Persistence::AsyncDatabasePool> m_db;
  uint64_t m_nextAccountId=1, m_nextCharId=1;
};
} // namespace Terrafront
```

- [ ] **Step 1: Write the failing round-trip test** in `Tests/TestTFOnboarding.cpp`:
```cpp
#include "TestFramework.h"
#include "Persistence/TFDatabase.h"
#include <filesystem>
using namespace Terrafront;
TEST(TFDatabase_AccountCharacter_RoundTrip)
{
    namespace fs = std::filesystem;
    const std::string path = "Saves/test_tfdb_roundtrip.db";
    fs::remove(path);
    uint64_t acctId=0, charId=0;
    {
        TFDatabase db; EXPECT_TRUE(db.Open(path));
        TFAccountRecord a;
        EXPECT_TRUE(db.CreateAccount("commander", "abc123", "hashed", a));
        acctId = a.id; EXPECT_TRUE(acctId != 0);
        EXPECT_FALSE(db.CreateAccount("commander", "x", "y", a)); // username taken
        TFCharacterRecord c;
        EXPECT_TRUE(db.CreateCharacter(acctId, "Vanguard", FactionId::MRA, c));
        charId = c.id; EXPECT_TRUE(charId != 0);
        db.SaveCharacterProgress(charId, 4200, 7, 150, 111);
        db.Close();
    }
    { // reopen a fresh instance -> data survived
        TFDatabase db; EXPECT_TRUE(db.Open(path));
        TFAccountRecord a; EXPECT_TRUE(db.FindAccountByUsername("commander", a));
        EXPECT_EQ(a.id, acctId); EXPECT_TRUE(a.passwordHash == "hashed"); EXPECT_TRUE(a.salt == "abc123");
        auto chars = db.ListCharacters(acctId);
        EXPECT_EQ(chars.size(), (size_t)1);
        TFCharacterRecord c; EXPECT_TRUE(db.FindCharacter(charId, c));
        EXPECT_TRUE(c.name == "Vanguard"); EXPECT_TRUE(c.faction == FactionId::MRA);
        EXPECT_EQ((int)c.xp, 4200); EXPECT_EQ((int)c.rank, 7); EXPECT_EQ((int)c.flux, 150);
        db.Close();
    }
    fs::remove(path);
}
```
- [ ] **Step 2: Add to SparkTests.** In `Tests/CMakeLists.txt`, add `TestTFOnboarding.cpp` AND `../GameModules/SparkGameMMOFPS/Source/Persistence/TFDatabase.cpp` to the `add_executable(SparkTests …)` list (mirroring how `GameMode.cpp` was added ~line 515). The include dir `GameModules/SparkGameMMOFPS/Source` is already present (~line 560).
- [ ] **Step 3: Implement `TFDatabase`.** First attempt: `AsyncDatabasePool` with prepared SQL — `Open` does `m_db = std::make_unique<Spark::Persistence::AsyncDatabasePool>(); m_db->Open(path,1);` then `ExecuteRaw`/`PrepareStatement` a schema (accounts, characters tables) and CRUD statements; use `SyncQuery(id, params)` and read `result.rows[i].GetInt/GetString(col)`. Ids: keep monotonic counters persisted as their own rows/keys (`m_nextAccountId/m_nextCharId`, loaded on Open). Build params as `std::vector<Spark::Persistence::PreparedStatementParam>{ {Spark::Persistence::QueryValue{(int64_t)id}}, {Spark::Persistence::QueryValue{std::string{username}}} }`.
- [ ] **Step 4: Build + run; if the DB round-trip FAILS, pivot the backing (documented).** Build: `powershell -File "C:\Users\natew\.claude\scripts\build.ps1" "cmake --build build --config Release --target SparkTests"`; run `build\bin\SparkTests.exe TFDatabase_AccountCharacter_RoundTrip`. If it FAILS because the JSON-KV `SQLiteConnection` does not actually execute the SQL (rows come back empty / persistence lost), **re-implement `TFDatabase` internals with the atomic-JSON-file pattern** (copy the tmp+rename + `Spark::Json` read-modify-write from `TFProgressionSystem.cpp:385-446`; store accounts[] + characters[] arrays in `Saves/terrafront.db` as JSON) — keep the public interface byte-for-byte identical. Re-run: the SAME test must PASS. Do NOT weaken the assertions. Also run the full `build\bin\SparkTests.exe` → 0 failed.
- [ ] **Step 5: Commit**
```bash
git add GameModules/SparkGameMMOFPS/Source/Persistence/TFDatabase.h GameModules/SparkGameMMOFPS/Source/Persistence/TFDatabase.cpp Tests/TestTFOnboarding.cpp Tests/CMakeLists.txt
git commit -m "feat(terrafront): TFDatabase account/character persistence + round-trip test"
```

---

## Task 2: TFAccountSystem (register/login core logic)

**Files:**
- Create: `GameModules/SparkGameMMOFPS/Source/Account/TFAccountSystem.h/.cpp`
- Modify: `Tests/TestTFOnboarding.cpp`; `Tests/CMakeLists.txt` (add `TFAccountSystem.cpp`)

**Interfaces:**
- Consumes: `TFDatabase` (Task 1).
- Produces:
```cpp
// TFAccountSystem.h — namespace Terrafront
enum class TFAuthErr : uint8_t { Ok=0, BadCredentials, UsernameTaken, UsernameTooShort, ServerError, NotLoggedIn };
struct TFAuthResult { bool ok=false; TFAuthErr err=TFAuthErr::ServerError; uint64_t accountId=0; };
class TFAccountSystem {
 public:
  void SetDatabase(TFDatabase* db) { m_db = db; }            // core logic uses the db directly (unit-testable)
  TFAuthResult Register(const std::string& username, const std::string& password); // min length 3
  TFAuthResult Login(const std::string& username, const std::string& password);
  // session layer (Task 4): bind a connection to an account
  void BindSession(uint32_t clientId, uint64_t accountId);   // PlayerId==clientId
  uint64_t AccountForClient(uint32_t clientId) const;        // 0 if not logged in
  void ClearSession(uint32_t clientId);
  static std::string GenerateSalt();
  static std::string HashPassword(const std::string& password, const std::string& salt);
 private:
  TFDatabase* m_db=nullptr;
  std::unordered_map<uint32_t,uint64_t> m_sessions; // clientId -> accountId
};
```

- [ ] **Step 1: Failing test** (append to `TestTFOnboarding.cpp`):
```cpp
#include "Account/TFAccountSystem.h"
TEST(TFAccountSystem_Register_Login_HashVerify)
{
    namespace fs = std::filesystem; const std::string path="Saves/test_tfacct.db"; fs::remove(path);
    TFDatabase db; EXPECT_TRUE(db.Open(path));
    TFAccountSystem acct; acct.SetDatabase(&db);
    auto r = acct.Register("commander", "sekret1");
    EXPECT_TRUE(r.ok); EXPECT_TRUE(r.accountId != 0);
    EXPECT_FALSE(acct.Register("commander", "other").ok);          // taken
    EXPECT_TRUE(acct.Register("x", "pw").err == TFAuthErr::UsernameTooShort);
    EXPECT_TRUE(acct.Login("commander","sekret1").ok);             // correct
    auto bad = acct.Login("commander","wrong");
    EXPECT_FALSE(bad.ok); EXPECT_TRUE(bad.err == TFAuthErr::BadCredentials);
    EXPECT_TRUE(acct.Login("ghost","x").err == TFAuthErr::BadCredentials); // no such user
    db.Close(); fs::remove(path);
}
```
- [ ] **Step 2:** Add `TFAccountSystem.cpp` to the SparkTests list. Run to confirm it fails to compile/link (not implemented).
- [ ] **Step 3: Implement.** Port `GenerateSalt`/`HashPassword` verbatim from `MMOAccountSystem.cpp:61-81` (salt = hex of `mt19937_64`; hash = hex of `std::hash` over `salt+password+salt` xor-mixed). `Register`: validate `username.size()>=3` (`UsernameTooShort`), `db.FindAccountByUsername` → `UsernameTaken`; else salt+hash, `db.CreateAccount`, return `{ok, Ok, rec.id}`. `Login`: `FindAccountByUsername`; if none or `HashPassword(pw, rec.salt) != rec.passwordHash` → `BadCredentials`; else `TouchLogin`, `{ok, Ok, rec.id}`. Session map is a plain `unordered_map`.
- [ ] **Step 4:** Build SparkTests, run `build\bin\SparkTests.exe TFAccountSystem_Register_Login_HashVerify` → PASS; full suite 0 failed.
- [ ] **Step 5: Commit** (`git add` the two Account files + test + CMake) — `feat(terrafront): TFAccountSystem register/login with salted hashing`.

---

## Task 3: TFCharacterSystem (character CRUD + enter-world core logic)

**Files:**
- Create: `GameModules/SparkGameMMOFPS/Source/Account/TFCharacterSystem.h/.cpp`
- Modify: `Tests/TestTFOnboarding.cpp`; `Tests/CMakeLists.txt` (add `TFCharacterSystem.cpp`)

**Interfaces:**
- Consumes: `TFDatabase` (Task 1), `FactionId`.
- Produces:
```cpp
// TFCharacterSystem.h — namespace Terrafront
constexpr int kTFMaxCharSlots = 5;
enum class TFCharErr : uint8_t { Ok=0, SlotsFull, NameTaken, NameInvalid, NoSuchCharacter, NotYourCharacter, ServerError };
struct TFCharCreateResult { bool ok=false; TFCharErr err=TFCharErr::ServerError; uint64_t charId=0; };
class TFCharacterSystem {
 public:
  void SetDatabase(TFDatabase* db) { m_db=db; }
  std::vector<TFCharacterRecord> List(uint64_t accountId);
  TFCharCreateResult Create(uint64_t accountId, const std::string& name, FactionId faction); // name 3..23, unique, valid faction, slot cap
  TFCharErr Delete(uint64_t accountId, uint64_t charId);          // ownership-checked
  // enter-world binding (Task 4): returns the character (faction becomes authoritative)
  bool EnterWorld(uint64_t accountId, uint64_t charId, TFCharacterRecord& out);
  void PersistProgress(uint64_t charId, uint32_t xp, uint16_t rank, uint32_t flux); // routes to db
 private:
  TFDatabase* m_db=nullptr;
};
```

- [ ] **Step 1: Failing test** (append): create account, then create 5 characters OK + the 6th returns `SlotsFull`; duplicate name returns `NameTaken`; a 2-char name returns `NameInvalid`; `List` returns 5; `Delete` a foreign account's char returns `NotYourCharacter`; `Delete` own char then `List`==4; `EnterWorld` returns the record with the right faction.
```cpp
#include "Account/TFCharacterSystem.h"
TEST(TFCharacterSystem_CRUD_SlotCap_EnterWorld)
{
    namespace fs=std::filesystem; const std::string path="Saves/test_tfchar.db"; fs::remove(path);
    TFDatabase db; EXPECT_TRUE(db.Open(path));
    TFAccountRecord a; db.CreateAccount("cmd","s","h",a);
    TFCharacterSystem cs; cs.SetDatabase(&db);
    for (int i=0;i<kTFMaxCharSlots;++i) EXPECT_TRUE(cs.Create(a.id, std::string("Char")+char('A'+i), FactionId::AUC).ok);
    EXPECT_TRUE(cs.Create(a.id, "OneMore", FactionId::AUC).err == TFCharErr::SlotsFull);
    EXPECT_TRUE(cs.Create(a.id, "CharA", FactionId::AUC).err == TFCharErr::NameTaken); // (delete one first if slots full blocks — see note)
    auto list = cs.List(a.id); EXPECT_EQ(list.size(), (size_t)kTFMaxCharSlots);
    EXPECT_TRUE(cs.Delete(a.id+999, list[0].id) == TFCharErr::NotYourCharacter);
    EXPECT_TRUE(cs.Delete(a.id, list[0].id) == TFCharErr::Ok);
    EXPECT_EQ(cs.List(a.id).size(), (size_t)(kTFMaxCharSlots-1));
    TFCharacterRecord ew; EXPECT_TRUE(cs.EnterWorld(a.id, list[1].id, ew));
    EXPECT_TRUE(ew.faction == FactionId::AUC);
    db.Close(); fs::remove(path);
}
```
(Note: order the slot-cap vs name-taken checks in `Create` so the test's expectations hold — check name-taken BEFORE slot-cap only if a slot is free; simplest: the 6th create with a NEW name hits SlotsFull, and a duplicate name at capacity — pick whichever your `Create` checks first and make the test match your implementation's precedence. Keep both an `EXPECT` for SlotsFull and one for NameTaken by freeing a slot before the NameTaken probe.)
- [ ] **Step 2:** Add `TFCharacterSystem.cpp` to SparkTests; confirm fail.
- [ ] **Step 3: Implement.** `Create`: validate name length 3..23 + valid faction (`NameInvalid`); `db.FindCharacterByName` → `NameTaken`; `db.ListCharacters(accountId).size() >= kTFMaxCharSlots` → `SlotsFull`; else `db.CreateCharacter`. `Delete`: `db.FindCharacter`; if none `NoSuchCharacter`; if `rec.accountId != accountId` `NotYourCharacter`; else `db.DeleteCharacter`. `EnterWorld`: `FindCharacter` + ownership check → out. `PersistProgress` → `db.SaveCharacterProgress`.
- [ ] **Step 4:** Build + run `TFCharacterSystem_CRUD_SlotCap_EnterWorld` → PASS; full suite 0 failed.
- [ ] **Step 5: Commit** — `feat(terrafront): TFCharacterSystem CRUD + slot cap + enter-world`.

---

## Task 4: Net protocol + session integration + WorldWelcome gating

**Files:**
- Modify: `Net/TFNetProtocol.h` (new ids + PODs), `Net/TFServerSim.h/.cpp`, `Net/TFClientNet.h`, `Net/TFClientNetHandlers.cpp`
- Modify: `Tests/TestTFOnboarding.cpp` (a wire-layout `static_assert` mirror test)

**Interfaces:**
- Consumes: `TFAccountSystem`, `TFCharacterSystem` (Tasks 2-3) — reached via `m_ctx->account` / `m_ctx->characters` (added in Task 6; for Task 4, forward-declare + call through the context pointers, which Task 6 wires — Task 4 compiles because the pointers are added to `TFGameContext` in Task 6; do Task 6's context-field addition as the FIRST step of Task 4 if needed to compile, or gate the new handlers behind `if (m_ctx->account)`).
- Produces: the new `TFMsg` ids + PODs (Task 5 client sends them).

- [ ] **Step 1: Add message ids + PODs** to `Net/TFNetProtocol.h` after `WorldWelcome = 0x5411`:
```cpp
    LoginRequest    = 0x5412,  // C->S TF_AuthRequest
    LoginReply      = 0x5413,  // S->C TF_AuthReply
    RegisterRequest = 0x5414,  // C->S TF_AuthRequest
    RegisterReply   = 0x5415,  // S->C TF_AuthReply
    CharListRequest = 0x5416,  // C->S (empty)
    CharListReply   = 0x5417,  // S->C TF_CharListReply
    CharCreateReq   = 0x5418,  // C->S TF_CharCreateRequest
    CharCreateReply = 0x5419,  // S->C TF_CharOpReply
    CharDeleteReq   = 0x541A,  // C->S TF_CharDeleteRequest
    CharDeleteReply = 0x541B,  // S->C TF_CharOpReply
    EnterWorldReq   = 0x541C,  // C->S TF_EnterWorldRequest  (reply is the gated TF_WorldWelcome)
```
Then, inside the `#pragma pack(push,1)` block, add PODs with frozen sizes:
```cpp
struct TF_AuthRequest   { char user[32]; char pass[64]; };
static_assert(sizeof(TF_AuthRequest) == 96, "wire layout frozen");
struct TF_AuthReply     { uint8_t ok; uint8_t err; uint8_t _pad[2]; uint64_t accountId; };
static_assert(sizeof(TF_AuthReply) == 12, "wire layout frozen");   // verify actual packing; adjust _pad/assert to match
struct TF_CharBrief     { uint64_t id; char name[24]; uint8_t faction; uint16_t rank; uint8_t _pad; };
static_assert(sizeof(TF_CharBrief) == 36, "wire layout frozen");
struct TF_CharListReply { uint8_t count; uint8_t _pad[3]; TF_CharBrief chars[5]; };
static_assert(sizeof(TF_CharListReply) == 4 + 5*36, "wire layout frozen");
struct TF_CharCreateRequest { char name[24]; uint8_t faction; uint8_t _pad[3]; };
static_assert(sizeof(TF_CharCreateRequest) == 28, "wire layout frozen");
struct TF_CharOpReply   { uint8_t ok; uint8_t err; uint8_t _pad[2]; uint64_t charId; };
static_assert(sizeof(TF_CharOpReply) == 12, "wire layout frozen");
struct TF_CharDeleteRequest { uint64_t charId; };
static_assert(sizeof(TF_CharDeleteRequest) == 8, "wire layout frozen");
struct TF_EnterWorldRequest { uint64_t charId; };
static_assert(sizeof(TF_EnterWorldRequest) == 8, "wire layout frozen");
```
Compile and let the `static_assert`s tell you the true packed sizes; fix each `_pad`/assert to the real number (do NOT guess-ship a wrong assert).

- [ ] **Step 2: Server handlers + gating** in `TFServerSim`. Add private handlers `HandleLogin/HandleRegister/HandleCharList/HandleCharCreate/HandleCharDelete/HandleEnterWorld(PlayerId sender, const void* data, size_t size)` and register them in `RegisterNetHandlers` (`TFServerSim.cpp:427`) with the existing `route(TFMsg::X, [this](const NetworkMessage& m){ HandleX(m.senderID, m.payload.data(), m.payload.size()); })` pattern. Each: copy the POD, call `m_ctx->account`/`m_ctx->characters`, `SendToPlayer(sender, replyId, &reply, sizeof(reply), true)`. `HandleEnterWorld`: on `characters->EnterWorld` success, set the client's authoritative faction (`m_factions[sender]=rec.faction`; also `SetPlayerFaction`), mark the session in-world, then `SendWorldWelcome(sender)`.
  **Gating change:** in `PollClientJoinsLeaves` (`TFServerSim.cpp:508`), REMOVE the immediate `SendWorldWelcome(id)` on connect — instead just `m_knownClients.insert(id)`. `SendWorldWelcome` is now called ONLY from `HandleEnterWorld`. (Keep the leave-path cleanup + also `m_ctx->account->ClearSession(id)` on leave.)
- [ ] **Step 3: Client handlers** in `TFClientNetHandlers.cpp` `RegisterClientHandlers` (`:275`): `route(TFMsg::LoginReply, …)` etc., each calling a `TFClientNet::OnLoginReply(const void*,size_t)` that forwards to `m_ctx->loginFlow` (added Task 6; guard `if (m_ctx->loginFlow)`). Add the loopback cases in `RouteLoopback` (`:159`) for the new C->S ids so standalone works: `case TFMsg::LoginRequest: if (m_ctx->serverSim) m_ctx->serverSim->/*expose a public HandleLogin or a generic RouteClientMsg*/ …`. Simplest: add a public `TFServerSim::RouteClientMessage(PlayerId, TFMsg, const void*, size_t)` that dispatches to the same handlers, and call it from both the net route and the loopback route (DRY).
- [ ] **Step 4: Wire-layout test** (append to `TestTFOnboarding.cpp`): a standalone test that re-declares the same packed structs (or includes the header if it links) and asserts each `sizeof` matches the header's `static_assert` values — guards accidental drift. Then build the module + tests.
- [ ] **Step 5: Build the game module** (`$env:TF_TARGET='SparkGameMMOFPS'` + build) green, and SparkTests green. **Commit** — `feat(terrafront): onboarding net protocol + handlers; gate WorldWelcome behind enter-world`.

---

## Task 5: TFLoginFlow client UI state machine

**Files:**
- Create: `UI/TFLoginFlow.h/.cpp`

**Interfaces:**
- Consumes: the Task-4 message PODs (sends via `m_ctx->clientNet->SendMsg(TFMsg::X, &pod, sizeof(pod))`), `TFUi` helpers, `TFDataTables` (faction names/blurbs).
- Produces: `enum class TFFlowState{Login,Register,CharSelect,CharCreate,EnteringWorld,InWorld}`; `bool Initialize(TFGameContext&,TFEventBus&)`, `Update(float)`, `RenderUI()`, `Shutdown()`, `RenderDebugUI()`, `bool IsOpen() const { return m_state != TFFlowState::InWorld; }`, `TFFlowState State() const`, and reply sinks `OnLoginReply(bool ok,uint8_t err,uint64_t acct)`, `OnCharList(const TF_CharListReply&)`, `OnCharOpReply(...)`, `OnEnteredWorld()` (called by `TFClientNet` handlers).

- [ ] **Step 1: Implement the state machine + screens** styled exactly like `TFSpawnScreen` (full-viewport `NoDecoration|NoBackground` window, dimmed backdrop, centered panel, `TFUi::AddTextCentered`/`FactionCol`). All UI under `#ifdef SPARK_HAS_IMGUI` with empty stubs otherwise (mirror `TFSpawnScreen.cpp:464`).
  - `RenderLoginScreen`: `ImGui::InputText` user + `InputText(...,ImGuiInputTextFlags_Password)` pass, "Login" → send `TF_AuthRequest` as `LoginRequest`, "Register" toggles to Register state (same form → `RegisterRequest`). Show `m_error`.
  - On `OnLoginReply(ok)`: ok → `m_accountId=acct`, send `CharListRequest`, go `CharSelect`; else set `m_error` from `err`.
  - `RenderCharacterSelectScreen`: draw up to 5 cards from `m_chars` (name + faction color via `TFUi::FactionCol` + rank), buttons Enter (→ send `TF_EnterWorldRequest`, go `EnteringWorld`), Delete (→ `TF_CharDeleteRequest`), and a "Create" button (→ `CharCreate`).
  - `RenderCharacterCreateScreen`: `InputText` name + three faction buttons (the `DrawFactionSplash` pattern, `m_ctx->data->GetFaction(f)->name/blurb`) → send `TF_CharCreateRequest`; on `OnCharOpReply` ok → re-request `CharList`, go `CharSelect`.
  - `RenderEnteringWorldScreen`: a "Entering the Cindral Wastes…" splash; on `OnEnteredWorld()` (fired from the gated `TF_WorldWelcome` client handler) → `m_state=InWorld`.
- [ ] **Step 2: Build** the module green.
- [ ] **Step 3: Commit** — `feat(terrafront): TFLoginFlow UI state machine (login/char-select/create/enter)`.

---

## Task 6: Wiring — additive context/boot, gating, progression re-key, DESIGN.md

**Files:**
- Modify (ADDITIVE, FROZEN): `Core/TFTypes.h`, `Core/SparkGameMMOFPS.h`, `Core/Main.cpp`; `UI/TFSpawnScreen.cpp`; `Game/TFProgressionSystem.cpp`; `DESIGN.md`

- [ ] **Step 1: Extend `TFGameContext`** (`TFTypes.h`): forward-declare `TFDatabase; TFAccountSystem; TFCharacterSystem; TFLoginFlow;` (the forward-decl block ~`:96-113`) and add pointers `TFDatabase* db=nullptr; TFAccountSystem* account=nullptr; TFCharacterSystem* characters=nullptr; TFLoginFlow* loginFlow=nullptr;`. Add `bool InWorld() const { return loginFlow ? /*in-world*/ true-if-state==InWorld : true; }` — since `TFLoginFlow` is forward-declared here, implement `InWorld()` out-of-line in a .cpp OR store a plain `bool inWorld=false` flag on the context that `TFLoginFlow` sets (simplest: `bool inWorld=false;` field + `InWorld(){return inWorld;}`; `TFLoginFlow` sets `m_ctx->inWorld=true` on EnteringWorld→InWorld). Update the FROZEN header comment noting the W5 additions.
- [ ] **Step 2: Add members + boot** (`SparkGameMMOFPS.h` members `m_db,m_account,m_characters,m_loginFlow`; `Main.cpp` construct at :76, publish at :97 (`m_ctx.db=m_db.get()` etc.), boot rows in the `boots[]` table — order: db → account → characters → loginFlow, AFTER `data`/`world`/`serverSim`/`clientNet` since they depend on the context). `TFAccountSystem::Initialize`/`TFCharacterSystem::Initialize` call `SetDatabase(m_ctx.db)` and `TFDatabase::Open("Saves/terrafront.db")`. Add `Update`, `Shutdown` (reverse), `RenderDebugUI`, and `m_loginFlow->RenderUI()` calls (see below).
- [ ] **Step 3: Gate the UI on InWorld.** In `Main.cpp OnImGui` (`:243`): render `m_loginFlow->RenderUI()` UNCONDITIONALLY (it's the pre-world menu) BUT the existing `hud/map/spawnUI/scoreboard` block stays behind `HasLocalPlayer()` AND now also `m_ctx.InWorld()`. In `TFSpawnScreen.cpp` `Update` auto-open (`:146`), add `if (!m_ctx->InWorld()) return;` so the spawn/deploy screen only appears after entering the world. Extend the input-suppression in `TFClientNet.cpp:120` to also treat `loginFlow->IsOpen()` as `uiOpen`.
- [ ] **Step 4: Re-key progression.** In `TFProgressionSystem`, on XP change, also call `m_ctx->characters->PersistProgress(activeCharId, xp, rank, flux)` — the active character id comes from the session bound at enter-world (expose `TFCharacterSystem::ActiveCharacter(clientId)` or store it in the session). Keep the existing in-session runtime state; the character DB is now the durable store. Guard `if (m_ctx->characters)`.
- [ ] **Step 5: DESIGN.md** — add a "## W5 — Onboarding" section documenting: the flow, the 4 new systems, the `TFGameContext` additive extension (list the new fields), the new `TFMsg` ids, and the `TF_WorldWelcome` gating change. This is the record for the frozen-contract change.
- [ ] **Step 6: Build** the module green. **Commit** — `feat(terrafront): wire W5 onboarding into boot/context; gate world behind login; DESIGN.md`.

---

## Task 7: Acceptance — the flow works end to end (loopback + screenshots)

**Files:**
- Modify: `Console/TFCommands.cpp` (console cmds), create `Tools/tf_onboard.cfg`, create `docs/superpowers/acceptance/2026-07-06-onboarding.md`

- [ ] **Step 1: Console commands** for the loopback flow: `tf_register <user> <pass>`, `tf_login <user> <pass>`, `tf_char_create <name> <faction>`, `tf_char_list`, `tf_enter <charId|index>` — each builds the POD and calls `m_ctx.clientNet->SendMsg(...)` (standalone routes via loopback to the server sim). Register them in `TFCommands.cpp` beside the existing `tf_faction`/`tf_spawn`.
- [ ] **Step 2: Launch + screenshot the login screen.** Build the game module + game exe. Run detached (no console leak):
  `Start-Process build\windows-release\bin\SparkEngine.exe -ArgumentList '-game','D:\SparkEngine\build\windows-release\bin\SparkGameMMOFPS.dll','-exec','Tools/tf_onboard_shot.cfg','-test-seconds','8','-window-size','1280','720' -WorkingDirectory 'D:\SparkEngine' -WindowStyle Hidden -RedirectStandardOutput out.txt -RedirectStandardError err.txt`
  where `tf_onboard_shot.cfg` = `t3 gfx_screenshot Screenshots/onboarding_login.png`. VIEW the PNG — the Login screen must render (not the war).
- [ ] **Step 3: Drive the flow via -exec** (`Tools/tf_onboard.cfg`): `t3 tf_register cmd sekret1` / `t4 tf_login cmd sekret1` / `t5 tf_char_create Vanguard mra` / `t6 tf_char_list` / `t7 tf_enter 0` / `t9 gfx_screenshot Screenshots/onboarding_inworld.png`. Run it (detached), then grep the exec audit log (`exec_audit.log`) for the login-ok / char-created / entered lines, and VIEW `onboarding_inworld.png` — confirm a pawn is in the world after enter (the soldier renders, HUD up). If the flow stalls, diagnose from the audit log; do not fake the screenshot.
- [ ] **Step 4: Acceptance doc** — record the commands, the audit-log evidence, and what the two screenshots show; state plainly whether login→create→select→enter-world is verified.
- [ ] **Step 5: Commit** — `feat(terrafront): W5 onboarding acceptance (loopback login->create->enter->spawn)`.

---

## Self-review

- **Spec coverage:** Goal 1 (login screen) → T5+T6+T7; Goal 2 (char select) → T3+T5; Goal 3 (char create) → T3+T5; Goal 4 (enter as character + persistent progression) → T3+T6; Goal 5 (server-authoritative, standalone+networked one path) → T4 (loopback + socket via one handler). Persistence decision → T1. Auth decision → T2. Net decision → T4. Character model → T3 (faction+name, class per-spawn untouched). Frozen-contract change → T6 + DESIGN.md.
- **Placeholder scan:** the only deferred detail is the true packed `sizeof` of the new PODs (T4 Step 1 explicitly says compile-and-fix-the-assert, not guess) and the JSON-KV-vs-SQL backing (T1 Step 4 has a concrete pivot). No "TBD/handle edge cases".
- **Type consistency:** `TFDatabase`/`TFAccountSystem`/`TFCharacterSystem` signatures are stable T1→T7; error enums (`TFAuthErr`,`TFCharErr`) are defined once and mapped to wire `err` bytes in T4/T5; `TFFlowState` stable T5→T6.
- **Known risks flagged:** the frozen-contract additive edits (T6, purely additive + DESIGN.md), the DB-backend uncertainty (T1 pivot), and the wire-`sizeof` verification (T4).
