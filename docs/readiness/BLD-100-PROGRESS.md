# BLD-100 progress log

This is the full dated progress log for work item BLD-100 (Create strict reproducible Shipping configurations).
The item's `rationale` in `docs/readiness/work-items/00-truth-ci-release.json` summarizes it, because the
work-item loader caps each string at 8192 characters. Entries up to 2026-09-25 are quoted verbatim from the
rationale as it stood before the split; add new progress here and keep the rationale summary in step.
Status, acceptance state and closure evidence remain governed by the work item itself.

## Baseline

Versioned Windows publication now selects the authoritative windows-shipping preset and MinSizeRel product tree, with separate windows-release validation. Nightly retains Debug/Release packages. Executed workflow-selection and asset-collection regressions, plus staged-package profile checks, verify the local orchestration contract.

## 2026-09-12 progress

the reviewed build-matrix inventory now binds stable shipping and validation profiles to matching CMake build presets/configurations and rejects drift. Runtime-only component staging now has an explicit trusted-layout preflight before CPack, retaining executable, runtime-content, and all configured module sidecar/hash checks without requiring installed SDK files.

## 2026-09-13 progress

the inventory retains CMake’s Visual Studio instance, archiver, and linker identities and fails closed when they are missing and disagree. It now also accepts only absolute Windows x64 C++ compiler paths, preventing relative or non-Windows spellings from being mistaken for exact MSVC provenance. Local fixture validation does not establish native installer execution. Hosted Windows Shipping packaging, reproducibility comparison, exact toolchain/dependency manifests, and private symbol retention remain unverified or unfinished; this item is not complete.

## 2026-09-24 progress

MinSizeRel now also defines SPARK_SHIPPING=1 on SparkEngineLib (PUBLIC, root CMakeLists.txt FEATURE_DEFINITIONS), so the SPARK_DEBUG_HOOK*, SPARK_TRACKED_LOCK and detector macros compile out of Shipping; DebugHookManager.h rejects SPARK_BUILD_SHIPPING without SPARK_SHIPPING, and test_build_shipping_contract.py rejects any first-party CMake call that gives MinSizeRel the Shipping profile without it. In a local Linux MinSizeRel tree no SparkEngineLib object except DebugHookManager.cpp.o references DebugHookManager (Release: 14), and a 60-frame headless SparkEngine run exits 0. The class stays in the executable through --whole-archive, and the lifecycle still initializes and ticks HitchDetector, AssetStallDetector, NetworkHealthMonitor, GPUResourceLeakDetector and InvalidStateDetector directly in Shipping; no hosted Windows Shipping build of this change has been observed.

## 2026-09-24 OD-04 CPU floor progress

vendored Jolt defaulted to USE_AVX2/AVX/FMADD/F16C/LZCNT/TZCNT=ON and publishes its ISA options PUBLIC, so the local linux-gcc-release SparkEngineLib compile carried -mavx2 -mbmi -mf16c -mfma -mlzcnt and JPH_USE_AVX2; the 'Unsupported CPU features are not silently required' criterion was violated in every non-native configuration, Shipping included. cmake/SparkCpuFloor.cmake now pins those Jolt options OFF (SSE4.1/SSE4.2 ON) unless SPARK_NATIVE_ARCH=ON, and spark_assert_cpu_floor() at the end of the root CMakeLists.txt fails configuration when any target or global flag selects AVX/AVX2/AVX-512/FMA/F16C/LZCNT/BMI, a non-floor -march, /arch:AVX* or a Jolt JPH_USE_AVX*/LZCNT/TZCNT/F16C/FMADD selector. Tests/Tools/test_cpu_floor.py (wired into build.yml) runs real CMake configurations, including vendored Jolt with upstream defaults (rejected) and through the floor (accepted). The reviewed Jolt build-matrix coordinates and inventory were regenerated. Per-source-file options are not scanned, no hosted windows-shipping configure of this change has been observed, and no Shipping binary has run on an SSE4.2-only CPU or emulator.

## 2026-09-25 OD-04 linked-image progress

