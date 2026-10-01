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

STRIP_DEBUG_SYMBOLS now keeps symbols out of the runtime package instead of never producing them. MSVC images always link with /DEBUG (/PDBALTPATH:%_PDB% outside Debug), so each carries an RSDS PDB GUID/age with a bare PDB name. ELF images link with -Wl,--build-id=sha1. With STRIP_DEBUG_SYMBOLS=ON every target compiles with -g (the static libraries hold most shipped code), and only the SPARK_SHIPPED_IMAGE_TARGETS get cmake/SparkSplitDebugLink.cmake as their per-target C/C++ LINKER_LAUNCHER; it splits <image>.debug off and strips the image with a .gnu_debuglink inside the link step, so module .sparkabi hashes see the stripped image. Every other image (SparkTests, test probes) links outside Debug with -g0 and -Wl,--strip-all and gets no .debug; under GCC 13 LTO a link-time -g0 stops LTRANS debug generation, which a probe confirmed (compile -g propagates through -flto unless the link says -g0). The global MinSizeRel -Wl,--strip-all is skipped in that mode, because it discarded the symbols before the split. PDBs and .debug files of SPARK_SHIPPED_IMAGE_TARGETS install only into a new symbols component that CPACK_COMPONENTS_ALL excludes. tools/shipping_symbol_manifest.py (stdlib only) writes a closed spark.shipping-symbol-manifest/1 JSON mapping each ELF image by build-id, debuglink name and CRC-32, and each PE image by RSDS GUID plus DBI age, to exactly one symbol file. It fails on missing, duplicate, orphaned or mismatched symbols, unstripped images, absolute PDB paths and symbols in the runtime tree. CTest ShippingManifest_SymbolManifestTool (30 cases) proves it on gcc fixtures built through the production launcher and on clang/lld-link PE+PDB fixtures cross-checked with llvm-readobj and llvm-pdbutil. It also keeps the target list in step with every install(TARGETS) rule in the root, Spark*/, GameModules/ and cmake/ CMake files, and rejects any CPACK_COMPONENTS_ALL list (root or cmake/SparkCPackOptions.cmake) that names the symbols component. ShippingManifest_PrivateSymbols, registered in STRIP_DEBUG_SYMBOLS ELF trees, installs the runtime/tools/samples and symbols components to separate roots and maps every installed image; it needs every installed target built. In a local linux-shipping MinSizeRel tree with ENABLE_LTO=OFF (SparkGameFPS), exactly the 18 shipped targets link through the split launcher, SparkBuildDownloaderTests and SparkBuildProcessRunnerTests link with -g0 and --strip-all and get no .debug, and ShippingManifest_PrivateSymbols passes: 18 installed images (libSparkGameFPS.so installs to bin and lib) map to their installed symbol files, and none keeps .debug_info or .symtab. The module .sparkabi hash matches the stripped module. In a preset-exact linux-shipping tree (ENABLE_LTO=ON, BUILD_TESTS=ON) only SparkDaemon, SparkOrchestrator and SparkDaemonServiceTests were built: both daemons map to DWARF .debug files and the unshipped test image is stripped with no .debug. The full LTO build with -g of SparkEngineLib and SparkTests, which the telemetry, security-runtime and network-integration jobs perform, has not been built locally, so its disk, memory and time on hosted runners are unmeasured; all SparkEngine-scale evidence here is LTO-off. A 30-frame headless SparkEngine run exits 0, and addr2line resolves main through SparkEngine.debug. build-windows-shipping now stages the components, runs the manifest tool and uploads the PDBs and manifest as a separate shipping-symbols-<sha> artifact. No hosted MSVC build of this change, no /Brepro PDB determinism evidence and no symbol-server publication (OPS-100) exist yet. The ShippingManifest_* selector covers only the private-symbol manifest: the exact MSVC, Windows SDK, dependency, CPU and configuration manifests and the two-clean-build comparison have no tests.

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

## 2026-09-29 Windows ISA scan: contributions, guard dominance, XSAVE/TSX/EVEX (local lane)

`tools/check_isa_baseline.py` gained four reviewed, feature-scoped mechanisms.
Each fails closed outside its exact scope.

