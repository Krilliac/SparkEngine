# GOV-400 Owner Decisions

GOV-400 is the release-readiness work item for licensing, third-party
notices, trademark, contribution, security, and support policy. This memo lists
the decisions only the project owner can make. For each one it gives the
options, the limits the vendored licenses place on those options, and the files
that change once a choice is made.

This memo does not make a decision and it is not legal advice. Where it names a
license duty, it points to the license text on disk. The generated
[`THIRD_PARTY_NOTICES`](../../THIRD_PARTY_NOTICES) reproduces that text, and a
legal reviewer should read the text itself, not this summary.

## What is already in place (no decision needed)

| Item | State | Where |
|---|---|---|
| Repository third-party notices | Generated from `ThirdParty/dependencies.lock`, `ThirdParty/supply-chain.lock`, and tracked license files. `--check` detects stale output. | `THIRD_PARTY_NOTICES`, `tools/governance/generate_third_party_notices.py`, `Tests/Tools/test_third_party_notices.py` |
| Package notices | CMake builds `THIRD_PARTY_NOTICES.txt` from the declared notice files at configure time and installs it with `LICENSE.txt` in runtime and SDK packages | `cmake/SparkThirdPartyAudit.cmake`, `CMakeLists.txt` (`SPARK_THIRD_PARTY_NOTICES_FILE`) |
| Public-wording guard | `tools/site-data/validate.py --legal` rejects unqualified "open source" wording while the declared license is non-OSI | required `license-compliance` CI job |
| Private vulnerability reporting | Enabled on the GitHub repository (checked through the API on 2026-09-23) | `SECURITY.md` |
| Supply-chain exception schema | Implemented: owner, justification, and expiry are enforced | `ThirdParty/POLICY.md` |

The two notice files overlap but are different. The repository file also lists
the gaps below. The packaged file lists only the declared notices. Unifying them
is follow-up work for decision D8.

## Third-party inventory

`THIRD_PARTY_NOTICES` covers the locked components. Every locked component has at least one
notice file on disk. Several have attention items. The editor fonts
outside `ThirdParty/` have their upstream license text on disk (item 4).

| License family (as declared) | Components | Duty the on-disk text states |
|---|---|---|
| MIT | Jolt Physics, EnTT, Dear ImGui, miniz, cgltf, nlohmann/json, tinyobjloader, VulkanMemoryAllocator | Include the copyright notice and permission notice in all copies or substantial portions |
| zlib | RecastNavigation, SDL2, AngelScript | Altered source must be plainly marked as altered. The notice may not be removed from source distributions. Credit in product documentation is optional. |
| BSD-3-Clause | tinyexr (including OpenEXR/ILM portions), zstd | Keep the notice in source. Reproduce it in the documentation or other materials of binary distributions. Do not use the named parties to endorse or promote derived products. |
| Public Domain / MIT (choice) | stb_image | The recipient chooses one alternative. The owner must record which one SparkEngine relies on. |
| Public Domain / MIT-0 (choice) | miniaudio | Same as stb_image. MIT-0 requires no attribution. |
| MIT and Apache-2.0 | glad | The MIT part covers the generator output. Apache-2.0 covers the Khronos specification material: ship a copy of the license and state changes. |
| LGPL-3.0 (undeclared) | `ThirdParty/Physics/JoltPhysics/Assets/Racetracks/` (Zandvoort.csv data from TUMFTM/racetrack-database) | Copyleft terms apply to that data wherever it is distributed |

### Items the generator could not resolve

These are listed in the ATTENTION REQUIRED section of `THIRD_PARTY_NOTICES`.
The generator does not guess any of them.

1. **Repository-authored stubs recorded as upstream snapshots.** The tracked
   headers for miniaudio, nlohmann/json, stb_image and stb_image_write, zstd,
   and VulkanMemoryAllocator describe themselves as minimal stubs, not the
   upstream library. `dependencies.lock` still records upstream versions and
   upstream licenses for them. The zstd stub's header says "BSD + GPLv2 dual
   license", which differs from the lock's `BSD-3-Clause`. The stub also
   writes a format that real zstd cannot read.