tools/check_isa_baseline.py disassembles ELF/PE images (objdump or llvm-objdump) and fails on VEX/EVEX, ymm/zmm, AVX-512 opmask, FMA, F16C, BMI1/BMI2, LZCNT and MOVBE instructions and on the legacy-encoded AES-NI, PCLMULQDQ, SHA-NI, GFNI, RDRAND, RDSEED and ADX families outside --allow-symbol CPUID-dispatched functions; instructions outside those families (for example XSAVE or TSX) are not classified. TZCNT is reported but allowed because it shares the REP BSF encoding GCC and Clang emit at the floor. spark_cpu_floor_violations() now also rejects -mmovbe, -maes, -mpclmul, -msha, -mgfni, -mrdrnd, -mrdseed, -madx, -mvaes and -mvpclmulqdq (test_cpu_floor.py). CTest CpuFloor_IsaBaselineChecker (15 unittest cases) proves the checker on gcc ELF fixtures built at the floor (pass), with -mavx2 -mfma, -mbmi -mbmi2, -mlzcnt and -mf16c, and with each legacy-encoded family (AES-NI/PCLMULQDQ/SHA/GFNI/RDRAND/RDSEED via -m flags, ADX via inline asm), each failing by feature, on a clang/lld-link PE DLL built with and without AVX2, and rejects non-x86, missing and empty images. CTest CpuFloor_IsaBaseline scans this tree’s SparkEngine, SparkEditor, SparkServer and all game-module images and is registered for ELF toolchains only: MSVC statically links the STL’s CPUID-dispatched AVX2 vector_algorithms code into every image and an MSVC PE has no COFF symbol table, so Windows images are not scanned until a PDB-aware exemption exists. Locally, linux-gcc-release reports 0 above-floor instructions across 14 images. The ledger recorded 17192 ymm/FMA instructions in bin/SparkEngine before the Jolt pin. Utils/MultiISA.h now detects features at runtime with CPUID/XGETBV, replacing the compile-time macros and the MSVC stub that hard-coded AVX2; CpuNeuralInference reports the level of the kernel MultiISADispatch::SelectLevel() actually chose (SSE2 in floor builds, which compile no AVX2 variant), not the CPU capability. The SparkEngine (Windows and POSIX), SparkEditor and SparkServer entry points call DescribeStableCpuFloorFailure() before logging, crash handling or server startup, and exit with a message naming the missing SSE4.2/POPCNT-level features (MultiISA_CpuFloor_* SparkTests cover the check). The below-floor startup path has not been executed on real or emulated below-floor hardware, and the Windows entry-point changes have not been compiled locally.

## 2026-09-25 private-symbol progress