1. **vector_algorithms section contributions.** The scanner reads the DBI
   section-contribution stream (`llvm-pdbutil dump --modules --section-contribs`).
   It exempts the code contributions of `vector_algorithms.obj` for AVX, AVX2
   and LZCNT only. The module must carry its exact Microsoft build path (the
   `/MT`, `/MD` or `/MDd` variant) and its PDB `Obj:` record must name a library
   under `MSVC\14.44.35207\lib\x64`. Unknown module indices, zero sizes and
   out-of-range or non-executable sections are errors.
   - The review covered the whole source of MSVC 14.44.35207
     `crt/src/stl/vector_algorithms.cpp`. The file has 372 `_mm256` uses and
     four `_lzcnt_u32` sites (2653, 2667, 3261, 3272), and every one is
     dominated by `_Use_avx2()` (line 26). The checker cites the dispatch
     lines. The file has no FMA, F16C, BMI or AVX-512 intrinsic.
   - The review cannot see code the compiler adds. The shipped object also
     holds auto-vectorized EVEX loops (`vpminuq xmm`, in `__std_minmax_disp`)
     behind `cmpl $0x6, __isa_available`. These stay AVX-512 violations.
   - The per-procedure vector symbol list is deleted because the contributions
     subsume it.
2. **Guard dominance for inline CRT/STL code.** The scanner collects every
   `S_GPROC32`/`S_LPROC32` extent and resolves guard addresses from
   `S_PUB32`. It builds an instruction-level CFG for each procedure that
   contains an uncovered finding. An instruction is exempt when it is
   reachable from the entry and unreachable once the guard edges that
   authorize its feature are removed. Edge conditions are evaluated as signed
   32-bit interval sets. The reviewed guards are:
   - `_Avx2WmemEnabled`/`_Avx2WmemEnabledWeakValue != 0`, which authorizes
     AVX/AVX2. Source: Windows SDK 10.0.26100.0 `ucrt/wchar.h`:207-214, 274
     and 401.
   - `__isa_available >= 5`, which authorizes LZCNT only. Source:
     `__msvc_bit_utils.hpp`:115-127.

   The real MSVC `wmemcmp` compares the guard against a register it zeroed
   with `xorl %eax, %eax` earlier in the same block. That exact shape is
   accepted when only mov/lea instructions that do not write the register
   sit between. The analysis fails closed on indirect jumps, a branch target
   between the compare and the jcc, other compare shapes, missing or
   ambiguous guard symbols, guards in callers and code outside every
   procedure record.
3. **XSAVE exemptions.** XGETBV is exempt in vcruntime `__isa_available_init`,
   via the exact `cpu_disp.obj` pair for `md` and `xmd` from 14.44.35207
   `msvcrt(d).lib`. The source is not shipped. The disassembly shows the one
   XGETBV only after `bt $0x1b` on CPUID.1 ECX (OSXSAVE). XGETBV is also
   exempt in `Spark::Detail::ReadXcr0`, which is now `noinline`, with the
   exact name matched in the PDB and in the ELF symbol. The name exemption is
   dropped when `/OPT:ICF` folds another procedure onto the same bytes.
4. **Classifier.** The classifier adds these families: XSAVE (`xsave*`,
   `xrstor*`, `xgetbv`, `xsetbv`), TSX-RTM, FSGSBASE, SSE4a, 3DNow!,
   CLFLUSHOPT/CLWB, RDPID, WAITPKG, MOVDIRI/MOVDIR64B, SERIALIZE, PKU and AMX.
   HLE prefixes are still stripped. The scanner now reads raw instruction
   bytes, so an EVEX-encoded xmm instruction is AVX-512 and never passes as
   AVX. Before this change MSVC's `vpmaxuq %xmm` was reported as "AVX (VEX)",
   which a module range or AVX guard would have excused. Undecodable bytes
   (`<unknown>`, `(bad)`) are no longer counted as instructions.

**Tests.** `Tests/Tools/test_isa_guard_dominance.py` (new, 17 cases) and the
expanded `test_pe_isa_ranges.py` (16 cases) use synthetic `llvm-objdump` and
`llvm-pdbutil` text, so they run on every host. `test_check_isa_baseline.py`
(26 cases) adds:

- classifier, EVEX and undecodable-byte cases;
- `xgetbv` and `-mrtm` object fixtures;
- ReadXcr0 name-scoping fixtures, where AVX2 inside ReadXcr0 still fails;
- lld-link PE fixtures linked from an llvm-lib `msvcprt.lib` with the MSVC
  module path. A neighbouring module, another toolset (14.45) and FMA are not
  exempt, and XGETBV is exempt only in `Spark::Detail::ReadXcr0`.