2. **Non-SPDX license fields.** The fields for stb_image, miniaudio, zstd, and
   glad are free text, not a single SPDX expression.
3. **Jolt Physics asset tree.** The vendored snapshot includes `Assets/`,
   `Samples/`, `TestFramework/`, and `JoltViewer/` as well as the library.
   `Assets/LICENSE` says the Horizon Zero Dawn assets are released under MIT
   with Guerrilla Games' permission. For `face.bin`, it names a creator but
   gives no license terms. `Assets/Racetracks/LICENSE.txt` is LGPL-3.0.
   `Assets/Fonts/Roboto-Regular.ttf` and `Assets/UI.tga` have no license file.
4. **Editor fonts.** `SparkEditor/Fonts/` holds IBMPlexSans (3 weights),
   JetBrainsMono-Regular, Roboto (2 weights), and fa-solid-900. Its
   `CMakeLists.txt` installs them, with their `LICENSES/` directory, to
   `bin/EditorAssets/Fonts`. `SparkEditor/Fonts/LICENSES/` holds each font's
   upstream license text, committed verbatim, and `fonts.json` maps each font
   to it. The generator checks that the recorded copyright and license match
   the font's own name table: IBM Plex Sans, JetBrains Mono, and Font Awesome
   Free 6.7.2 are OFL-1.1, and Roboto 2.001047 is Apache-2.0. The repository
   `THIRD_PARTY_NOTICES` reproduces these texts. The packaged
   `THIRD_PARTY_NOTICES.txt` reproduces them too: `cmake/SparkThirdPartyAudit.cmake`
   reads `fonts.json` and writes one `<family> (editor font)` entry per license
   text, naming its fonts on the `Files:` line. Rendering fails when
   `fonts.json` is malformed, names a missing font or license text, or misses a
   font in `SparkEditor/Fonts`. The remaining work is the legal review of the
   texts.
5. **Submodule notice copies.** The six submodule notices live in
   `ThirdParty/Licenses/`. A checkout without submodules cannot compare these
   copies with upstream.
