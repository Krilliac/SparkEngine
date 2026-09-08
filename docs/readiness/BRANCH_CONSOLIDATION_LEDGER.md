# SparkEngine branch and worktree consolidation ledger

## Decision

The sole surviving repository source will be the GitHub default branch, `Working`, checked out only at `D:\SparkEngine-release-canonical`. During this audit, `claude/stable-v1-release` is the canonical staging branch; once its final verified commit is published, `Working` will fast-forward to it and the staging ref will be removed.

This ledger records why every noncanonical source is either already contained, integrated as an equivalent/stronger repair, or deliberately superseded. It is an audit record, not release evidence: the external release gates remain blocked below.

## Audit basis

- Staging source at audit start: `claude/stable-v1-release` at `279f2c6fef146e46c47c730996b4aa1cd30c3c12`.
- Comparison method: ancestry, patch-equivalence where applicable, focused source review, and the release verification suites named in the associated canonical commits.
- Live GitHub heads were read on 2026-09-08 and matched their local tracking refs exactly.
- No noncanonical branch or worktree is removed until the final clean-tree verification and the fast-forward publication step succeed.

## Directly contained sources

The following heads are already ancestors of the staging branch and require no replay:

- `Working`
- `claude/ci-msan-instrumented-libcxx`
- `worktree-doc-410-opus46`
- `worktree-rdy020-asset-integrity`
- `worktree-rdy020-repair`
- `worktree-sec110-supply-chain`
- `worktree-sec120-fuzz-policy`
- `worktree-spark-audit-security`

The current remote heads are also direct ancestors: `Working` (`891e00c1c`), `claude/ci-msan-instrumented-libcxx` (`8a0cfc0b2`), `claude/ci-windows-compiler-cache` (`45e2da5df`), `fix/ci-release-evidence` (`ff951c19a`), `fix/msan-canary-census` (`aec29db6a`), `release/blender-asset-quality` (`e44c1c6f8`), `release/fps-sdk-slice` (`af99169cc`), `release/msan-validation` (`ed61b8936`), and `release/stable-shipping` (`13f7ee585`).

## Reviewed and consumed sources

| Source group | Disposition | Canonical evidence |
| --- | --- | --- |
| `codex/sec100-cross-epoch`, `sec100-repair-codex`, `codex/remote-loopback-fix` | Integrated and strengthened. | `88efd3fab`, `d936b5d3a`, `8dbdb10b5` harden remote-debug epoch authority and race coverage. |
| `codex/lan-discovery-fix` | Integrated. | `b12bfa2eb` receives directed LAN broadcasts. |
| `codex/stable-v1-rc`, `codex/stable-v1-life200a`, `codex/stable-v1-integration`, `codex/stable-v1-next-batch` | Integrated or superseded by the coherent release baseline. | Canonical scene, lifecycle, CI, asset, and docs repairs replace the mixed batch history without reopening reverted work. |
| `claude/ops100-adversarial-opus46`, `claude/ops100-tooling-repair`, `worktree-ops100-tooling`, `codex/stable-v1-ops100a` | Integrated and strengthened. | `846fda053`, `3b167342c`, `68be034f9`, `16d2cdae0`, `ffe75be6a`. |
| `claude/perf100-opus46`, `claude/perf100-adversarial-opus46`, `worktree-perf100-adversarial` | Integrated and strengthened. | `fa5bf604a`, `b75ba9f25`, `a1d1ccca5`, `ad916bfa2`. |
| `claude/rdy-000-release-profiles`, `worktree-spark-rdy000-opus` | Integrated. | The canonical readiness contract carries the stable-v1 profile and fail-closed evidence policy. |
| `claude/rdy010-opus46`, `claude/rdy010-adversarial-opus46` | Integrated and strengthened. | `6d4c22f34`, `f18cd4dc0`, `c7f126b1f`, `4a1fb4823`, `769f45064`, `eb3a96e8a`, `169fbb337`. |
| `claude/rdy020-adversarial-opus46`, `claude/rdy020-asset-integrity-repair-opus46`, `codex/stable-v1-rdy020`, `codex/stable-v1-rdy020-next`, `rdy020-repair2-opus46` and detached asset-validation worktrees | Semantically superseded by the stronger canonical asset-integrity contract. | Canonical manifests, ownership evidence, hostile-path validation, and direct containment of the final repair heads. |
| `claude/sec110-adversarial-opus46`, `claude/sec110-repair-opus46`, `sec110-repair2-opus46`, `worktree-sec110-supply-chain` | Integrated and strengthened. | Canonical dependency/provenance safeguards and generated evidence supersede the individual repair branches. |
| `codex/sec120-repair`, `worktree-sec120-fuzz-policy` | Integrated. | The canonical SEC-120 policy/fuzz infrastructure is directly contained and remains structurally blocking until external evidence exists. |
| `plt200-repair2-opus46` and detached `D:\SparkEngine-plt200-opus46` | Integrated and strengthened. | `2e566e915`, `dcc59d842`, `9a1d28bf1`, `4cc4e122`. |
| `codex/doc410-repair2`, `codex/docs-generator-fix`, `doc-410-postfix-opus46`, `worktree-doc-410-opus46` | Superseded by the current deterministic docs contract and regenerated outputs. | The current generators, currentness checker, and checked-in artifacts are the retained source of truth. |
| `ci120-build-matrix-parity`, `ci120-repair2-opus46`, `claude/ci110-sanitizer-opus46`, `claude/ci-windows-compiler-cache`, `claude/codeql-evidence`, `worktree-spark-ci-provenance`, `worktree-release-acceptance-gate` | Integrated or superseded by stronger fail-closed canonical CI controls. | Current build-matrix, sanitizer, CodeQL, release-acceptance, provenance, and required-job contracts are retained; the package/required-job gate repair in this commit closes a further discovered omission. |
| `codex/package-contract-next` | Deliberately superseded, not cherry-picked. | Its large static package parser reports baseline false positives and duplicates incompatible policy. The retained semantic gate rejects advisory, conditional, shell, and error-suppressed package validation paths while executing live workflow fixtures. |
| `codex/sanitizer-regression-fixes` | Superseded. | Canonical sanitizer evidence and source validation are stronger than the isolated regression patch. |
| `codex/telemetry-network-fixes` | Partially integrated; weaker network policy rejected. | The safe macOS `/var` spool alias repair is retained in `279f2c6fe`; LAN behavior is covered by `b12bfa2eb`. |
| `backup-working-pre-merge` | Historical backup; patch-equivalent content is already covered. | No unique cherry-pickable patch remains after comparison. |

