# RDY-010 Module Lifecycle Evidence Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce and consume fail-closed, exact-commit Windows lifecycle evidence for the real stable-v1 SparkGameFPS module.

**Architecture:** Extend the post-teardown ModuleManager snapshot with module-scoped factory/callback counts, then emit one direct structured record per module from the Windows host. A Windows CI producer runs the downloaded exact-commit engine/DLL package and uploads JSON evidence; the existing module-evidence gate validates that artifact with its current target and JUnit evidence.

**Tech Stack:** C++23, CMake/CTest, Python 3 standard library, GitHub Actions, Windows D3D11 WARP.

**Spec:** docs/superpowers/specs/2026-09-09-rdy010-module-lifecycle-evidence-design.md

## Global Constraints

- Stable-v1 includes only SparkGameFPS; package-smoke evidence remains owned by MOD-310.
- The only admissible producer run is SparkEngine.exe -game <DLL> -require-game -test-seconds 1.0 -threads 2 -window-size 640x360 -no-subprocess with SPARK_RHI_BACKEND=d3d11 and SPARK_D3D11_DRIVER=warp.
- Terminal output is host-owned, line-anchored, post-teardown, and rejects duplicate or logger-prefixed records.
- A failed, timed-out, malformed, cross-SHA, or phase-incomplete run writes no lifecycle JSON.
- The CI job consumes only the same-workflow Windows Release artifact named for github.sha; it is producer evidence, not CI-120 external attestation.
- Do not change normal interactive logging, the module ABI, experimental-module coverage, or MOD-310 package-smoke scope.

---

### Task 1: Add real ModuleManager lifecycle-record coverage

**Files:**
- Modify: SparkEngine/Source/Core/ModuleManager.h
- Modify: SparkEngine/Source/Core/ModuleManager.cpp
- Modify: Tests/TestModuleLifecycleReal.cpp

**Interfaces:**
- Produces ModuleManager::ModuleLifecycleRecord { std::string module; uint64_t createModule; uint64_t onLoad; uint64_t onUpdate; uint64_t onFixedUpdate; uint64_t onRender; uint64_t onUnload; uint64_t destroyModule; uint64_t faults; }.
- Extends LifecycleEvidence with std::vector<ModuleLifecycleRecord> modules and FindModule(std::string_view) returning a const record pointer or nullptr.

- [ ] **Step 1: Write the failing real-fixture tests**

Add a success test that loads SPARK_TEST_COMPATIBLE_MODULE_PATH, calls InitializeAll, UpdateAll, FixedUpdateAll, RenderAll, ShutdownAll, and UnloadAll, then checks:

~~~cpp
const auto evidence = manager.GetLifecycleEvidence();
const auto* record = evidence.FindModule("Spark Compatible ABI Fixture");
ASSERT_NE(record, nullptr);
EXPECT_EQ(record->createModule, 1u);
EXPECT_EQ(record->onLoad, 1u);
EXPECT_GE(record->onUpdate, 1u);
EXPECT_GE(record->onFixedUpdate, 1u);
EXPECT_GE(record->onRender, 1u);
EXPECT_EQ(record->onUnload, 1u);
EXPECT_EQ(record->destroyModule, 1u);
EXPECT_EQ(record->faults, 0u);
~~~

Add a failure test with SPARK_MODULE_ABI_FAIL_ON_LOAD enabled. It must prove createModule == 1, onLoad == 0, onUnload == 1, and destroyModule == 1 so a failed initialization cannot become evidence.

- [ ] **Step 2: Run tests red**

Run: ctest --test-dir build/windows-shipping -C MinSizeRel -R "^ModuleLifecycle_" --output-on-failure --no-tests=error

Expected: compile failure for the absent record API or assertions fail because no per-module record exists.

- [ ] **Step 3: Implement bounded accounting**

Add an exact-name bounded record for each successfully named new-style LoadedModule. Increment createModule after non-null CreateModule and known ModuleInfo name; onLoad only after a true return; update/fixed/render only inside a completed guarded callback; onUnload after return; destroyModule after destroyFn returns. Increment that record's faults when the corresponding guarded callback does not complete. Extend AccumulateLifecycleEvidence to merge records by exact module name, preserving current staged-reload non-publication behavior.

~~~cpp
auto& record = FindOrCreateLifecycleRecord(entry.name);
++record.onUpdate; // only after entry.instance->OnUpdate completed
~~~

- [ ] **Step 4: Run tests green**

