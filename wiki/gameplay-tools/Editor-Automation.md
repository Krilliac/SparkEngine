# Editor Automation

Extensible command registry, script execution engine, and multi-step wizard framework for automating editor operations.

**Source:** `SparkEditor/Source/Panels/EditorAutomation.h`

## Overview

Editor Automation provides a central registry for named commands that can be invoked programmatically, from text scripts, or through wizard UIs. Any editor operation that modifies the scene -- selecting objects, changing properties, importing assets -- can be registered as a command and then composed into automated workflows.

Commands are registered with a name, description, category, and handler callback. They accept string arguments and return a structured result with success/failure, a human-readable message, and an optional return value. The system records all executed commands in a history with timestamps, enabling undo-style replay and audit logging.

The scripting engine parses multi-line text scripts where each line is a command invocation. Lines beginning with `#` or `/` are treated as comments. For more structured workflows, the wizard system organizes commands into named, ordered steps with optional skip support. Wizards can be registered and triggered from UI buttons or other automation scripts. The system is designed as the integration point for AngelScript, Lua, or Python -- each scripting language binds a thin wrapper that calls `ExecuteCommand` under the hood.

## Key Classes

| Class / Struct | Description |
|---|---|
| `EditorAutomation` | Singleton command registry, script runner, and wizard manager |
| `EditorCommandInfo` | Metadata for a registered command: name, description, usage, category |
| `EditorCommandResult` | Execution result: success flag, message, and optional return value |
| `EditorWizard` | Multi-step wizard with ordered steps and execution state |
| `WizardStep` | A single wizard step: name, description, command string, optional flag |
| `CommandHistoryEntry` | Record of an executed command with result and timestamp |
| `ScriptResult` | Summary of a script run: commands executed, failures, error messages |

## Usage

```cpp
auto& automation = SparkEditor::EditorAutomation::GetInstance();
automation.Initialize();  // registers built-in commands (echo, help, wait, noop)

// Register a custom command
automation.RegisterCommand("select_all_lights", "Select all light entities",
    [](const std::vector<std::string>& args) -> SparkEditor::EditorCommandResult {
        // ... select lights via SelectionManager ...
        return {true, "Selected 12 lights", "12"};
    },
    "select_all_lights", "Selection");

// Execute a single command
auto result = automation.ExecuteCommand("select_all_lights", {});

// Run a multi-line script
auto scriptResult = automation.RunScript(R"(
    # Lighting setup script
    select_all_lights
    set_property intensity 2.5
    save_scene
)");

// Create and run a wizard
SparkEditor::EditorWizard wizard("Setup Lighting");
wizard.AddStep("Select lights", "select_all_lights", "Find all lights in scene");
wizard.AddStep("Set intensity", "set_property intensity 1.0", "Normalize intensity");
wizard.AddStep("Bake lightmaps", "bake_lighting", "Optional bake step", /*optional=*/true);
automation.RegisterWizard(wizard);
auto wizResult = automation.RunWizard("Setup Lighting");

// Query available commands
auto names = automation.GetCommandNames();
auto sceneCommands = automation.GetCommandsByCategory("Scene");
```

### Scripting Language Integration

```cpp
// AngelScript binding
engine->RegisterGlobalFunction("bool EditorExec(const string &in cmd)",
    asFUNCTION(+[](const std::string& cmd) -> bool {
        return SparkEditor::EditorAutomation::GetInstance().ExecuteCommand(cmd, {}).success;
    }), asCALL_CDECL);

// Lua binding
lua["editor_exec"] = [](const std::string& cmd) -> bool {
    return SparkEditor::EditorAutomation::GetInstance().ExecuteCommand(cmd, {}).success;
};
```

## API Reference

### Command Registration

| Method | Return | Description |
|---|---|---|
| `RegisterCommand(name, desc, handler, usage, category)` | `bool` | Register a named command with handler callback |
| `UnregisterCommand(name)` | `void` | Remove a registered command |
| `HasCommand(name)` | `bool` | Check if a command is registered |
| `GetCommandCount()` | `size_t` | Number of registered commands |
| `GetCommandInfo(name)` | `const EditorCommandInfo*` | Get metadata for a command |
| `GetCommandNames()` | `vector<string>` | All registered command names (sorted) |
| `GetCommandsByCategory(category)` | `vector<string>` | Commands in a specific category |

### Execution

| Method | Return | Description |
|---|---|---|
| `ExecuteCommand(name, args)` | `EditorCommandResult` | Execute a command by name with arguments |
| `ExecuteCommandLine(commandLine)` | `EditorCommandResult` | Parse and execute a single command string |
| `RunScript(script)` | `ScriptResult` | Execute a multi-line script |
| `GetHistory()` | `const vector<CommandHistoryEntry>&` | Full command execution history |

### Wizards

| Method | Return | Description |
|---|---|---|
| `RegisterWizard(wizard)` | `void` | Register a multi-step wizard |
| `GetWizard(name)` | `const EditorWizard*` | Get a wizard by name |
| `RunWizard(name)` | `ScriptResult` | Execute all steps of a wizard |
| `GetWizardNames()` | `vector<string>` | All registered wizard names |

## Related Systems

- [Selection Manager](Selection-Manager.md) -- automation commands frequently manipulate selection
- [Asset Dependency Graph](Asset-Dependency-Graph.md) -- asset audit commands use the dependency graph
- [AngelScript Scripting](../subsystems/Scripting-with-AngelScript.md) -- primary scripting language integration point
- [Editor Panels](SparkEditor.md) -- panels can expose operations as automation commands
