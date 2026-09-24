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

`THIRD_PARTY_NOTICES` covers 16 locked components. All 16 have at least one
notice file on disk. Seven of them have attention items. Seven tracked font
files outside `ThirdParty/` have no license file at all.

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
   `CMakeLists.txt` installs them to `bin/EditorAssets/Fonts`. The repository
   has no license file for any of them, and neither notice file names them.
5. **Submodule notice copies.** The six submodule notices live in
   `ThirdParty/Licenses/`. A checkout without submodules cannot compare these
   copies with upstream.

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
- **Editor fonts:** get each font's license text from its upstream
  distribution (do not supply it from memory), add the texts next to the
  fonts, and give them an inventory entry so both notice files include them.
  List the font file names in the entry's required files: the packaged notice
  prints them on a `Files:` line, and the staged-package gate
  (`cmake/ValidateStagedPackageNotices.cmake`, rules in
  `cmake/PackageNoticeCoverageRules.json`, also run by
  `generate_third_party_notices.py --check-package <install root>`) covers a
  shipped font only when an entry names it and reproduces license text. Today
  it reports all 7 editor fonts; `ValidateStagedPackageExecutables.cmake` runs
  it in report mode until these texts land, after which the default should
  become `enforce`.
- **SPDX fields:** rewrite the free-text `license` fields as SPDX expressions
  once the choices in the inventory table are made.
- **Files that change:** `ThirdParty/dependencies.lock`;
  `ThirdParty/supply-chain.lock` (through `tools/check-supply-chain.py
  --update`); `ThirdParty/**` payload; license files for `SparkEditor/Fonts/`;
  `cmake/SparkThirdPartyAudit.cmake` (if packaged notices must include fonts
  and extra notices, or should reuse the repository generator);
  `THIRD_PARTY_NOTICES` (regenerated).

## Regenerating and checking the notices

```bash
python tools/governance/generate_third_party_notices.py            # write THIRD_PARTY_NOTICES
python tools/governance/generate_third_party_notices.py --check    # exit 1 if stale
python tools/governance/generate_third_party_notices.py --require-complete  # exit 1 if a locked component has no notice
python -m pytest Tests/Tools/test_third_party_notices.py
```

Regenerate `THIRD_PARTY_NOTICES` after any change to the two lockfiles or to
a tracked license file under `ThirdParty/`.