STRIP_DEBUG_SYMBOLS now keeps symbols out of the runtime package instead of never producing them. MSVC images always link with /DEBUG (/PDBALTPATH:%_PDB% outside Debug), so each carries an RSDS PDB GUID/age with a bare PDB name. ELF images link with -Wl,--build-id=sha1. With STRIP_DEBUG_SYMBOLS=ON every target compiles with -g (the static libraries hold most shipped code), and only the SPARK_SHIPPED_IMAGE_TARGETS get cmake/SparkSplitDebugLink.cmake as their per-target C/C++ LINKER_LAUNCHER; it splits <image>.debug off and strips the image with a .gnu_debuglink inside the link step, so module .sparkabi hashes see the stripped image. Every other image (SparkTests, test probes) links outside Debug with -g0 and -Wl,--strip-all and gets no .debug; under GCC 13 LTO a link-time -g0 stops LTRANS debug generation, which a probe confirmed (compile -g propagates through -flto unless the link says -g0). The global MinSizeRel -Wl,--strip-all is skipped in that mode, because it discarded the symbols before the split. PDBs and .debug files of SPARK_SHIPPED_IMAGE_TARGETS install only into a new symbols component that CPACK_COMPONENTS_ALL excludes. tools/shipping_symbol_manifest.py (stdlib only) writes a closed spark.shipping-symbol-manifest/1 JSON mapping each ELF image by build-id, debuglink name and CRC-32, and each PE image by RSDS GUID plus DBI age, to exactly one symbol file. It fails on missing, duplicate, orphaned or mismatched symbols, unstripped images, absolute PDB paths and symbols in the runtime tree. CTest ShippingManifest_SymbolManifestTool (30 cases) proves it on gcc fixtures built through the production launcher and on clang/lld-link PE+PDB fixtures cross-checked with llvm-readobj and llvm-pdbutil. It also keeps the target list in step with every install(TARGETS) rule in the root, Spark*/, GameModules/ and cmake/ CMake files, and rejects any CPACK_COMPONENTS_ALL list (root or cmake/SparkCPackOptions.cmake) that names the symbols component. ShippingManifest_PrivateSymbols, registered in STRIP_DEBUG_SYMBOLS ELF trees, installs the runtime/tools/samples and symbols components to separate roots and maps every installed image; it needs every installed target built. In a local linux-shipping MinSizeRel tree with ENABLE_LTO=OFF (SparkGameFPS), exactly the 18 shipped targets link through the split launcher, SparkBuildDownloaderTests and SparkBuildProcessRunnerTests link with -g0 and --strip-all and get no .debug, and ShippingManifest_PrivateSymbols passes: 18 installed images (libSparkGameFPS.so installs to bin and lib) map to 17 installed symbol files, and none keeps .debug_info or .symtab. The module .sparkabi hash matches the stripped module. In a preset-exact linux-shipping tree (ENABLE_LTO=ON, BUILD_TESTS=ON) only SparkDaemon, SparkOrchestrator and SparkDaemonServiceTests were built: both daemons map to DWARF .debug files and the unshipped test image is stripped with no .debug. The full LTO build with -g of SparkEngineLib and SparkTests, which the telemetry, security-runtime and network-integration jobs perform, has not been built locally, so its disk, memory and time on hosted runners are unmeasured; all SparkEngine-scale evidence here is LTO-off. A 30-frame headless SparkEngine run exits 0, and addr2line resolves main through SparkEngine.debug. build-windows-shipping now stages the components, runs the manifest tool and uploads the PDBs and manifest as a separate shipping-symbols-<sha> artifact. No hosted MSVC build of this change, no /Brepro PDB determinism evidence and no symbol-server publication (OPS-100) exist yet. The ShippingManifest_* selector covers only the private-symbol manifest: the exact MSVC, Windows SDK, dependency, CPU and configuration manifests and the two-clean-build comparison have no tests.

## 2026-09-25 progress

tools/compare_build_outputs.py writes a closed spark.build-output-manifest/1 (relative path, size, SHA-256, GNU build-id or COFF timestamp and RSDS, per-section hashes) and compares two, naming the first differing section. SparkReproducibleBuild.cmake now also maps the build root (-ffile-prefix-map=<build>=., after the source map; GCC LTO repeats both at link) and seeds GCC LTO objects with -frandom-seed=<OBJECT>. Fail-before: two trees built from the pre-change source differed in bin/SparkCooker (build-id), SparkCooker.debug (.debug_info comp_dir) and libSparkAssetPipelineCore.a. After: ReproducibleBuild_LinuxToolTargets passes locally (GCC 13.3, about 75 s), a single manual Clang two-tree run outside the CTest (no retained artifact) was equivalent, and a same-tree rebuild gives a byte-identical archive. GCC LTO IR in static-library members still records the build directory, so the CTest compares bin/ only. The reproducibility-windows job (two checkouts, two windows-shipping builds and installs, compared) is job-level continue-on-error and not a required-ci-gate need; it has never run, and MSVC /Z7 objects in the SDK libraries may differ.

## 2026-09-28 OD-24 comparator and Windows CPU scan (local lane)

`tools/compare_build_outputs.py` now implements the OD-24 replacement only inside
`.debug$S` sections of valid x86 COFF/BigObj members of `.lib` archives. The
replacement has the original root byte length. Headers, section layout, relocation
and symbol tables, padding, other members and sections remain exact; overlapping
or unsupported layouts receive no normalization. Standalone COFF objects are
compared exactly. The closed `spark.build-output-manifest/2` schema records each
normalized member name, ordinal and root length; old manifests must be regenerated.
The Windows two-tree CTest scans both `bin/MinSizeRel` and `lib/MinSizeRel`, with
equal-length build roots. The advisory workflow uses `tree-a` and `tree-b` and
passes native Windows build-root strings to both stage manifests. Its
`continue-on-error` remains unchanged.

