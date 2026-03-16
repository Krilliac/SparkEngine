# Visual Scripting

## Status: Removed

The visual scripting system (`VisualScriptSystem` and `VisualScriptingSystem`) was removed in March 2026 as part of a dead code cleanup. Neither implementation was wired into any startup path, and both existed as unintegrated scaffolding (~7,300 lines combined).

**What was removed:**
- `SparkEngine/Source/Engine/Scripting/VisualScriptSystem.{h,cpp}` (2,406 lines) -- Engine-side node graph with compilation to AngelScript
- `SparkEditor/Source/VisualScripting/VisualScriptingSystem.{h,cpp}` (4,937 lines) -- Editor-side visual scripting with UI integration
- `Tests/TestVisualScriptSystem.cpp` -- Orphaned test (not in CMakeLists.txt)

**If you need visual scripting in the future:**
- The original implementations are preserved in git history
- The design supported node-based graphs, typed pins, compilation to AngelScript, and JSON serialization
- Consider building a single unified implementation rather than the parallel engine/editor systems that existed before

## See Also

- [Scripting with AngelScript](Scripting-with-AngelScript) -- Text-based scripting (active, fully integrated)
- [Entity-Component-System](Entity-Component-System) -- How scripts attach to entities