### Detached save/load worktree

`D:\SparkEngine-save230` at `ead4f25dc2d71012dd07e3a898daed058d26f953` contains the older `e8d03d323` lifecycle transaction repair. It is superseded by the canonical lifecycle series (`734d16fc8`, followed by `be3047d28`, `7490b6a18`, `1987c40dc`, `ebdd6ab04`, and `530fd519d`). The retained code adds reactive rebind notifications, allocator/topology checks, and exception-boundary behavior beyond the old patch. Its canonical lifecycle tests cover observer preservation, stale entity subscription retirement, candidate rollback, and reactive rebind behavior.

### Detached worktrees with no remaining source delta

`C:\Users\Nathan\AppData\Local\Temp\sparkengine-badge-docs-fefa8ae3`, `D:\SparkEngine-codex-asset-validation`, `D:\SparkEngine-codex-rdy020-verify`, `D:\SparkEngine-codex-rdy020-verify-final`, `D:\SparkEngine-doc410-opus46`, `D:\SparkEngine-net100`, `D:\SparkEngine-plt200-opus46`, `D:\SparkEngine-rel100`, and `D:\SparkEngine-sec120-opus46` have either no patch delta or only material superseded above.

## Hosted pull requests

Open PRs 567 through 574 are a stacked release chain whose head commits are all ancestors of the staging branch. After `Working` is fast-forwarded, they will be closed as already consumed and their remote source branches deleted:

| PR | Head branch | Head |
| --- | --- | --- |
| #567 | `claude/ci-windows-compiler-cache` | `45e2da5df` |
| #568 | `claude/ci-msan-instrumented-libcxx` | `8a0cfc0b2` |
| #569 | `fix/ci-release-evidence` | `ff951c19a` |
| #570 | `fix/msan-canary-census` | `aec29db6a` |
| #571 | `release/stable-shipping` | `13f7ee585` |
| #572 | `release/fps-sdk-slice` | `af99169cc` |
| #573 | `release/msan-validation` | `ed61b8936` |
| #574 | `release/blender-asset-quality` | `e44c1c6f8` |

The Blender asset branch is retained in the canonical history; it is not discarded. Any future visual/model iteration will be performed from the surviving source, with Astra reserved for genuine editor UX or Blender asset work.

## Non-source generated state

- `D:\SparkEngine-codex-next-batch` has only stale generated docs/badge output and is not an implementation source.
- `D:\SparkEngine-doc410-repair2-codex` has an untracked generated `.site-data-doc410-current` tree and is not an implementation source.
- `D:\SparkEngine\.claude\worktrees\rdy020-asset-integrity` has untracked session-memory state only.
- The canonical worktree's generated Python fixture directories and `.pytest_cache` were inspected, confirmed as scratch output, and removed before final verification.

## Closure sequence

1. Commit the current release-gate hardening and regenerated artifacts.
2. Re-run clean-tree verification at the committed SHA.
3. Fast-forward `Working` to the verified staging commit and publish it.
4. Close PRs #567–#574 as consumed; delete their remote source branches.
5. Remove every noncanonical worktree by exact path, then delete its local branch only after its ledger classification and ancestry check succeed.
6. Switch `D:\SparkEngine-release-canonical` to `Working`, delete `claude/stable-v1-release`, prune stale worktree metadata, and verify exactly one worktree/branch source remains.

## Release blockers that consolidation cannot manufacture

- Exact-SHA hosted CI and trusted provenance for all required platforms.
- Signed, SBOM-attested, scanned, install/uninstall/rollback-tested shipping artifacts.
- Platform certification evidence and publisher approval.
- Certified performance baselines, soak measurements, and rendering golden images.
- SEC-120 closure evidence and the three remaining structural policy blockers.
- CMake File API evidence for the installed SDK consumer, Windows Shipping, and Windows validation profiles; their absence remains intentionally blocking in the build-matrix report.

No release tag or artifact publication is authorized by this ledger.
