# SparkEngine Project Overview
Load complete project context for comprehensive understanding.

## Context Loading
- #file:README.md
- #file:CMakeLists.txt  
- #folder:SparkEngine/Source
- #folder:Shaders
- #folder:Resources

## Analysis Tasks
1. **Architecture Summary**
   - Identify all major subsystems (Graphics, Audio, Camera, Editor, etc.)
   - Map dependencies between systems
   - Highlight unique features (console integration, thread safety, object pooling)

2. **Working Branch Features**
   - Enhanced GraphicsEngine with console integration
   - Professional 3D AudioEngine with spatial positioning
   - Thread-safe SparkEngineCamera with real-time control
   - Advanced projectile system with object pooling
   - Comprehensive shader pipeline with PBR support

3. **Console Integration**
   - SimpleConsole system with rate-limited logging
   - 200+ console commands across all systems
   - Real-time parameter control for all engine features
   - Professional command help and auto-completion

4. **Performance Characteristics**
   - 60+ FPS target with editor active
   - Memory-efficient object pooling
   - Thread-safe operations throughout
   - Professional performance monitoring

## Output
Provide comprehensive architecture map with:
- System interaction diagram
- Performance hotspots and optimization opportunities
- Console command categories and examples
- Risk assessment for major changes