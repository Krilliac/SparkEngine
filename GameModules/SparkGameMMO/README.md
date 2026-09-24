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
The `mmo_*` console commands (`mmo_help` lists them) drive the systems.

Account passwords are stored only as `Spark::PasswordHash` hashes, never as plaintext.

## Known limitations

- Accounts and sessions are in memory only. `MMOAccountSystem` keeps them in `std::unordered_map`s and nothing
  persists them, so every account is lost when the process exits.
- Scene paths do not match the shipped scenes. `MMOWorldSetup` registers streaming areas as
  `Assets/Scenes/<AreaName>.scene` (for example `Assets/Scenes/TownSquare.scene`) and `MMODungeonSystem` uses
  `Assets/Scenes/shadow_crypt.scene`, but the scenes ship as `Assets/Scenes/MMO/town_square.scene`,
  `Assets/Scenes/MMO/shadow_crypt.scene`, and so on, so area streaming cannot find them.
- The `Data/Localization/mmo_*.json` string tables that `MMOEngineSystems` loads do not exist in the repository.

## Assets

Assets authored for this module live under `Assets/Models/MMO`, `Assets/Textures/MMO`, `Assets/Audio/MMO`,
`Assets/Materials/MMO`, and `Assets/Scenes/MMO`. Every file is listed in `Assets/assets.integrity.json`, and
`Assets/MMO/asset_manifest.json` records their license and generators. The module source does not reference the
model, texture, audio, or material files by path, and its scene paths miss the shipped scenes (see above).

## Tests

- `Tests/TestGameModuleMMO.cpp` (`MMO_*`) compiles the module's real system sources into SparkTests.
- `Tests/TestMMOCredentialSecurity.cpp` (`MMOCredentials_*`) covers password handling in the account system.
- `Tests/harden/Test_gamemodules_mmochat_di.cpp` (`MMO_ChatSystem_ResolvesNetworkViaInjectedContext`) checks that
  chat uses the injected engine context's `NetworkManager`.