The registered in-memory comparator tests cover equal-root replacement, BigObj,
non-library/standalone objects, non-COFF members, header/layout/relocation/symbol
overlap, archive padding, unequal roots, and PE/ELF manifest validation. A real
MSVC 14.44.35207 two-directory `/Z7 /Brepro /bigobj` single-source archive probe
still fails after normalization: the BigObj timestamp and bytes in `.debug$T`
differ. A clang-cl probe additionally retained `.debug$T` paths and auxiliary
section checksums. OD-24 forbids normalizing those bytes. This implements the
comparator policy, not Windows clean-build equivalence; no reproducibility
criterion promotion is proposed from these probes.

`tools/check_isa_baseline.py` now scans Windows PE images; each needs its
matching PDB (`--pdb IMAGE=PDB`) and `llvm-pdbutil`, and a missing or mismatched
PDB is a tool error (exit 2). The scanner validates PE/PDB GUID and age, streams `llvm-pdbutil` procedure
records, interprets their section offsets as decimal, and restricts exemptions
to exact reviewed procedure/module pairs and half-open executable byte ranges.
Only AVX/AVX2 is allowed there; other ISA families and neighboring functions
remain violations. ELF regex exemptions cannot exempt a PE image.

Review provenance is installed MSVC 14.44.35207: `crt/src/stl/vector_algorithms.cpp`
uses `_Use_avx2()` at line 26 (`__isa_enabled & (1 << __ISA_AVAILABLE_AVX2)`).
The explicit procedure list cites the direct guards and guarded wrapper targets
in the checker. `crt/src/x64/memcpy.asm` lines 266-267 and `memset.asm` lines
204-205 branch to `NoAVX` when `__isa_available` is below AVX. Concrete PDB module
paths were observed in locally linked `/MT`, `/MD` and `/MDd` fixtures; unknown
module variants/functions fail closed until reviewed. These source files were
read locally; no external service was contacted.

`CpuFloor_IsaBaseline` stays ELF-only. Scanning the real MSVC Release images of
the local windows-release tree (ENABLE_LTO=ON) with their PDBs fails:
SparkServer.exe 1098 above-floor instructions, SparkGame.dll 1030,
SparkEngine.exe 1361 and SparkEditor.exe 1356 (one AVX-512) remain after the reviewed ranges
(SparkServer: 11 exempted). Attributed through the SparkServer PDB:

- libsodium AVX2 and AES-NI implementations (`salsa20_xmm6int-avx2`,
  `chacha20_dolbeau-avx2`, `aegis128l_aesni`, `aegis256_aesni`; 1046 of 1109).
  The MSVC branch of cmake/SparkLibsodium.cmake compiles them, while its
  non-MSVC branch documents that above-floor variants are excluded because this
  scan rejects them even behind CPUID. The same bytes are in every image that
  links spark_sodium; SecureChannel::Seal/Open and PasswordHash also carry VEX
  instructions under LTCG.
- The UCRT's inline `wmemchr`/`wmemcmp`, whose AVX2 paths are guarded by
  `_Avx2WmemEnabled`, compiled into engine objects (GatewayAreaControl.obj).
- `lzcnt` in EnTT code via MSVC `<bit>` countl_zero, which dispatches on
  `__isa_available` inline in engine objects.
- 116 instructions in vector_algorithms.obj contributions with no S_GPROC32
  record in the PDB, so no reviewed range can cover them.

Only the libsodium case is a build-configuration defect; the others are
CPUID-guarded code that the reviewed-range mechanism cannot identify (inline in
arbitrary modules, or without procedure records). Registering the Windows scan
before these are resolved or separately reviewed would fail every Windows CTest
run, so the criterion "Unsupported CPU features are not silently required" is not
met for Windows. The scan has not been run on a MinSizeRel windows-shipping tree.