`CpuFloor_IsaBaselineChecker` now runs all three modules. On Windows all 59
pass. The same tests run against the 256603c checker give 36 failures and 1
error, and the two synthetic modules fail to import (their API did not exist).

**Real-image measurement.** These are the local windows-release images
(Release, LTO on, built before this lane's `noinline` change) scanned with
`llvm-objdump`. GNU objdump gave the same counts for SparkServer.

| Image | Before (256603c) | After | Allowed after (AVX / ymm / LZCNT / XSAVE) |
|---|---|---|---|
| SparkServer.exe | 1098 | 968 | 43 / 93 / 7 / 1 |
| SparkEngine.exe | 1361 | 988 | 114 / 262 / 10 / 1 |
| SparkEditor.exe | 1356 | 994 | 102 / 267 / 9 / 1 |
| SparkGame.dll | 1030 | 946 | 26 / 55 / 3 / 1 |

In SparkServer, the 116 vector_algorithms instructions without a procedure
record are now under "allowed (cpuid-dispatched)". So are the 13 inline
`wmemchr`/`wmemcmp` instructions (GatewayAreaControl.obj) and the 3 inline
`lzcnt` from `<bit>` (ServerApplication.obj, EnTT). Some findings are new
because XSAVE is now classified: `Spark::DetectCpuFeatures` and `sodium_init`
in each executable.

Residual per image, none of which was exempted:

- **libsodium (every image).** The count is 946. It covers
  `salsa20_encrypt_bytes` (358 ymm + 2 VEX), `chacha20_encrypt_bytes`
  (342 ymm + 2 VEX) and the AES-NI aegis128l/aegis256 routines (242). The
  executables also carry LTCG-inlined AVX2 `vpsrlvq`/`vpsllvq` in
  `SecureChannel::Seal`/`Open` (16, 4 in `PasswordHash` for Server and Editor)
  and XGETBV in `sodium_init`. All of this comes from the MSVC branch of
  `cmake/SparkLibsodium.cmake`, which BLD100-ISA-1 owns; this lane does not
  edit that file.
- **`Spark::DetectCpuFeatures` XGETBV (the three executables).** These images
  predate the `noinline` commit. A 14.44 `/O2 /MD` probe of `Utils/MultiISA.h`
  scans OK with XSAVE allowed twice (ReadXcr0 and `__isa_available_init`).
  GCC `-flto` and Clang keep `Spark::Detail::ReadXcr0()` out of line. No
  engine image was rebuilt on Windows in this lane.
- **MSVC auto-vectorizer AVX-512 dispatch.** There are 12 in
  `__std_minmax_disp<1,_Minmax_traits_8,0>` (vector_algorithms.obj, Engine
  and Editor) and 12 in `cgltf_calc_index_bound` (Engine and Editor). Both
  are EVEX `vpminuq`/`vpmaxuq xmm` behind `cmpl $0x6, __isa_available; jl`.
  Level 6 is set by `__isa_available_init` only with AVX512F/DQ/CD/BW/VL and
  XCR0 opmask/ZMM state. Exempting them needs a reviewed
  `__isa_available >= 6` guard, and the classifier cannot yet tell AVX-512
  subsets apart. An alternative is a build change that stops the dispatch.
  Both need a decision; neither was made here.
- **Switch tables in `.text` (Editor).** One AVX-512 finding in
  `VisualScriptEmitter::EmitNode` is jump-table data: the instruction at
  `0x14041b956` loads `0x41c724(%rdx,%rax,4)`, and the "instruction" bytes
  are table RVAs. One VEX finding in `nlohmann::json::escape_string` looks
  like the same case but was not verified. Handling data in code needs a
  jump-table-aware disassembly and was not attempted.

Native MSVC 14.44 probes, built with `cl /O2 /Zi` with `/MD` and with `/MDd`,
call `std::countl_zero`, `wmemchr`/`wmemcmp` and `std::reverse` and
`std::max_element`. Both pass apart from one out-of-line
`std::_Countl_zero_lzcnt<unsigned __int64>`. That is the "guard in caller"
shape, which stays a residual by design. The `/MDd` probe confirmed the
`xmd` module paths for `vector_algorithms.obj` and `cpu_disp.obj`.

**BLD100-ISA-6 (register `CpuFloor_IsaBaseline` for PE) was not done.** The
residuals above remain, so the test was not registered, narrowed or given
new exemptions. `Tests/CMakeLists.txt` keeps the ELF-only registration, with
the comment updated. No windows-shipping or Debug engine tree was scanned.
The Windows scan still has none of the following: hosted evidence, a proof
on SSE4.2-only hardware or an emulator, or a ledger state change.

**ELF.** The classifier and EVEX changes also apply to ELF images. A local
WSL linux-gcc-release build (GCC, LTO on) of this lane's `noinline` commit
covered the 14 images that `CpuFloor_IsaBaseline` registers. The in-tree
checker reports 0 above-floor instructions. The lane-head checker also
reports 0, with one "allowed (cpuid-dispatched) XSAVE" in each of
SparkEngine, SparkEditor and SparkServer: the XGETBV in
`Spark::Detail::ReadXcr0()`, which GCC kept out of line. No other XSAVE, TSX,
EVEX or newer scalar instruction was found. The `MultiISA_CpuFloor_*`
SparkTests were not rebuilt or run. The only change they would see is the
`noinline` attribute. `SparkServer --help` runs with exit status 0.

**clang-tidy.** clang-tidy 18.1.8 used a private copy of the calibrated
Debug/libc++ configuration. It checked the five Linux TUs that include
`Utils/MultiISA.h` (SparkEditor and SparkServer `main.cpp`,
`GameplayLifecycleShared.cpp`, `SparkEngineLinux.cpp`,
`CpuNeuralInference.cpp`). The lane head and 256603c both give 2025
diagnostics with identical per-check counts, so no budget entry changes.

## 2026-09-30 continuation: fail-closed scan and Windows registration

The three lane commits already ported onto `aecd0356343c20535c59807c9438c0409f9a34d1`
were retained. Review of their scanner found failures that synthetic positive
fixtures had missed:

- Undecodable bytes were discarded, including between a guard comparison and
  its branch. They could hide a newer ISA instruction or a flag/control-flow
  change. They now fail the scan and invalidate guard reasoning for that
  procedure. Reports distinguish undecodable bytes from identified ISA findings.
- VAES, VPCLMULQDQ, GFNI and XOP rotations inherited AVX exemptions. They now
  have separate feature classifications. AVX-512 mask operations using k0 are
  classified too. The reviewed XSAVE procedure ranges now authorize XGETBV
  only, not XSETBV or save/restore operations.
- PDB guard offsets were not checked against section bounds. The whole
  four-byte guard must now fit. A real SparkCooker PDB places its zero-valued
  guard in the virtual, zero-filled tail of .data; that is valid data storage,
  while executable ranges still require actual file bytes.
- LLVM emits LOCK as a separate address record. The parser now joins only
  contiguous prefix/instruction bytes, preserving the instruction entry address.
  Orphan prefixes and undecodable following bytes still fail.
- Procedure lookup walked every preceding procedure for instructions in gaps.
  A prefix maximum of extent ends bounds that search without changing overlap
  handling; regression coverage includes both gaps and nested extents.

The new negative regressions were run against the old implementation and failed
before each fix. Two existing expectations were strengthened: VAES is no longer
classified as ordinary AVX, and undecodable records must remain failures rather
than disappearing. No reviewed range, ISA exemption or readiness state was added.

`cmake/SparkIsaBaseline.cmake` registers the Windows MSVC scan from the existing
`SPARK_SHIPPED_IMAGE_TARGETS` inventory, using configuration-specific image/PDB
pairs. It follows the existing distribution-floor applicability predicate.
`CpuFloor_IsaBaseline` is both a custom target and, when `BUILD_TESTS=ON`, a
CTest. The Windows Shipping CI job invokes the custom target even though its
preset disables tests. Missing tools, PDBs or images fail; residual findings
are no longer a reason to omit Windows registration. The generated build-matrix
inventory was regenerated. Configuration-only fixtures use `LANGUAGES NONE`
and capture registration arguments; they do not prove MSVC generator-expression
resolution or a real build of the new custom target.

### Actual Shipping artifact scans

The existing `build/windows-shipping/CMakeCache.txt` identifies this worktree as
its source, MinSizeRel, `SPARK_NATIVE_ARCH=OFF`, `BUILD_TESTS=OFF`, and
`ENABLE_LTO=OFF`. No C++ build or CTest was run during this continuation. These
previously linked artifacts establish local scanner behavior, not fresh
same-commit or default-LTO Shipping qualification.

Every existing product PE in `build/windows-shipping/bin/MinSizeRel` was scanned
with LLVM and its matching PDB. Standalone SparkBuild test executables are not
products in the authoritative shipped-image list. Results from the final scanner:

| Image | Identified above-floor instructions | Undecodable records | Exit |
|---|---:|---:|---:|
| SparkAutomation.exe | 0 | 0 | 0 |
| SparkBuild.exe | 0 | 1 | 1 |
| SparkConsole.exe | 0 | 0 | 0 |
| SparkCooker.exe | 0 | 76 | 1 |
| SparkCrashReporter.exe | 0 | 70 | 1 |
| SparkEditor.exe | 2800 | 1022 | 1 |
| SparkEngine.exe | 2796 | 591 | 1 |
| SparkGameFPS.dll | 959 | 448 | 1 |
| SparkInstaller.exe | 0 | 55 | 1 |
| SparkLauncher.exe | 2 | 205 | 1 |
| SparkShaderCompiler.exe | 0 | 0 | 0 |
| SparkWorker.exe | 0 | 0 | 0 |

Commands, image/PDB/scanner SHA-256 hashes and per-image logs are retained locally
under `build/isa-review/final.json` and `build/isa-review/*.final.log`. Findings
include libsodium AES/AVX variants (including AVX-512 in `fill_block`), LZCNT
helpers and data/instruction ambiguity inside .text. Their counts are decoder
findings, not proof that each byte sequence is executable. Resolving the product
code/build findings and proving code-versus-data boundaries remain necessary;
silently excluding these bytes is not acceptable.

### Verification commands for integration

The focused Python and configuration-only suite passed locally, as did inventory
currentness, standard `validate.py`, and `git diff --check`. `validate.py --docs`
reported four errors for the absent generated `docs/api` root and its references
from `docs/README.md`; the full documentation gate remains unverified here.

Pure Python and configuration-only verification (no C++ compilation):

```powershell
python -B -m unittest Tests.Tools.test_check_isa_baseline.ClassifierTests Tests.Tools.test_pe_isa_ranges Tests.Tools.test_isa_guard_dominance Tests.Tools.test_isa_fail_closed Tests.Tools.test_isa_scan_registration -v
python Tools/buildmatrix/inventory.py --check docs/readiness/build-matrix-inventory.json --output build/isa-review/inventory-check.json
python tools/site-data/validate.py
git diff --check
```

For a RED control, load the original `f7c894bdb` scanner into the test module
without changing the working tree, then run the new negative regressions:

```powershell
python -B -c "import subprocess,unittest; from Tests.Tools.test_check_isa_baseline import checker; old=subprocess.check_output(['git','show','f7c894bdb:tools/check_isa_baseline.py'],text=True); exec(compile(old,checker.__file__,'exec'),checker.__dict__); unittest.main(module='Tests.Tools.test_isa_fail_closed',argv=['red-control'])"
```

The corrected scanner makes that regression module GREEN. The actual product
scan must remain RED until the residuals above are resolved. Definitive Windows
MSVC integration, through the normal vcvars/build-preflight wrapper:

```powershell
cmake --preset windows-shipping -DENABLE_LTO=ON
cmake --build build/windows-shipping --config MinSizeRel --target CpuFloor_IsaBaseline --parallel 1
cmake --preset windows-release -DENABLE_LTO=ON
cmake --build build/windows-release --config Release --target SparkTests CpuFloor_IsaBaseline --parallel 1
ctest --test-dir build/windows-release -C Release -R '^CpuFloor_IsaBaseline(Checker)?$' --output-on-failure
$env:SPARK_TEST_NAME='MultiISA_CpuFloor_'
$env:SPARK_TEST_EXPECT_COUNT='7'
& .\build\windows-release\bin\Release\SparkTests.exe --warn-is-error
$env:SPARK_TEST_NAME=$null
$env:SPARK_TEST_EXPECT_COUNT=$null
```

Shipping has `BUILD_TESTS=OFF`: its verification command is the custom target,
not a CTest invocation that would select nothing. A clean LTO-on rebuild,
compiled fixture suite, below-floor execution and exact-SHA hosted CI remain
unverified. BLD-100 remains open.

## 2026-10-01: Shipping residuals resolved, enforcement re-applied

The residuals that held back the Windows scan were measured again on a fresh
`windows-shipping` MinSizeRel build of this branch's base (079b99131, LTO off,
MSVC 14.44.35207, LLVM 22.1.8 `llvm-objdump`/`llvm-pdbutil`). The base scanner
reported:

| Image | Above-floor | Undecodable |
|---|---:|---:|
| SparkEditor.exe | 29 | 1026 |
| SparkEngine.exe | 25 | 616 |
| SparkGameFPS.dll | 13 | 324 |
| SparkLauncher.exe | 2 | 205 |
| SparkInstaller.exe | 0 | 60 |
| SparkCooker.exe | 0 | 76 |
| SparkCrashReporter.exe | 0 | 70 |
| SparkBuild.exe | 0 | 1 |
| SparkAutomation, SparkConsole, SparkShaderCompiler, SparkWorker | 0 | 0 |

The libsodium AVX2/AES-NI/AVX-512 findings from the 2026-09-30 table were
already gone: that table was taken before `cmake/SparkLibsodium.cmake` stopped
compiling those variants on MSVC. Each remaining class was resolved:

- **Switch tables in `.text` (every undecodable record).** MSVC x64 places
  jump tables (32-bit RVAs) and byte index tables after the procedure body,
  inside the extent the PDB records. The sweep decodes them as undecodable
  bytes or as bogus instructions (one decoded as `vshufps` in
  `Spark::Json::Detail::PrettyImpl`) and can leave the stream out of step.
  `tools/isa_code_map.py` now proves each table before treating its bytes as
  data. The checks: the dispatch idiom with the image base reaching every use;
  the extent from the bound check, or for `std::variant` (no bound, index -1
  slot) from entries that name instruction starts; entries landing on rebuilt
  instructions; no branch into or fallthrough into a table; PDB `noreturn` on
  the calls in front of tables; and a fixed point when the rebuilt stream is
  re-read. Out-of-step stretches are re-decoded with the same disassembler.
  Unexplained bytes stay undecodable. Nothing is exempted by image, procedure
  or range.