Run:
~~~powershell
cmake --build build/windows-shipping --config MinSizeRel --parallel 2
ctest --test-dir build/windows-shipping -C MinSizeRel -R "^ModuleLifecycle_" --output-on-failure --no-tests=error
~~~

Expected: build exits 0 and all selected tests pass.

- [ ] **Step 5: Commit**

~~~powershell
git add -- SparkEngine/Source/Core/ModuleManager.h SparkEngine/Source/Core/ModuleManager.cpp Tests/TestModuleLifecycleReal.cpp
git diff --cached --check
git commit -m "feat(modules): record per-module lifecycle evidence"
~~~

### Task 2: Emit and parse host-owned terminal records

**Files:**
- Modify: SparkEngine/Source/Core/SparkEngineWindows.cpp
- Modify: cmake/RunSparkModuleProfileLifecycle.cmake
- Modify: Tests/CMakeLists.txt

**Interfaces:**
- Consumes ModuleManager::GetLastTeardownLifecycleEvidence().modules.
- Produces one direct record per module:

~~~text
SPARK_MODULE_LIFECYCLE module=SparkGameFPS create=1 load=1 update=4 fixed=2 render=4 unload=1 destroy=1 faults=0
~~~

- [ ] **Step 1: Write failing parser self-tests**

Replace the aggregate-only fixture with the module-qualified fixture above. Add rejection cases for an unknown module, duplicate matching records, malformed fields, zero create/load/update/fixed/render/unload/destroy, nonzero faults, and a logger-prefixed line.

~~~cmake
_spark_expect_lifecycle_case(valid 0 "${_device}${_marker}" "" TRUE)
_spark_expect_lifecycle_case(duplicate 0 "${_device}${_marker}${_marker}" "" FALSE)
_spark_expect_lifecycle_case(logger_copy 0 "${_device}[info] ${_marker}" "" FALSE)
~~~

- [ ] **Step 2: Run test red**

Run: ctest --test-dir build/windows-shipping -C MinSizeRel -R "^ModuleProfileLifecycleParserContract$" --output-on-failure --no-tests=error

Expected: old aggregate-only output is rejected.

- [ ] **Step 3: Implement output and strict parser**

In SparkEngineWindows.cpp, replace the old aggregate-only output in the existing requireGame teardown block with stable-name ordered records using WriteCommandOutput. Do not use SimpleConsole. In the CMake parser, select only exact module=SparkGameFPS lines, require exactly one, validate the nine numeric fields, and retain the exact single WARP device record requirement.

- [ ] **Step 4: Run production smoke green**

Run:
~~~powershell
cmake --build build/windows-shipping --config MinSizeRel --parallel 2
ctest --test-dir build/windows-shipping -C MinSizeRel -R "^(ModuleProfileLifecycleParserContract|ModuleProfileLifecycle_SparkGameFPS_D3D11)$" --output-on-failure --no-tests=error
~~~

Expected: both tests pass and the real run contains exactly one standalone SparkGameFPS record.

- [ ] **Step 5: Commit**

~~~powershell
git add -- SparkEngine/Source/Core/SparkEngineWindows.cpp cmake/RunSparkModuleProfileLifecycle.cmake Tests/CMakeLists.txt
git diff --cached --check
git commit -m "feat(ci): emit stable module lifecycle records"
~~~

### Task 3: Refactor the collector and JSON schema

**Files:**
- Modify: tools/module-evidence/collect_lifecycle.py
- Modify: tools/module-evidence/lifecycle.py
- Modify: Tests/Tools/test_module_evidence.py

**Interfaces:**
- Replaces parse_trace(text, module) with parse_terminal_record(text, module).
- Adds CLI options --module-image, --working-directory, and --rhi-backend.
- Adds required record keys moduleSHA256 and modulePath.

- [ ] **Step 1: Write failing Python contracts**

Add tests for a valid record, duplicate matching records, logger-prefixed records, wrong module, malformed fields, zero required count, nonzero faults, exact command/environment construction, module image outside the supplied root, script/reparse rejection, no output file after failure, and missing/invalid moduleSHA256 or modulePath.

~~~python
VALID_RECORD = (
    "SPARK_MODULE_LIFECYCLE module=SparkGameFPS create=1 load=1 "
    "update=4 fixed=2 render=4 unload=1 destroy=1 faults=0"
)
~~~

- [ ] **Step 2: Run tests red**

Run: python -m unittest Tests.Tools.test_module_evidence.TestLifecycleCollector -v

Expected: parser/options/schema tests fail because the old collector only accepts obsolete raw traces.

- [ ] **Step 3: Implement strict collection**