6. **Fonts compiled into binaries.** Dear ImGui's `imgui_draw.cpp` embeds two
   fonts as compressed data: ProggyClean.ttf (declared "MIT License /
   Copyright (c) 2004, 2005 Tristan Grimmer") and a minimal ProggyForever
   subset (declared "MIT license / Copyright (c) 2026 Disco Hello, Copyright
   (c) 2019,2023 Tristan Grimmer"); its `docs/FONTS.md` lists both as MIT.
   Every binary that links Dear ImGui carries them: a byte scan of the v0.9
   Windows MinSizeRel package found both in `SparkEditor.exe`,
   `SparkEngine.exe`, `SparkInstaller.exe` and `SparkLauncher.exe`, and its
   notice file named neither. `ThirdParty/Licenses/embedded-fonts.json`
   inventories them, and `ThirdParty/Licenses/ProggyClean-LICENSE.txt` and
   `ProggyForever-LICENSE.txt` reproduce each declaration verbatim with the MIT
   permission notice as Dear ImGui distributes it. The font projects' own
   license files are not vendored, so these texts are assembled from the
   declarations, not copied from upstream; the legal review should confirm
   that reading. The generator scans the embedding source for
   `Default font data (...)` sections and reports any section without an
   entry, and `--require-complete` fails when the source is not checked out.
   The packaged notice names each font on a `Files:` line, and the package
   gate searches shipped binaries for the names Dear ImGui compiles in
   (`embeddedFonts` in `cmake/PackageNoticeCoverageRules.json`).

## Decisions

### D1: Outbound license classification

The root `LICENSE` is the custom, non-OSI "Spark Open License 1.0". GitHub
reports its SPDX ID as `NOASSERTION`.

- **Options:** (a) keep the custom license and go through legal review; (b)
  adopt an OSI-approved license; (c) offer a dual license (for example, a
  custom license plus a commercial license).
- **Limits from vendored code:** every vendored code license (MIT, zlib,
  BSD-3-Clause, MIT-0/public domain, Apache-2.0) is permissive. None of them
  stops SparkEngine from choosing any outbound license, but none of them can
  be relicensed. Their notices must still ship under their own terms.
  Apache-2.0 material (glad/Khronos) is generally regarded as incompatible
  with GPLv2-only terms, so a GPLv2-only choice would conflict. The LGPL-3.0
  racetrack data keeps its own copyleft terms whatever the owner chooses.
- **Files that change:** `LICENSE`; `README.md` (badge and License section);
  `SparkBuild/README.md`; `Templates/README.md`; `docs/site/content.json`
  (`content.legal.license`); `CURRENT_LICENSE_DECLARATION` in
  `tools/site-data/validate.py`; `wiki/Home.md`; `wiki/getting-started/FAQ.md`;
  `.github/copilot-instructions.md`; `.github/prompts/copilot-instructions.md`;
  SPDX headers in first-party sources, if adopted; and the GitHub repository
  license metadata.

### D2: Scope of the root license

This covers third-party code, compiled games, templates, and assets.

- **Options:** add a scope statement to `LICENSE`, or add a separate scope
  document, covering:
  - which paths the root license covers;
  - that `ThirdParty/` components and the editor fonts stay under their own
    licenses;
  - how the root license applies to compiled games, statically linked
    binaries, plugins, templates, and generated assets.
- **Limits:** the conditions in `LICENSE` §2 to §4 (naming, anti-plagiarism,
  attribution) cannot attach to third-party code. The zlib licenses also
  require altered source to be marked. AngelScript is patched through
  `ThirdParty/Scripting/patches/`, so the owner must decide how to mark that
  altered source.
- **Files that change:** `LICENSE` or a new scope document; the header text of
  `THIRD_PARTY_NOTICES` (through the generator); `docs/site/content.json`
  (the `policyGaps` entry "Compiled-game, static-linking, plugin, asset, and
  template license interpretation").

### D3: Trademark and naming policy

`LICENSE` §2 already restricts use of the names "SparkEngine", "Spark
Engine", and "SparkEditor" and of the logo. No separate trademark or
logo-use policy exists.

- **Options:** (a) rely on `LICENSE` §2 only; (b) publish a trademark and
  logo-use policy (a new file such as `TRADEMARKS.md`); (c) register the
  marks (outside the repository).
- **Limits:** the BSD-3-Clause texts for zstd and tinyexr forbid using Meta,
  Facebook, Industrial Light & Magic, Syoyo Fujita, or their contributors to
  endorse or promote derived products. Marketing copy and a trademark policy
  must not suggest such endorsement.
- **Files that change:** the new policy file; `LICENSE` §2 (if the policy
  becomes the reference); `README.md`; `docs/site/content.json` (the
  `policyGaps` entry "Full trademark and logo-use policy").

### D4: Inbound contribution model

`CONTRIBUTING.md` currently states no inbound terms.

- **Options:** (a) inbound=outbound (contributions under the root license);
  (b) Developer Certificate of Origin (DCO) with `Signed-off-by` enforcement;
  (c) Contributor License Agreement (CLA).
- **Limits:** D1 decides whether relicensing is possible later. Under options
  (a) and (b), relicensing contributed code needs every contributor's
  consent. Under option (c), it depends on the CLA's grant. Contributions that
  touch `ThirdParty/` stay under the upstream license. Jolt Physics ships its
  own `ContributorAgreement.md`, which governs upstream contributions only.
- **Files that change:** `CONTRIBUTING.md` (a new section on contribution
  terms); a DCO or CLA check workflow under `.github/workflows/` and its
  required-gate wiring; a PR template, if one is added; `docs/site/content.json`
  (the `policyGaps` entry "Inbound contribution licensing model").

### D5: Security contact and response commitments

- **Current state:** GitHub private vulnerability reporting (enabled), with
  best-effort handling and no timelines. No versioned release exists, so the
  "Supported Versions" table has no supported version.
- **Options:** (a) keep best-effort handling; (b) publish acknowledgment,
  triage, and fix targets; (c) add a second private channel (a dedicated
  address) alongside GitHub advisories; (d) define supported-version rules
  (for example, latest release only) that start at the first tag.
- **Limits:** SECURITY.md sends reports about third-party vulnerabilities
  upstream. Any fix commitment for vendored code depends on upstream releases
  and on a lock update (the SEC-110 lane).
- **Files that change:** `SECURITY.md` (Supported Versions, Reporting, and
  Response Expectations); `docs/site/content.json` (`securityCaution`; the
  `security` document status, now `review-required`; and the `policyGaps`
  entries "Security support scope and approved response commitments" and
  "Copyright and takedown process with a designated contact").

### D6: Support policy

- **Current state:** the repository has no `SUPPORT.md`. Issues and
  Discussions are both enabled on GitHub. `CONTRIBUTING.md` sends bug reports
  to Issues.
- **Options:** (a) community support through Issues and Discussions,
  best-effort; (b) name the supported channels and the kinds of question each
  one takes; (c) paid or commercial support.
- **Limits:** none come from third-party licenses. Any statement must match
  the release channels that actually exist (see `SECURITY.md`).
- **Files that change:** a new `SUPPORT.md`; `README.md`; `CONTRIBUTING.md`
  (Reporting Issues); `docs/site/content.json` (the forum and operator-contact
  `policyGaps` entries, if the forum is covered).

### D7: Code of Conduct enforcement

- **Current state:** `CODE_OF_CONDUCT.md` is an abridged Contributor Covenant
  2.1. Reports go to "the project maintainers", with no contact and no
  enforcement ladder.
- **Options:** (a) name a reporting contact and adopt the upstream
  enforcement guidelines; (b) name a contact and write a custom process.
- **Limits:** the Contributor Covenant is licensed under CC BY 4.0, so keep
  the attribution section in any adaptation.
- **Files that change:** `CODE_OF_CONDUCT.md`; `docs/site/content.json` (the
  forum moderation and appeals entries in `policyGaps`, if they share one
  process).

### D8: Third-party remediation

These choices change `ThirdParty/` or its locks, which the SEC-110 lane owns.

- **Options for the stubs:** (a) replace each stub with the real upstream
  file at the locked version; (b) reclassify the stubs as first-party
  (`project_owned_dirs`, with a justification) and decide whether to keep an
  upstream attribution for the API surface they imitate.
- **Options for the Jolt snapshot:** (a) trim the snapshot to the library
  (`Jolt/`, `Build/`, `LICENSE`), which removes the LGPL-3.0 data and the
  assets that have no license; (b) keep it and declare each extra license
  file in `dependencies.lock`.
- **Editor fonts:** done. The upstream license texts are next to the fonts in
  `SparkEditor/Fonts/LICENSES/` (item 4), and the packaged notice file names
  each font on a `Files:` line of an entry that reproduces its license text,
  generated from `fonts.json` rather than duplicated into `dependencies.lock`.
  The staged-package gate (`cmake/ValidateStagedPackageNotices.cmake`, rules in
  `cmake/PackageNoticeCoverageRules.json`, also run by
  `generate_third_party_notices.py --check-package <install root>`) covers all 7
  fonts, and `ValidateStagedPackageExecutables.cmake` now runs it in `enforce`
  mode by default. Before the flip, a path mirror of a local MSVC
  `windows-shipping` install reported only the 7 fonts as uncovered, and none
  once the fonts were added. A Linux install was measured on 2026-09-28 at
  commit `90106eb62`: `linux-gcc-release`, GCC 14.3, `BUILD_TESTS=OFF`,
  `ENABLE_LTO=OFF`, Ubuntu 26.04 under WSL2, `cmake --install` into a scratch
  prefix. Both checks passed with no rule change:
  `ValidateStagedPackageNotices.cmake` and `--check-package` both reported
  complete notice coverage for the staged fonts and third-party payload. A probe file added under `include/SparkEngine/ThirdParty/` made
  both fail and name it, so the pass is not vacuous. This measures coverage
  only. The stub, Jolt snapshot and SPDX items above remain open owner
  choices, and the CPack archive itself was not run through the gate.
- **Fonts compiled into binaries:** repository side done (item 6). Both
  implementations of the package gate were run on a package made from the
  four v0.9 shipping executables and the editor fonts: with the notice file
  rendered before this change they reported 8 uncovered (binary, font) pairs,
  and with the current rendering they passed with 8 embedded fonts counted.
  The legal review of the assembled license texts remains (OD-28).
- **Microsoft Visual C++ runtime (new legal-review item).** Every MSVC package
  ships `vcruntime140*.dll`, `msvcp140*.dll`, `concrt140.dll` and the rest of
  `CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS` app-local in `bin/` (the `redist`
  component, ENG-220). Until 2026-09-29 no notice rule matched them, so both
  gate implementations presumed these Microsoft binaries first-party; that is
  why the Windows path mirror above reported only the fonts. The rule set now
  has a `systemRuntime` rule for them. It requires a "Microsoft Visual C++
  Runtime" inventory entry whose `Files:` line names each shipped DLL and whose
  `Terms:` line names the governing terms. `spark_thirdparty_generate_notice()`
  writes that entry from the DLL basenames and the compiler version. The
  Distributable Code terms are not a file in the repository, so the entry names
  them but does not reproduce them. This identifies the license; it does not
  interpret it. **Owner/legal decision:** whether the `Terms:` wording is
  adequate, whether the terms must be reproduced or linked, and whether shipping
  the DLLs app-local (rather than the Microsoft redistributable installer)
  satisfies them.
- **Closed-world classification.** The gate used to be open-world. It checked
  only files under the third-party roots or matching a payload rule, and
  presumed every other installed file first-party. `SPARK_PACKAGE_NOTICE_CLASSIFICATION=closed`
  (CMake) and `--closed-world` (Python) remove that presumption. Every other
  file must then be either listed with an identified license (not
  `NOASSERTION`) in the installed RDY-020 asset manifest
  (`assetManifests`: `bin/Assets/assets.integrity.json`), or match a
  `firstPartyRoots` pattern that carries a written justification. Anything
  else fails as `unclassified`. A shipped license text (the editor fonts'
  `LICENSES/*.txt`) matches a `noticeText` rule instead. It is covered only
  when its name is on the `Notice files:` line of an inventory entry whose
  texts are reproduced. Open world stays the default for the existing
  staged-package callers (`ValidateStagedPackageExecutables.cmake`, the release
  workflow and its fixtures). `LicenseInventory_InstallTreeNoticeCoverage`
  installs the `CPACK_COMPONENTS_ALL` components of a real build into a fresh
  prefix. It runs both implementations closed-world and requires identical,
  non-zero counts, with every installed file classified. The one permitted
  residual is the set of assets whose installed manifest license is
  `NOASSERTION`. OD-09 excludes those only from the stable-v1 package, and the
  default package still ships them. Both gates must report exactly that set
  for exactly that reason. The files are then removed, as the stable-v1
  profile removes them, and the rest of the tree must pass. A stable-v1 tree
  has no such file, so it must pass as installed. Two probe files, an
  unmapped header under `include/SparkEngine/ThirdParty/` and an unclassified
  root file, must then make both implementations fail by name.
  Measurements: see "Install-tree measurements (2026-09-29)" below. Still open:
  switching the staged-package gate itself to closed world, the CPack archive
  and native installers, and the stub/upstream license identity (item 1: the
  zstd stub header's "BSD + GPLv2" against the lock's BSD-3-Clause). The last is
  an owner/legal decision, so this criterion can at most be `implemented` for
  coverage.
- **SPDX fields:** rewrite the free-text `license` fields as SPDX expressions
  once the choices in the inventory table are made.
- **Files that change:** `ThirdParty/dependencies.lock`;
  `ThirdParty/supply-chain.lock` (through `tools/check-supply-chain.py
  --update`); `ThirdParty/**` payload; `cmake/SparkThirdPartyAudit.cmake` (if packaged notices must include
  extra notices, or should reuse the repository generator);
  `THIRD_PARTY_NOTICES` (regenerated).

#### Install-tree measurements (2026-09-29)

Both runs are `LicenseInventory_InstallTreeNoticeCoverage` on local builds of
this lane, and neither is hosted CI evidence.

| Host | Build | Installed | NOASSERTION residual | Fonts | Third-party payload | First-party | Asset manifest |
|---|---|---|---|---|---|---|---|
| Windows 11 | MSVC 14.44, `windows-release` (Release, LTO on) | 2841 | 475 | 7 | 496 (8 MSVC runtime DLLs among them) | 1112 | 751 |
| WSL2 Ubuntu | GCC 14.3, `linux-gcc-release`, `ENABLE_LTO=OFF` | 2920 | 475 | 7 | 585 | 1102 | 751 |

In both runs the CMake and Python gates agreed on every count. After the
residual was removed, every installed file was classified (2366 of 2366 on
Windows and 2445 of 2445 on Linux), and both probe files failed both gates by
name. The Windows components were `runtime;sdk;tools;templates;samples;redist`
and the Linux components were the same without `redist`.

The first Windows run failed, and it found real gaps:

- the editor fonts' license texts, now covered by the `noticeText` rule;
- `fonts.json`, the playtest launcher and its instructions, the asset
  pipeline headers and the example configs, now covered by justified
  first-party roots;
- the NOASSERTION assets, now the explicit residual described above.

RED check: with the `^tools/` first-party root removed, the Windows run failed
and named 67 `tools/` files as unclassified. The 8 runtime DLLs covered were
`concrt140`, `msvcp140`, `msvcp140_1`, `msvcp140_2`, `msvcp140_atomic_wait`,
`msvcp140_codecvt_ids`, `vcruntime140` and `vcruntime140_1`.

Not measured: a stable-v1 (`SparkGameFPS`) tree with tests enabled, the CPack
archive and the native installers.

#### Port review (2026-09-30)

The manifest readers now parse JSON independently of key order and whitespace,
reject duplicate paths and malformed manifests, and treat non-string, blank,
`NONE` and `NOASSERTION` license values as unidentified. The CMake reader builds
a path-indexed map once instead of searching for license-looking text.

Script-only fixtures reproduced the previous CMake acceptance of malformed
JSON, both readers' acceptance of duplicate paths, and the Python reader's
coercion of null/numeric/container license fields into strings. The corrected
readers agree on all twelve fixtures, including compact and reordered valid
JSON.

The repository notice currentness/completeness check passes. The full Python
suite is pending because this sandbox denies access inside temporary fixture
directories. No C++ build, CTest, real install-tree run, or hosted evidence was
produced for this port review. The default package still ships 475
`NOASSERTION` assets (OD-09); the install-tree test keeps them as the exact
accounted residual described above rather than failing the default
windows-release CTest run that `build-windows-vs2022` executes. No exception or
readiness promotion was added.

## Regenerating and checking the notices

```bash
python tools/governance/generate_third_party_notices.py            # write THIRD_PARTY_NOTICES
python tools/governance/generate_third_party_notices.py --check    # exit 1 if stale
python tools/governance/generate_third_party_notices.py --require-complete  # exit 1 if a locked component or a font has no notice, or an embedding source is not checked out
python -m pytest Tests/Tools/test_third_party_notices.py
```

Regenerate `THIRD_PARTY_NOTICES` after any change to the two lockfiles or to
a tracked license file under `ThirdParty/`.
