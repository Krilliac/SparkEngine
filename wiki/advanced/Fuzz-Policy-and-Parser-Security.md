# Fuzz Policy and Parser Security

**Audience:** Engine developers, security reviewers, CI maintainers
**Thread Context:** SEC-120 — fuzz and bound every untrusted parser and protocol
**Platform-Backend Scope:** All platforms (fuzz infrastructure runs on Linux CI)

---

## Overview

SparkEngine processes untrusted data from files, network packets, scripts, and modded content through ~48 distinct parser surfaces. SEC-120 establishes a **fail-closed fuzz policy** that requires every parser crossing a trust boundary to be inventoried, classified, and either fuzzed or explicitly blocked with a tracking ticket.

The policy infrastructure lives in `tools/fuzz-policy/` and is enforced by CI via `check_fuzz_policy.py`.

---

## Trust Boundary Model

Every parser is classified into one of three trust boundaries:

| Boundary | Description | Risk | Examples |
|----------|-------------|------|----------|
| `untrusted-file` | Data from disk that users/modders can supply | Path traversal, buffer overflow, integer overflow, unbounded allocation | Save files, scene files, textures, meshes, mods, config |
| `untrusted-network` | Data from network peers (unauthenticated UDP) | All file risks + remote exploitation, DoS | Packets, replication data, datablocks |
| `trusted-internal` | Data from engine-controlled sources | Lower risk but still fuzzable (IPC, database) | Daemon framing, SQLite rows, scene snapshots |

### Trust Boundary Rules

1. **Untrusted-file** is the default classification for any parser that reads from disk — user-editable config, save files, and modded content are all untrusted
2. **Untrusted-network** applies to any parser that processes data received over the network, regardless of whether the protocol is authenticated
3. **Trusted-internal** is reserved for parsers where both the writer and reader are engine-controlled, with no user/modder path to inject data
4. A parser handling common external formats (`.json`, `.xml`, `.csv`) in a trusted-internal role must document why the trust boundary is correct

---

## Parser Inventory

The authoritative parser inventory lives in `tools/fuzz-policy/parser_inventory.py` as the `KNOWN_PARSERS` list. Each entry contains:

- **parser_id**: Unique kebab-case identifier
- **description**: What the parser does
- **trust_boundary**: One of the three boundaries above
- **source_files**: All source files that implement the parser
- **formats_handled**: File extensions or wire format names
- **fuzz_status**: `fuzzed`, `blocked`, or `unclassified`
- **fuzz_target**: Path to the libFuzzer harness (when fuzzed)
- **blocker_reason/blocker_ticket**: Why fuzzing is blocked and the tracking ticket

### Adding a New Parser

When adding code that parses external data:

1. Add a `ParserEntry` to `KNOWN_PARSERS` in `parser_inventory.py`
2. Set the trust boundary — default to `untrusted-file` unless you can prove otherwise
3. Set `fuzz_status` to either `fuzzed` (with a harness) or `blocked` (with reason + ticket)
4. Run `python tools/fuzz-policy/check_fuzz_policy.py` to verify
5. The CI gate will reject any PR that adds parser-like code without a registry entry (in `--ci` mode)

---

## Fuzz Status Lifecycle

```
UNCLASSIFIED → BLOCKED (with ticket) → FUZZED (with harness)
                                            ↓
                                      Regression fixtures persisted
```

- **UNCLASSIFIED**: The CI gate rejects this in `--ci` mode. Every parser must be classified.
- **BLOCKED**: The parser is known but not yet fuzzed. Must have a `blocker_reason` explaining why and a `blocker_ticket` tracking the work.
- **FUZZED**: The parser has a working libFuzzer/AFL harness. Must declare `fuzz_target`, `max_input_bytes`, and `max_parse_time_ms`.

---

## Resource Budgets

Every fuzz target must declare bounded resource limits to prevent unbounded consumption during fuzz campaigns. Four standard tiers are defined in `corpus_manifest.py`:

| Tier | Max Input | Timeout | Memory | Corpus Entries | Corpus Size |
|------|-----------|---------|--------|----------------|-------------|
| **network** | 64 KB | 500 ms | 128 MB | 10,000 | 100 MB |
| **small** | 1 MB | 1,000 ms | 256 MB | 5,000 | 500 MB |
| **medium** | 10 MB | 5,000 ms | 1 GB | 10,000 | 2 GB |
| **large** | 50 MB | 30,000 ms | 2 GB | 50,000 | 5 GB |

Hard ceilings enforce that no tier can exceed: 100 MB input, 60s timeout, 4 GB memory, 100K entries, 10 GB corpus.

### Corpus Freshness

Seed corpora must be re-verified every 90 days. The `corpus_manifest.py` `last_verified` field tracks this, and the CI gate rejects stale corpora.

---

## Risk-Priority Matrix

Parsers are prioritized for fuzz harness implementation based on trust boundary and attack surface:

| Priority | Trust Boundary | Parser Category | Examples |
|----------|---------------|-----------------|----------|
| **P0 — Critical** | untrusted-network | Network packet deserializers | `network-packet-parser`, `entity-replicator`, `datablock-registry` |
| **P0 — Critical** | untrusted-file | Archive/package extractors | `sparkpak`, `archive-resource-provider`, `mod-system` |
| **P1 — High** | untrusted-file | Binary format parsers | `save-system`, `mesh-obj-loader`, `audio-sound-effect`, `navmesh-loader` |
| **P1 — High** | untrusted-file | Scene/prefab deserializers | `scene-serializer`, `runtime-prefab`, `entity-archetype-loader` |
| **P2 — Medium** | untrusted-file | Text format parsers | `config-parser`, `material-loader`, `localization-system`, `datatable-system` |
| **P2 — Medium** | untrusted-file | Shader source parsers | `shader-source-loader`, `shader-compiler-tool` |
| **P3 — Lower** | trusted-internal | Internal serialization | `reflection-serializer`, `daemon-framing`, `async-database` |

---

## CI Integration

### Policy Check (Current)

```yaml
# In .github/workflows/build.yml (planned)
- name: Check fuzz policy
  run: python tools/fuzz-policy/check_fuzz_policy.py --source-root . --ci
```

The `--ci` flag treats unclassified parser sources as hard failures (exit 1).

### Fuzz Smoke (Planned — SEC-120 acceptance criteria)

```yaml
# Planned CI job: fuzz-smoke
- name: Build fuzz targets
  run: |
    cmake --preset linux-fuzz
    cmake --build build/linux-fuzz
- name: Run fuzz smoke
  run: ctest --test-dir build/linux-fuzz -L fuzz-smoke --output-on-failure
```

### Scheduled Campaigns (Planned — SEC-120 acceptance criteria)

Longer fuzz campaigns run on a schedule (not per-PR) and publish:
- Coverage percentage per parser
- Crash-free duration
- Minimized regression fixtures

---

## Validation Scripts

| Script | Purpose | CI Integration |
|--------|---------|----------------|
| `tools/fuzz-policy/parser_inventory.py` | Emit parser manifest | `--emit-json` for evidence |
| `tools/fuzz-policy/corpus_manifest.py` | Validate corpus metadata | `--check` for CI |
| `tools/fuzz-policy/check_fuzz_policy.py` | Fail-closed policy gate | `--ci` for PR checks |
| `tests/fuzz-policy/test_fuzz_policy.py` | 54 mutation tests | `pytest` |

---

## Related Work Items

- **SEC-120**: This policy infrastructure (fuzz and bound every untrusted parser)
- **SEC-110**: Supply-chain and dependency policy (ThirdParty fuzzing is upstream's responsibility)
- **NET-100**: Authenticated encryption (network parsers need transport security first)
- **ASSET-220**: Asset pipeline hardening (parsers in the asset import path)
- **SAVE-230**: Save system hardening (save file parser)

---

## Source & Freshness

- **Created:** 2026-08-28
- **Work item:** SEC-120
- **Base commit:** `360c05e8`
- **Evidence:** `docs/sec120-fuzz-policy-evidence.json`, `docs/sec120-parser-inventory.json`
- **Status:** Policy infrastructure delivered; 0/48 parsers have active fuzz harnesses; SEC-120 remains open/blocking