Local validation: the focused Python comparator/range/classifier/wiring suite
passes. Linked clang/lld PE fixtures pass at the floor and in an allowlisted
range, and reject ordinary AVX2, adjacent unreviewed AVX2, FMA inside an otherwise
allowed function, and missing/mismatched PDBs. Native MSVC `/MT`, `/MD` and `/MDd`
fixtures pass with their actual STL/CRT PDB ranges. The pre-change scanner rejects
the guarded native fixture, and the pre-change comparator cannot equate the
synthetic archives that differ only by the permitted root bytes.

A real two-directory MSVC 14.44 `/Z7 /O1 /Brepro /d1trimfile` single-object
`lib /Brepro` probe with equal-length build roots and relative member names
reproduces the remaining differences: the root in `.debug$S` is normalized and
that section then matches, but the member still differs in (1) the COFF header
TimeDateStamp, which /Brepro derives from content that includes the build root;
(2) `.debug$T`, whose LF_STRING_ID record holds `<build root>\predefined C++`;
and (3) `.chks64`, the per-section checksums. When lib.exe receives absolute
object paths, the member names in the `//` long-name table also carry the root.
None of these is inside OD-24's scope.

The ordinary temporary-directory suites cannot fully execute here: the sandbox
returns Access denied on nested temporary paths and their cleanup. The CPU
configuration suite also skips host-dependent checks because `platform.machine()`
is empty in this shell. No engine CMake build, full engine image scan, Windows
entry-point rebuild, Linux suite, below-floor hardware run, hosted Windows job
or exact-commit CI was performed. No work-item JSON, generated handoff, owner
decision or fuzz-policy file was edited.

## 2026-09-29 libsodium floor and per-source flag scan (local lane)

**libsodium on MSVC.** The MSVC branch of `cmake/SparkLibsodium.cmake` now
compiles no libsodium variant above the OD-04 floor, which matches the non-MSVC
branch. On MSVC x64, `private/common.h` (lines 240-260 at the pinned revision)
defines `HAVE_AVXINTRIN_H`, `HAVE_WMMINTRIN_H`, `HAVE_AVX2INTRIN_H` and
`HAVE_AVX512FINTRIN_H` unconditionally, so a compile definition cannot turn them
off. Every `spark_sodium` source now force-includes (`/FI`) a generated
`spark_sodium_cpu_floor.h`. That header includes `common.h`, whose include guard
makes each source's own include a no-op, and then undefines the four macros. As
a result the AVX, AVX2, AVX-512 and AES-NI/PCLMUL implementation files compile
empty, the dispatchers never select them, and `runtime.c` reports none of them.
ChaCha20 keeps its SSSE3 path and argon2 keeps its SSSE3 fill-block path. The
engine uses no AES-GCM, AEGIS or ipcrypt.

`TEST(CpuFloor_Libsodium_AboveFloorVariantsExcluded)` in
`Tests/TestNET100Libsodium.cpp`, registered as CTest `CpuFloor_LibsodiumVariants`
(exact count 1), asserts that after `sodium_init()` libsodium reports no AVX,
AVX2, AVX-512F, AES-NI, PCLMUL or RDRAND, that `crypto_aead_aes256gcm_is_available()`
is 0, and that SSE2 is 1. Before the change it failed 6 assertions on this
lane's Ryzen 9 9900X3D MSVC 14.44 build (the lane's earlier RED run). After the
change it passes on MSVC windows-release. It also passes on WSL
linux-gcc-release (GCC 14.3, `ENABLE_LTO=OFF`), whose non-MSVC branch never
compiled these variants; `CpuFloor_IsaBaseline` (ELF) passes there too.
`NetworkSecurity_*` (11 CTests,
including the libsodium RFC 8439 vector, handshake and tamper cases) and
`Tests.Tools.test_network_security_csprng` (10 cases) still pass on MSVC. The
test only has force on a host with AVX2 or AES-NI, which every local and hosted
runner has.

PE ISA scan of the rebuilt windows-release images (ENABLE_LTO=ON, MSVC 14.44,
`tools/check_isa_baseline.py --pdb`), compared with the 2026-09-28 numbers:

