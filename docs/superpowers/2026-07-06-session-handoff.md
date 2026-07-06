# SparkEngine / TERRAFRONT — Session Handoff (2026-07-06)

Autonomous session on branch `claude/terrafront-buildout` → fast-forward merged into `Working`
(the repo's default branch; there is no `main`). 33 commits. Full test suite green (SparkTests
**5831 passed / 0 failed**); onboarding acceptance harness **20/20**.

## What shipped

1. **Exhaustive audit + design sweep** (`docs/superpowers/2026-07-06-exhaustive-sweep.md`) — 55-agent
   read-only sweep, 34 adversarially-verified confirmed-high findings + 7 feature design briefs.
2. **11 confirmed-high fix-lanes** (each build+test-gated, several with verified regression tests):
   scripting injection + sandbox ctor; combat cheats (weapon-switch + fire-rate); server→client auth
   (`senderID`); Win32 arg-injection; audio 3D stack-OOB; editor undo UAF; AsyncDatabase race + `?N`
   binding + newline truncation; module-loader crash + leak; ECS enum/mask round-trip data-loss; NaN
   tick-hang; faction cheat; vehicle eject scaling; RHIBridge null-guards.
3. **TERRAFRONT W5 onboarding (T1–T7)** — DB persistence, accounts (register/login), characters
   (CRUD + slot cap), packed net protocol (compiler-verified sizes), login FSM, boot wiring, and a
   **server-side enter-world gate**. Two review passes; acceptance harness proves the gate blocks
   un-entered clients AND the happy path; screenshot-verified (login screen → in-world HUD).
4. **sp4-dehardcode** — faction-material switch consolidated into `TFVisualUtils::FactionStructureMaterial`;
   skybox/terrain/wind/viewmodel/FX moved to `presentation.json` / `deployables.json`.
5. **Follow-up hardening** — PBKDF2-HMAC-SHA256 password KDF (`TFCrypto`, known-answer tested);
   progression load-on-enter + clear-on-disconnect (fixed a session-wipe data-loss); seated NaN guard;
   `inWorld` reset; authority-path vehicle/squad gate; editor `OpenScene` undo-clear; ScriptSandbox
   registration-time whitelist enforcement; editor fly-camera wired + `--open-scene` flag.

## ⚠️ Important caveats (read before trusting the above)

- **AngelScript is inert in shipping builds.** `SPARK_ANGELSCRIPT_SUPPORT` is not defined in CMake
  (SDK is include-wired but no library linked), so the real `AngelScriptEngine` — including the
  script-injection escaping (`c0dc3e85`) and the ScriptSandbox enforcement (`cadd8136`) — compiles but
  is never the active path (the stub ships). **Those two security fixes are correct but dormant until
  the SDK is linked into `CMakeLists.txt`.**
- **Editor screenshot/CI is a stub.** `SparkEngineIntegration::TakeScreenshot()` is a no-op and
  `EditorApplicationWindows::Run()` has no frame-limit; automated visual verification isn't possible yet.
- **Password KDF is a clean cutover** — old `std::hash`-format account hashes won't verify; recreate any
  dev/test accounts.

## Validate the visual bits (needs a human at the machine)

- **Fly-camera** (finding #21 fix): `D:\SparkEngine\build\bin\SparkEditor.exe --open-scene Assets/Scenes/Default.scene --debug-console`
  → right-drag orbit / wheel zoom / WASD+RMB fly; confirm the view moves (was hardcoded at eye `(0,3,-6)`).
- **Onboarding flow**: launch the game module, run `Tools/tf_onboard.cfg` (or `tf_selftest_onboarding`
  via `Tools/tf_onboard_selftest.cfg`) — login → char-create → enter-world.
- Launch engine/editor exes **detached** (`Start-Process -WindowStyle Hidden`) — they call `AttachConsole`
  and will leak into the parent console otherwise.

## Follow-ups / deferred (tracked)

Small / clean:
- Real AsyncDatabase test coverage (sweep #31 — `Tests/TestAsyncDatabase.cpp` uses a mock, not the real header).
- Windows screenshot + frame-limit machinery (unblocks editor visual CI).

Needs visual validation:
- Editor **B3 gizmo** overlay (design in the fan-out brief; GizmoSystem math exists, unwired).

Big / needs scoping (brainstorm first):
- **collab-server** — multi-user scene editing (new sub-project; design brief exists).
- **NetworkEncryption pipeline wiring** (sweep #8) — encryption/auth/replay layer exists but is not on
  the live send/receive path (all traffic plaintext once networking is live).
- **Windows daemon named-pipe transport** (`DaemonClient::Connect` is a `_WIN32` stub; daemon tests
  gated off on Windows) + wiring the **AngelScript SDK** into CMake (activates the dormant scripting +
  its security fixes — build-system change, verify carefully).

Minor security/robustness (from reviews, low-pri): a real crypto note remains that PBKDF2 params are
fixed at 150k iters; the vendored `JsonUtils` parser is lenient on malformed-but-braced input.

## Trilobite (local 7B)

Ran a 33-task compile+test coding eval (**76% pass**) + live use in sp4. Profile saved to file-memory
(`trilobite-offload-profile.md`): reliable for known algorithms / pure logic / scaffolds, unreliable for
wire-layout / API-signatures / edge-cases — **always compile-gate; never offload correctness-critical code**.
Its scrubbed lessons + eval writeup are committed **locally** in its repo (`~/.claude/mcp-servers/local-llm`,
commit `e0c0879`); the external push was blocked by the exfil classifier — run when ready:
`git -C ~/.claude/mcp-servers/local-llm push origin claude/seed-everything`.

## Ledger

Full per-task detail (every commit hash, every review finding, every deferral) is in
`.superpowers/sdd/onboarding-progress.md` (git-ignored scratch, on disk).