Validate engine and module images are regular non-reparse binaries under --working-directory; require the Windows expected_library_names(module) basename. Launch only:

~~~python
cmd = [
    str(engine), "-game", str(module_image), "-require-game",
    "-test-seconds", "1.0", "-threads", "2", "-window-size", "640x360",
    "-no-subprocess",
]
env = {**os.environ, "SPARK_RHI_BACKEND": "d3d11", "SPARK_D3D11_DRIVER": "warp"}
~~~

Write the captured log alongside JSON. Parse only the exact standalone host record. Store moduleSHA256/modulePath beside existing engine digest/path and source tree SHA. Reject unknown record keys and invalid digest formats in lifecycle.check_record.

- [ ] **Step 4: Run tests green**

Run:
~~~powershell
python -m unittest Tests.Tools.test_module_evidence -v
python tools/module-evidence/validate_manifest.py --repo-root . --policy-only
~~~

Expected: all tests pass and policy-only validation still reports the lifecycle gap until the producer job lands.

- [ ] **Step 5: Commit**

~~~powershell
git add -- tools/module-evidence/collect_lifecycle.py tools/module-evidence/lifecycle.py Tests/Tools/test_module_evidence.py
git diff --cached --check
git commit -m "feat(evidence): collect real module lifecycle records"
~~~

### Task 4: Add the Windows producer and fail-closed consumer

**Files:**
- Modify: .github/workflows/build.yml
- Modify: tools/module-evidence/evidence-gaps.json
- Modify: tools/module-evidence/schema.py
- Modify: .github/scripts/test-workflow-failure-propagation.py
- Modify: Tests/Tools/test_module_evidence.py

**Interfaces:**
- Produces module-profile-lifecycle, requiring build-windows-vs2022.
- Produces artifact module-profile-lifecycle-${{ github.sha }} containing build/module-evidence/module-lifecycle.json and module-lifecycle-SparkGameFPS.log.
- Changes module-evidence needs to [build-linux-gcc, module-profile-lifecycle] and passes --lifecycle-evidence to the validator.

- [ ] **Step 1: Write failing workflow-policy tests**

Require:

~~~python
self.assertIn("module-profile-lifecycle:", workflow)
self.assertIn("needs: [build-windows-vs2022]", lifecycle_job)
self.assertIn("SparkEngine-Windows-VS2022-Release", lifecycle_job)
self.assertIn("collect_lifecycle.py", lifecycle_job)
self.assertIn("module-profile-lifecycle-${{ github.sha }}", lifecycle_job)
self.assertIn("needs: [build-linux-gcc, module-profile-lifecycle]", module_evidence_job)
self.assertIn("--lifecycle-evidence", module_evidence_job)
~~~

Also add a manifest test proving missing or cross-SHA lifecycle JSON fails once its gap row is removed.

- [ ] **Step 2: Run tests red**

Run:
~~~powershell
python .github/scripts/test-workflow-failure-propagation.py WorkflowFailurePropagationTests.test_module_evidence_pipeline
python -m unittest Tests.Tools.test_module_evidence -v
~~~

Expected: policy assertions fail because the producer, artifact, and validator argument do not exist.

- [ ] **Step 3: Implement artifact flow**

Add module-profile-lifecycle on windows-2022. It checks out source, downloads only SparkEngine-Windows-VS2022-Release from the same workflow, expands it into a fresh directory, resolves SparkEngine.exe and SparkGameFPS.dll underneath it, runs the collector with --commit-sha "${{ github.sha }}", and uploads JSON plus log with if-no-files-found: error.

Make module-evidence download this artifact and invoke:

~~~bash
python3 tools/module-evidence/validate_manifest.py --repo-root . --target-evidence build/module-evidence/module-targets.json --lifecycle-evidence build/module-evidence/module-lifecycle.json --expected-sha "$MODULE_EVIDENCE_SHA" --allow-declared-gaps tools/module-evidence/evidence-gaps.json
~~~

Delete only the lifecycle-log object from evidence-gaps.json; retain MOD-310 package-smoke-log. Set lifecycle producer metadata to the new job. Add module-profile-lifecycle to required-ci-gate needs and its exact expected-job JSON.

- [ ] **Step 4: Run tests green**

Run:
~~~powershell
python .github/scripts/test-workflow-failure-propagation.py
python .github/scripts/test-verify-exact-required-gate.py
python -m unittest Tests.Tools.test_module_evidence -v
python tools/module-evidence/validate_manifest.py --repo-root . --policy-only
~~~