| Image | 2026-09-28 | 2026-09-29 | Residual by PDB module |
|---|---|---|---|
| SparkServer.exe | 1098 | 152 | vector_algorithms.obj 116; NetworkEncryption.obj 16; GatewayAreaControl.obj 13; PasswordHash.obj 4; ServerApplication.obj 3 (LZCNT) |
| SparkEngine.exe | 1361 | 415 | vector_algorithms.obj 355; NetworkEncryption.obj 16; cgltf_impl.obj 12; SparkEngineWindows.obj 13; SparkEngineWindowsHeadless.obj 4 (LZCNT); rest below 4 each |
| SparkEditor.exe | 1356 (1 AVX-512) | 409 (0 AVX-512) | vector_algorithms.obj 351; NetworkEncryption.obj 16; cgltf_impl.obj 12; EditorProcessLaunch.obj 8; PasswordHash.obj 4; others 1-3 each |
| SparkGame.dll | 1030 | 84 | vector_algorithms.obj 76; ShowcaseLocalization.obj 5; SaveSystem.obj 2 and GameplayShowcase.obj 1 (LZCNT) |

No residual instruction maps to a `spark_sodium` module. The 16 VEX
instructions in NetworkEncryption.obj and the 4 in PasswordHash.obj are now
MSVC's own auto-vectorized `vpsrlvq`/`vpsllvq` paths. They are guarded by
`cmp $5, __isa_available; jl` (inspected at 0x14013f040 in SparkServer.exe),
which puts them in the same class as the inline `__isa_available` dispatch
described on 2026-09-28. They are not libsodium code. The single "FMA" in
SparkEditor.exe (InspectorPanel.obj) decodes as `vfnmadd132ph (%r30), %xmm28`,
which uses APX/AVX-512 registers. It is probably data decoded as code, but this
has not been confirmed. Because the other residual classes remain,
`CpuFloor_IsaBaseline` stays ELF-only and the criterion stays unmet for Windows.

**Per-source-file flags.** `spark_assert_cpu_floor()` now also reads the
`COMPILE_OPTIONS`, `COMPILE_FLAGS` and `COMPILE_DEFINITIONS` of every source of
every non-INTERFACE target. It reads them in the target's directory scope
(`TARGET_DIRECTORY`), skips generator-expression entries such as
`$<TARGET_OBJECTS:...>`, and reports `<target> <source> <PROP>: <flag>`.
`Tests/Tools/test_cpu_floor.py` gains six real-configuration cases: options,
flags, definitions, a property set from another directory, floor-level flags
with a `$<TARGET_OBJECTS>` entry, and the `SPARK_NATIVE_ARCH=ON` stand-down.
All six fail against the previous module, and the whole suite (16 cases) passes.
The real trees still configure. windows-release scans 86 targets and 3936
target sources, windows-shipping scans 33 targets and 2302 sources, and WSL
linux-gcc-release (GCC 14.3) scans 94 targets and 4072 sources. No ThirdParty source carries an above-floor per-source flag. Under
`--profiling-output` the whole `spark_assert_cpu_floor()` call took 1.9 s of a
156 s windows-release reconfigure.

Not done: no hosted run, no MinSizeRel windows-shipping image scan, and no run
on below-floor hardware or an emulator. No work-item JSON was edited.

## 2026-09-30 port review

The eight existing lane commits are present on `rel/w8-bld100-gov400`, based on
`aecd0356343c20535c59807c9438c0409f9a34d1`. The libsodium force-include and its
vendored macro/dispatch guards were reviewed without a C++ build. The earlier
build measurements above are historical evidence, not validation of this port.

The inventory was regenerated against the new base and is byte-identical to
the checked-in result. Jolt's declaration moved from line 1771 to 1770 and
AngelScript's from 1052 to 1051; their reviewed command bodies and condition
blocks remain byte-identical, with only the corresponding line pins changed.

The configure-time source-property scan still skips generator-expression
source entries. `$<TARGET_OBJECTS:...>` is covered through its owning target;
other conditional source expressions are a remaining inspection gap. Do not
treat this scan alone as proof that every compiled object meets the floor.
The Python CMake-fixture suite is pending because temporary fixture directories
are inaccessible in this sandbox. MSVC `spark_sodium`/`SparkTests` builds,
runtime tests and exact-commit CI remain for the integrating session. No
readiness status was promoted.
