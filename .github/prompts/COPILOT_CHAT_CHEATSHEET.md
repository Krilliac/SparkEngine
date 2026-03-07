# GitHub Copilot Chat Cheat Sheet for SparkEngine

## Quick Commands
- `#prompt:<name>` - Load specific prompt file (e.g., #prompt:02_editor_ui_tasks)
- `/fix` - Apply refactoring to selected code following SparkEngine standards
- `/explain` - Explain selected code or diff with SparkEngine context
- `/tests` - Generate unit tests following SparkEngine testing patterns
- `/doc` - Generate XML documentation for selected code

## Common Workflows

### Code Refactoring
```
#prompt:01_code_refactor_rules
/fix Apply thread-safety and console integration to selected files
```

### Editor Development
```
#prompt:02_editor_ui_tasks
Implement asset browser window with console integration and async thumbnails
```

### Graphics Programming
```
#prompt:03_graphics_dx11_tasks
Add HDR rendering pipeline with console controls for tone mapping
```

### Audio System Work
```
#prompt:04_audio_system_tasks
Implement 3D audio source pooling with distance attenuation
```

## Agent Mode (Multi-file edits)
Switch to Agent mode for complex refactoring:
```
Apply #prompt:01_code_refactor_rules to all files in SparkEngine/Source/Editor 
and generate commit-ready changes with descriptive messages
```

## File References
Use `#file:` to include specific file context:
- `#file:SparkEngine/Source/Graphics/GraphicsEngine.h`
- `#folder:SparkEngine/Source/Audio`

## Best Practices
1. Always load project context with `#prompt:00_project_overview` first
2. Use specific prompts for targeted development tasks
3. Request code reviews and testing recommendations
4. Ask for performance impact analysis for changes
5. Ensure all new code includes console integration