Expected: all local contracts pass; policy-only validation reports only MOD-310 package smoke, never a hosted lifecycle success.

- [ ] **Step 5: Commit**

~~~powershell
git add -- .github/workflows/build.yml .github/scripts/test-workflow-failure-propagation.py tools/module-evidence/evidence-gaps.json tools/module-evidence/schema.py Tests/Tools/test_module_evidence.py
git diff --cached --check
git commit -m "feat(ci): produce stable module lifecycle evidence"
~~~

### Task 5: Rehearse locally and update truthful readiness wording

**Files:**
- Modify: docs/readiness/work-items/00-truth-ci-release.json
- Modify: docs/readiness/ENGINE_READINESS_HANDOFF.md
- Modify: Tests/Tools/test_site_data_contract.py
- Generated: exact paths reported by docs/update-all-docs.sh update

**Interfaces:**
- Records RDY-010 as in progress with local producer/consumer code, exact hosted proof outstanding, and MOD-310 package smoke still open.

- [ ] **Step 1: Write a failing readiness cross-reference test**

Require RDY-010 not to claim lifecycle evidence complete while evidence-gaps.json contains lifecycle-log, and not to claim RDY-010 complete while any remaining declared gap is owned by MOD-310.

- [ ] **Step 2: Run test red**

Run: python -m unittest Tests.Tools.test_site_data_contract -v

Expected: stale rationale/implementation wording fails the new cross-reference contract.

- [ ] **Step 3: Produce local real evidence and correct wording**

Run the collector against the built SparkEngine.exe and SparkGameFPS.dll under build/windows-shipping with the exact producer command. Do not commit generated evidence. Update RDY-010 to say local producer/consumer code exists, hosted exact-SHA evidence remains required, and MOD-310 owns package smoke. Do not mark the work item complete.

- [ ] **Step 4: Run documentation gates**

Run:
~~~powershell
python tools/site-data/validate.py
python -m unittest Tests.Tools.test_site_data_contract -v
python Tests/Tools/test_docs_health.py -q
& 'C:\Program Files\Git\bin\bash.exe' docs/update-all-docs.sh update
git diff --check
~~~

Expected: every command exits 0 and wording is evidence-backed.

- [ ] **Step 5: Commit and verify deterministic docs**

~~~powershell
git diff --name-only -- README.md wiki docs .github
git add -- docs/readiness/work-items/00-truth-ci-release.json docs/readiness/ENGINE_READINESS_HANDOFF.md Tests/Tools/test_site_data_contract.py README.md wiki/getting-started/Engine-Architecture-Flowchart.md wiki/advanced/Codebase-Statistics.md
git diff --cached --check
git commit -m "docs(readiness): track module lifecycle producer evidence"
& 'C:\Program Files\Git\bin\bash.exe' docs/update-all-docs.sh check
~~~

### Task 6: Whole-slice verification and review

**Files:**
- Inspect every file changed by Tasks 1-5.

**Interfaces:**
- Verifies producer-to-consumer behavior locally without claiming protected remote release authority.

- [ ] **Step 1: Run Windows build preflight**

Run: powershell -NoProfile -File "C:\Users\Nathan\.claude\scripts\fleet-preflight.ps1"

Expected: GO. If CAUTION or STOP, serialize builds and free resources before retrying.

- [ ] **Step 2: Run complete verification**

~~~powershell
cmake --build build/windows-shipping --config MinSizeRel --parallel 2
ctest --test-dir build/windows-shipping -C MinSizeRel -R "^(ModuleLifecycle_|ModuleProfileLifecycle_)" --output-on-failure --no-tests=error
python -m unittest Tests.Tools.test_module_evidence -v
python .github/scripts/test-workflow-failure-propagation.py
python .github/scripts/test-verify-exact-required-gate.py
python tools/site-data/validate.py
python Tests/Tools/test_docs_health.py -q
git diff --check
~~~

Expected: every command exits 0. A local pass proves implementation, not hosted exact-SHA evidence.

- [ ] **Step 3: Request independent review**

Dispatch one read-only reviewer for lifecycle accounting, host-output spoof resistance, collector binary/path validation, artifact SHA binding, workflow needs, and lifecycle-gap removal. Fix every Critical and Important finding and rerun affected tests.

- [ ] **Step 4: Preserve remote-evidence handoff**

Do not publish without explicit remote-mutation confirmation. After publication, require a completed exact Working run with module-profile-lifecycle, module-evidence, and required-ci-gate all passing before updating any release status. Preserve URLs and artifact names in the release ledger.