- **AVX2 loops behind `__isa_available >= 5`** (auto-vectorized
  `Sha256State::Finalize`): the `__isa_available` guard rule now covers
  AVX/AVX2 as well as LZCNT at level 5.
- **EVEX loops behind `__isa_available >= 6`** (`cgltf_calc_index_bound`,
  `__std_minmax_disp`/`__std_minmax_impl`): a level-6 rule covers only
  `vpmaxuq`/`vpminuq` (AVX512F/VL). In the vcruntime `__isa_available_init`
  disassembly, the store of 6 needs `CPUID.7:EBX & 0xD0030000` (F, DQ, CD, BW,
  VL) and `XCR0 & 0xE0`. Any other EVEX instruction still fails.
- **`std::_Countl_zero_lzcnt<unsigned __int64>` (LZCNT, out of line).** Its
  only reference is `_Checked_x86_x64_countl_zero`'s `cmpl $0x5,
  __isa_available; jge` tail jump. `REVIEWED_CALLER_GUARDED_PROCEDURES` holds
  this one entry. Each scan re-proves it: every code reference is guarded, the
  address is never taken, and no data section holds its VA or RVA.

Result with the new scanner, through the real `CpuFloor_IsaBaseline` target
(`cmake --build build/windows-shipping --config MinSizeRel --target
CpuFloor_IsaBaseline`): `OK` for all 12 images, 0 above-floor, 0 undecodable.
SparkEngine had 162 proven tables (14458 bytes), SparkEditor 218, SparkGameFPS
69 and SparkLauncher 30.

Load-bearing check: a temporary, uncommitted `SparkIsaProbeAvx2` (AVX2
intrinsics, reachable from `wWinMain`) was added to
`Core/SparkEngineWindows.cpp`. The rebuilt target then failed with MSB8066:
`FAIL SparkEngine.exe: ... 5 above-floor`, all five in `SparkIsaProbeAvx2`.
After restoring the file (the object was recompiled), the target passed again.
That probe build also exposed a layout-dependent false table rejection in
`Spark::Net::TrustStoreErrorText`, which is what led to the fixed-point
requirement. `Tests/Tools/test_isa_code_map.py` was mutation-checked: removing
the validation, the base-reach check, the reachable-code check on backward
slots, the case-leader re-match or the caller-guard reasons each fails it.

The enforcement commits 97809e21d and d57694e24 are re-applied. The
windows-shipping CI step and, with `BUILD_TESTS=ON`, the `CpuFloor_IsaBaseline`
CTest are fail-closed again.

Entry points: the Windows engine and editor floor checks now compile in a real
MSVC Shipping build. `SparkEngine.exe --version` runs the passing branch
(exit 0). The refusal branch has still not run. Running it needs a CPU below
the floor, or a CPUID emulator such as Intel SDE (an external download not made
here).

Unverified: hosted CI (the windows-2022 runner's MSVC toolset and LLVM version
may differ from the reviewed 14.44.35207 and the local LLVM 22.1.8; either can
turn the scan red), LTO-on images, the `CpuFloor_IsaBaseline` CTest in a
`windows-release` tree, and below-floor execution.

## 2026-10-01 (cont.): independent review hardening (Codex gpt-6-sol)

An independent review rejected the first cut with four executed synthetic
checks. All four are fixed, each with a regression test built from the
reviewer's case, and the real windows-shipping scan still passes on all 12
images (0 above-floor, 0 undecodable) while the temporary AVX2 probe still
fails.

1. **A switch table could hide code entered from outside its procedure.** The
   branch/fall-through checks were per-procedure, so a cross-procedure tail jump
   (or an address-taken pointer) into a table's bytes was unchecked.
   `check_isa_baseline.py` now computes image-wide code entry points and rejects
   any proven table whose bytes they enter (`isa_code_map.ImageBytes.entered_within`).
   Entries are gathered over the *rebuilt* streams of a first scan pass (so table
   bytes, which decode as bogus branches on a raw linear sweep, do not pollute
   them), plus the PE's own exception handlers, exports, base-relocation pointees
   and guard-CF table. `.pdata` BeginAddress is deliberately excluded: MSVC gives
   a compiler-placed jump table its own RUNTIME_FUNCTION (observed in
   `ImGui::ColorConvertHSVtoRGB` and the UCRT wmem* fragments), so a begin can
   legitimately coincide with table bytes.
2. **Guard dominance assumed the procedure's entry was the only way in.** A tail
   jump from another procedure into a guarded AVX block, or a reliable
   address-taken reference to it, now seeds the reachability analysis as an
   alternate root, so a block reachable without the guard edge stays a
   violation. The caller-guarded helper exemption
   (`std::_Countl_zero_lzcnt`) is withdrawn unless its complete reference set is
   closed: a computed `mov RVA; add imagebase; call` reference, an export, a
   relocation or a guard-CF entry now counts as unguarded.
3. **An unknown VEX mnemonic fell through to plain AVX.** VEX classification now
   uses an explicit allow-list of the AVX/AVX2 mnemonics llvm-objdump prints
   (built from the AVX/AVX2 ISA and every VEX mnemonic in the shipped images);
   anything else fails closed. AVX-VNNI (`vpdpbusd`), AVX-IFMA, AVX-NE-CONVERT
   and the XOP forms get their own feature classes, so the `__isa_available >= 5`
   guard cannot excuse them.
4. **A record without raw bytes disabled rebuilding, and the final check needed
   only one classified instruction.** `_verify_coverage` now requires that the
   classified instructions, proven tables and padding tile every executable
   file-backed byte range with no gap; a record whose size is unknown (no raw
   bytes) leaves a gap and fails.

Scoping note (learned while fixing 1-2): immediate-materialized addresses are
used only for the caller-guard reference check, never to reject tables or seed
guard roots -- a data constant can coincide with a byte inside a real jump table
or a guarded block, and treating every such immediate as a code entry wrongly
rejected ~160 legitimate tables and withdrew the UCRT `wmemcmp` guard on a first
attempt. Only branches and reliable structural pointers enter code.

The MSVC CRT/STL exemptions remain pinned to toolset 14.44.35207; on a PDB built
by a different toolset the report now prints a "re-review for toolset X" note so
a hosted-runner red is diagnosable, without loosening anything.

Regression tests: `Tests/Tools/test_isa_code_map.py` (ExternalEntryTests,
CallerGuardedTests computed/closed-reference cases, CoverageTests),
`Tests/Tools/test_isa_guard_dominance.py` (alternate-entry and
separately-bitted-VEX cases), `Tests/Tools/test_check_isa_baseline.py`
(separately-bitted and unrecognized VEX). Each fix was mutation-checked: removing
it fails its test. The ISA unit suites pass. Still unverified, as before: the
below-floor refusal path, and any hosted Windows run.

## 2026-10-01 (cont.): second independent review hardening (Codex gpt-6-sol)

A second review accepted that the four earlier fixes handle their synthetic
cases but found residual computed control flow and an incomplete toolset pin.
Three further changes, each with a regression test from the reviewer's
synthetic; all 12 windows-shipping images still pass with the same proven-table
counts as before (SparkEngine 162, SparkEditor 218, SparkGameFPS 69,
SparkLauncher 30, SparkInstaller 16, SparkCooker 3, SparkCrashReporter 1,
SparkAutomation 1, SparkBuild 8, SparkShaderCompiler 3; 0 above-floor, 0
undecodable), and the AVX2 probe still fails.

1. **Toolset pinning is now complete.** Previously memcpy/memset were recognised
   by module-suffix alone and the inline guard rules and caller-guarded helper
   did not consult the PDB's toolset, so a PDB marked as another toolset still
   got those exemptions. Every reviewed MSVC CRT/STL exemption is now granted
   only when the PDB's sole observed toolset is 14.44.35207 (`PdbInfo.reviewed_
   toolset`); otherwise the instruction is a violation and the report's
   "re-review for this toolset" note explains the red. The engine's own
   `Spark::Detail::ReadXcr0` XSAVE review is tagged toolset-independent, so the
   lld-link fixture (no CRT) still passes.

2. **Computed control flow is resolved by basic-block dataflow, not by treating
   every immediate as an entry** (which caused the round-1 false positives). The
   collector tracks an immediate image address materialized into a register
   (movabs/mov/lea, constant add/sub/inc/dec, and the mov-RVA-plus-image-base
   idiom) and, when it reaches `jmp *reg` / `call *reg` -- or is stored while the
   procedure has an indirect branch -- records the target as a computed entry.
   A computed entry (a) seeds guard dominance as an alternate root
   (`movabs AVXblock; call *rax` on the scalar path), (b) withdraws the
   caller-guarded exemption (`movabs helper+1; dec; call *rax`), and (c) rejects
   a table whose bytes it enters. Separately, for any materialized image address
   that lands inside proven-table bytes, the bytes are decoded from that offset
   and the table is rejected if they form an **above-floor** instruction before a
   terminator. Undecodable bytes at a merely-pointed offset do not reject the
   table: a byte/jump table's own bytes decode as undecodable garbage (an MSVC
   index table in SparkGameFPS `_On_type` is pointed into by a data constant),
   and a computed *jump* into such bytes is caught independently as an image-wide
   code entry. This is the one deliberate narrowing from the reviewer's "above-
   floor or undecodable" wording, forced by that real legitimate table; it still
   catches the reviewer's `c5 f8 77` (vzeroupper) masked-table synthetic.

3. **Residual limit documented** (threat model: compiler-generated MSVC code from
   this repository, not adversarial binaries). Resolved: direct branches,
   structural pointers (relocs/exports/handlers/guard-CF) and basic-block-local
   computed targets. Not resolved: interprocedural or memory-carried computed
   targets in a fixed-base image. Recorded in the scanner module docstring, here,
   and the BLD-100[3] note. Coverage is every configured first-party image with
   its own build PDB; the Microsoft runtime DLLs packaged under `redist/` have no
   build PDB and are not scanned (Codex round-3 review, ACCEPT-WITH-FIXES).

Each fix was mutation-checked (toolset flag, per-range gate, computed-use,
constant adjust, stored pointer, above-floor decode-check -- removing any fails
its test). Regression tests: `test_isa_guard_dominance` (toolset-provenance and
computed-target guard withdrawal), `test_pe_isa_ranges` (CRT vs engine toolset
gating), `test_isa_code_map` (ComputedTargetTests, TableMasksCodeTests,
caller-guard toolset and computed-reference cases). Still unverified, as before:
the below-floor refusal path, and any hosted Windows run.
