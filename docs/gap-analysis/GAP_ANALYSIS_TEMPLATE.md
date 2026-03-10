# SparkEngine Gap Analysis — Master Template

<!-- ============================================================================
     DO NOT DELETE THIS FILE. This template is a permanent reference document.
     It defines the standard format, severity levels, and workflow for all
     SparkEngine gap analyses. Individual gap analysis documents should follow
     this template structure.
     ============================================================================ -->

> **Purpose**: Permanent reference template for creating, maintaining, and tracking gap analysis documents across the SparkEngine project.
> **Status**: PERMANENT — This document is not deleted after implementation work is complete.
> **Last Updated**: 2026-03-10

---

## Table of Contents

1. [Gap Analysis Index](#gap-analysis-index)
2. [Template Structure](#template-structure)
3. [Severity Definitions](#severity-definitions)
4. [Gap Entry Format](#gap-entry-format)
5. [Implementation Tracking](#implementation-tracking)
6. [Workflow](#workflow)
7. [Naming Conventions](#naming-conventions)

---

## Gap Analysis Index

The following gap analysis documents exist in this directory. Each covers a specific engine subsystem or cross-cutting concern.

| Document | Prefix | Subsystem | Scope |
|----------|--------|-----------|-------|
| `AI_NAVIGATION_GAP_ANALYSIS.md` | GAP-AI | AI & Navigation | `Engine/AI/`, NavMesh, BehaviorTree, Perception, Steering |
| `ANIMATION_GAP_ANALYSIS.md` | GAP-AN | Animation | `Engine/Animation/`, skeletal, IK, state machines |
| `AUDIO_GAP_ANALYSIS.md` | GAP-AUD | Audio | `Audio/`, XAudio2, SoundEffect, MusicManager |
| `CINEMATIC_GAP_ANALYSIS.md` | GAP-CIN | Cinematics | `Engine/Cinematic/`, Sequencer, keyframe animation |
| `CORE_INFRASTRUCTURE_GAP_ANALYSIS.md` | GAP-CI | Core | `Core/`, EngineContext, Platform, module loading |
| `COROUTINE_GAP_ANALYSIS.md` | GAP-CO | Coroutines | `Engine/Coroutine/`, CoroutineScheduler |
| `CPU_PERFORMANCE_GAP_ANALYSIS.md` | GAP-CPU | CPU Performance | Frame timing, job system, cache optimization |
| `ECS_GAP_ANALYSIS.md` | GAP-ECS | Entity Component System | `Engine/ECS/`, Components, Systems, World |
| `EDITOR_GAP_ANALYSIS.md` | GAP-ED | Editor | `SparkEditor/Source/`, ImGui panels, tools |
| `EVENT_SYSTEM_GAP_ANALYSIS.md` | GAP-EV | Events | `Engine/Events/`, EventBus, typed dispatch |
| `GPU_CPU_OFFLOADING_GAP_ANALYSIS.md` | GAP-OFF | GPU/CPU Offloading | VRAM budget, resource residency, quality scaling |
| `GRAPHICS_GAP_ANALYSIS.md` | GAP-G | Graphics/Rendering | `Graphics/`, RHI, materials, lighting, post-processing |
| `INPUT_GAP_ANALYSIS.md` | GAP-I | Input | `Input/`, PlatformInput, gamepad, action mapping |
| `LOGGING_GAP_ANALYSIS.md` | GAP-LOG | Logging | `Utils/Logger`, `Utils/FileLogger`, log macros |
| `MEMORY_LEAK_GAP_ANALYSIS.md` | GAP-MEM | Memory | Leak detection, allocation tracking, RAII |
| `NETWORKING_GAP_ANALYSIS.md` | GAP-NET | Networking | `Engine/Networking/`, NetworkManager, replication |
| `PHYSICS_GAP_ANALYSIS.md` | GAP-PH | Physics | `Physics/`, Bullet integration, collision |
| `PROCEDURAL_GENERATION_GAP_ANALYSIS.md` | GAP-PG | Procedural Gen | `Engine/Procedural/`, terrain, content generation |
| `SAVE_SYSTEM_GAP_ANALYSIS.md` | GAP-SS | Save System | `Engine/SaveSystem/`, serialization, metadata |
| `SCRIPTING_GAP_ANALYSIS.md` | GAP-SC | Scripting | `Engine/Scripting/`, AngelScript integration |
| `STABILITY_GAP_ANALYSIS.md` | GAP-ST | Stability | Cross-cutting: smart pointers, error handling, thread safety |

---

## Template Structure

Every gap analysis document MUST follow this structure:

```markdown
# SparkEngine [Subsystem Name] — Gap Analysis

> **Scope**: [directories and files covered]
> **Date**: [YYYY-MM-DD]
> **Methodology**: [how the analysis was conducted]
> Each gap is assigned a severity: **Critical**, **Major**, **Moderate**, **Minor**.

---

## Context
[Brief description of the subsystem's purpose, current state, and why gaps matter]

---

## Critical Gaps
### GAP-XX01 — [Title]
[Gap entry — see format below]

## Major Gaps
### GAP-XX02 — [Title]
[Gap entry]

## Moderate Gaps
### GAP-XX03 — [Title]
[Gap entry]

## Minor Gaps
### GAP-XX04 — [Title]
[Gap entry]

---

## Implementation Priority
[Ordered list of gaps by recommended implementation sequence]

## Architectural Recommendations
[High-level suggestions that span multiple gaps]
```

---

## Severity Definitions

| Severity | Definition | Impact | Response Time |
|----------|-----------|--------|---------------|
| **Critical** | Blocks core functionality or causes crashes/data loss | System is non-functional or unsafe without fix | Must address before any release |
| **Major** | Significant missing feature or incorrect behavior | Feature is incomplete or produces wrong results | Address in current development cycle |
| **Moderate** | Partial implementation or suboptimal approach | Feature works but is limited or fragile | Address when working in the area |
| **Minor** | Polish, optimization, or code quality improvement | No functional impact but improves maintainability | Address opportunistically |

### Severity Selection Guide

- **Critical**: "The engine crashes / renders nothing / loses data because of this"
- **Major**: "A significant advertised feature doesn't work because of this"
- **Moderate**: "It works, but the implementation is incomplete or fragile"
- **Minor**: "It works correctly, but could be better (performance, readability, etc.)"

---

## Gap Entry Format

Each gap entry MUST include these sections:

```markdown
### GAP-[PREFIX][NUMBER] — [Descriptive Title]

**Files**:
- `path/to/File.h` (lines X-Y: description of relevant code)
- `path/to/File.cpp` (lines X-Y: description of relevant code)

**Impact**: [What breaks, what doesn't work, what the user/developer experiences
because this gap exists. Be specific — "X doesn't work" not "could be improved".]

**Evidence**:
```cpp
// File.cpp:LineNumber — description of the problematic code
[Relevant code snippet showing the gap]
```

**What is needed**:
1. [Specific, actionable implementation step]
2. [Another specific step]
3. [Continue until the gap would be fully closed]

**Implementation Status**: [See tracking section below]
```

### Rules for Gap Entries

1. **Gap IDs are permanent** — Once assigned, a GAP-XX## ID is never reused, even if the gap is resolved. This maintains traceability.
2. **One gap = one issue** — Don't combine multiple distinct problems into a single gap. Split them.
3. **File references must include line numbers** — Approximate is fine, but give readers a starting point.
4. **Evidence must be real code** — Copy from the actual source, not pseudocode.
5. **"What is needed" must be implementable** — Someone reading only this section should be able to implement the fix.

---

## Implementation Tracking

Each gap entry may include an `**Implementation Status**` field with one of these values:

| Status | Meaning |
|--------|---------|
| `OPEN` | Gap identified, not yet addressed |
| `IN_PROGRESS` | Implementation underway |
| `IMPLEMENTED` | Code written and compiling |
| `VERIFIED` | Tested and confirmed working |
| `WONT_FIX` | Intentionally not addressing (must include rationale) |
| `SUPERSEDED` | Replaced by a different approach (must reference replacement) |

### Tracking Format

Add this to any gap entry to track its status:

```markdown
**Implementation Status**: IMPLEMENTED
- **Date**: 2026-03-10
- **Commit**: abc1234
- **Notes**: Implemented LRU eviction with 3-tier priority system.
  Added 512MB staging pool with async re-upload via staging buffers.
```

### Summary Table

Each gap analysis document may include a summary tracking table at the top:

```markdown
## Status Summary

| Gap ID | Severity | Status | Date |
|--------|----------|--------|------|
| GAP-XX01 | Critical | IMPLEMENTED | 2026-03-10 |
| GAP-XX02 | Major | OPEN | — |
| GAP-XX03 | Moderate | IN_PROGRESS | 2026-03-10 |
```

---

## Workflow

### Creating a New Gap Analysis

1. Copy this template's structure
2. Choose a unique 2-3 letter prefix (check the index above for conflicts)
3. Conduct the analysis: read every file in scope, check for stubs, missing features, incorrect behavior
4. Write gaps starting from Critical down to Minor
5. Number gaps sequentially within severity (but they appear grouped by severity)
6. Add the document to the index table in this template
7. Commit both the new document and the updated template

### Updating an Existing Gap Analysis

1. When new gaps are discovered, append them with the next available number
2. When gaps are resolved, update `**Implementation Status**` — do NOT delete the gap entry
3. Update the date in the document header
4. If the scope changes significantly, note it in the Context section

### Post-Implementation Review

After implementation work:
1. Update each addressed gap's `**Implementation Status**`
2. Add commit references and testing notes
3. If new gaps were discovered during implementation, add them
4. **DO NOT delete resolved gap entries** — they serve as historical documentation

---

## Naming Conventions

### Document Names
- Format: `[SUBSYSTEM]_GAP_ANALYSIS.md`
- Use UPPER_SNAKE_CASE
- The subsystem name should match the engine directory/concept

### Gap ID Format
- Format: `GAP-[PREFIX][NUMBER]`
- PREFIX: 2-3 uppercase letters unique to the document (see index)
- NUMBER: 2-digit zero-padded sequential number (01, 02, ... 99)
- Example: `GAP-G01`, `GAP-ECS02`, `GAP-OFF14`

### Reserved Prefixes

| Prefix | Document |
|--------|----------|
| AI | AI & Navigation |
| AN | Animation |
| AUD | Audio |
| CI | Core Infrastructure |
| CIN | Cinematic |
| CO | Coroutine |
| CPU | CPU Performance |
| ECS | Entity Component System |
| ED | Editor |
| EV | Event System |
| G | Graphics |
| I | Input |
| LOG | Logging |
| MEM | Memory |
| NET | Networking |
| OFF | GPU/CPU Offloading |
| PG | Procedural Generation |
| PH | Physics |
| SC | Scripting |
| SS | Save System |
| ST | Stability |

---

## Cross-References

When gaps in different documents are related, use cross-references:

```markdown
**See also**: STABILITY_GAP_ANALYSIS.md GAP-ST01 (related smart pointer migration)
```

This links related work across subsystem boundaries without duplicating gap entries.

---

## Architecture Decision Records

When a gap's resolution involves a significant architectural decision, document it:

```markdown
**ADR**: Chose LRU eviction over LFU because:
- Simpler implementation with O(1) evict via doubly-linked list
- Frame-based access tracking aligns naturally with LRU
- LFU would require frequency counters that persist across scene loads
```

---

<!-- ============================================================================
     END OF TEMPLATE
     This file is a permanent reference. Do not delete after implementation.
     ============================================================================ -->
