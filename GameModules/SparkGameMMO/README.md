# SparkGameMMO

SparkGameMMO is a prototype of MMO gameplay systems: accounts and characters, chat, guilds, parties, inventory,
crafting, trading, reputation, achievements, dungeons, and world bosses, driven from console commands.

**Release classification:** experimental prototype, outside the stable-v1 release profile
(`tools/module-evidence/manifest.json`). Turning it into a secure, persistent, integrated world is tracked under
MOD-320.

## What runs

`Source/Core/Main.cpp` creates the world setup, player, chat, inventory, crafting, guild, trading, party,
achievement, reputation, dungeon, world-boss, gameplay-session, persistence, account, character, login-UI, and
engine-bridge systems on load. World areas are registered with the engine's seamless area streaming manager, and
chat and world traffic go through the engine `NetworkManager` resolved from the injected engine context.
Character and world state are written through `MMOPersistenceSystem`, which uses the engine `AsyncDatabasePool`.
Character IDs come from a counter stored in the database (`meta_next_character_id`), seeded on open from that
counter and from every stored `character_<id>` key, so a restarted server never reissues an ID an earlier run
used. Each character save is one transaction: the character row, the whole inventory as one
`inventory_<id>` record that keeps every slot index, and the reputation, achievement, crafting and lockout
records commit together, and the pool's single worker applies queued saves in order. A save also deletes the
records the character no longer has, so a lost faction, achievement, stat, skill, recipe or expired lockout
stays gone after a restart. Loads refuse out-of-range skill levels, unknown disciplines and difficulties,
unparsable values and expired lockouts. Deleting a character deletes every record keyed by its ID.

Guilds are world state. On load the module reads every `guild_<id>` and `gm_<guild>_<player>` record and the
`meta_next_guild_id` counter, and `MMOGuildSystem::RestoreGuilds` rebuilds the guilds. The rank permissions are
reset to their defaults, which nothing changes at runtime. Each auto-save and the unload path save the whole guild
set in one transaction, deleting disbanded guilds and departed members. A malformed world record, or a guild set
that fails validation, disables persistence for that run and leaves the store untouched for repair. Guild
activity logs and per-member level, note and contribution are not persisted. Boss kills are not persisted:
`MMOWorldBossSystem` keeps no kill history, so a load neither reads nor validates `bosskill_*` records.
The `mmo_*` console commands (`mmo_help` lists them) drive the systems.

Account passwords are stored only as `Spark::PasswordHash` hashes, never as plaintext.

## Network session gate

With networking enabled, the module owns `MMOSessionGate` over the real `NetworkManager` secure channel.
`SparkServer` injects that service before module initialization; the shared headless host can start it on the
module's first update. An existing host-owned listener is adopted without rebinding or ticking it twice.
Client console registration and login call the server's `MMOAccountSystem`; login is wrapped by
`Spark::Gateway::GuardedGatewayAuthenticator`. Transport admission alone grants no character authority.

After connecting with the engine's normal network commands and pinned/known-host trust, use `mmo_register`,
`mmo_login`, `mmo_session_create <name> <race> <class>`, and `mmo_session_enter <character-id>`.
`mmo_session_status` reports the last asynchronous reply, including account and character IDs. Human/Warrior
use race/class IDs `0/0`. Credentials longer than the gate's bounded fields are rejected, never truncated.
The local login UI remains the offline showcase flow; network admission uses these console commands.

Character ownership comes from the server account index. Each move is a normalized direction integrated for
one server-selected step, funded by server-time credit. Client positions and supplied account IDs cannot
choose authority. `mmo_session_interact <other-character-id>` greets a living character in the same area
within three metres; the server applies its cooldown and records the greeting target/count. Both clients
receive authoritative positions and greetings. Legacy position uploads are refused while the gate is active.
Disconnect, expired authentication, and replacement login revoke the previous world actor.

This is a bounded small-area session slice. It does not add account persistence, world collision, remote
login UI, cross-area handoff, or server-restart recovery. It remains experimental. The registered
`MMOIntegratedWorld_TwoClientSessionGate` CTest starts a server and two client processes using these production
services; it must pass centrally before the MOD-320 criterion is promoted. `MMOSessionGateProtocol` pins the
codec tests. Local syntax checks are not runtime qualification or exact-commit CI.

The new byte parser has a fuzz harness and reviewed seeds. Required SEC-120 registration is documented in
[SessionGate-Fuzz-Integration.md](SessionGate-Fuzz-Integration.md); policy files are owned by the SEC-120 lane.

## Known limitations

- Accounts and sessions are in memory only. `MMOAccountSystem` keeps them in `std::unordered_map`s and nothing
  persists them, so every account is lost when the process exits. Because account IDs restart at 1, the
  character-select list is not rebuilt from the database after a restart: a stored character's owner ID could
  name a different account in the new process. `MMOPersistenceSystem::ListCharacters` is ready for that once
  accounts are durable.
- Persistence uses the engine's key-value fallback store, and the restart tests restart it inside one process.
  No test yet restarts a server with two connected clients.
- Two dungeons have no authored scene. Each world area carries an exact-case `sceneFile` under
  `Assets/Scenes/MMO/` (for example `Assets/Scenes/MMO/town_square.scene`), and the Shadow Crypt dungeon uses
  `Assets/Scenes/MMO/shadow_crypt.scene`. Forgotten Mine and Void Spire have no scene yet, so they stay registered
  but are not enterable: `CreateInstance` refuses them and `mmo_dungeon_enter` reports why.
- The `Data/Localization/mmo_*.json` string tables that `MMOEngineSystems` loads do not exist in the repository.

## Assets

Assets authored for this module live under `Assets/Models/MMO`, `Assets/Textures/MMO`, `Assets/Audio/MMO`,
`Assets/Materials/MMO`, and `Assets/Scenes/MMO`. Every file is listed in `Assets/assets.integrity.json`, and
`Assets/MMO/asset_manifest.json` records their license and generators. The module source does not reference the
model, texture, audio, or material files by path. `MMOWorld_AreaScenePathsExistExactCase` checks that every
registered scene path exists with exact case and that each scene's `areaId` header matches its area.

## Tests

- `Tests/TestGameModuleMMO.cpp` (`MMO_*`, `MMOWorld_*`) compiles the module's real system sources into SparkTests.
- `Tests/TestMMOCredentialSecurity.cpp` (`MMOCredentials_*`) covers password handling in the account system.
- `Tests/TestMOD320MMOPersistenceReal.cpp` (`MMOPersistence_*`) reopens a real store after shutdown: unique and
  restart-stable character IDs, owner-filtered character lists, exact inventory slots, ordered async saves,
  crafting and lockout values, removed progression that stays removed, refused out-of-range records, complete
  character deletion, guilds and members (including disbanded guilds and invalid restores), malformed world
  records, and the boss-kill log.
- `Tests/harden/Test_gamemodules_mmochat_di.cpp` (`MMO_ChatSystem_ResolvesNetworkViaInjectedContext`) checks that
  chat uses the injected engine context's `NetworkManager`.
