# CSG System

Constructive Solid Geometry system for rapid level greyboxing with boolean mesh operations on primitive shapes.

**Source:** `SparkEngine/Source/Engine/LevelDesign/CSGSystem.h`, `SparkEditor/Source/Panels/CSGEditorPanel.h`

## Overview

The CSG system enables level designers to rapidly prototype environments using primitive shapes and boolean operations. Instead of modeling in external tools, designers create boxes, cylinders, spheres, wedges, and cones directly in the engine, then combine them with union, subtraction, and intersection operations to carve doorways, create windows, build complex room shapes, and more.

The system operates on `CSGSolid` objects -- collections of convex polygon faces with normals and texture coordinates. Boolean operations use BSP-based face clipping: each face of solid A is split by the planes of solid B's faces, keeping either the inside or outside portions depending on the operation. The result is a new solid that can be triangulated into a renderable `CSGMesh` with vertex and index buffers.

The editor provides `CSGEditorPanel`, an ImGui-based interface for creating brushes, selecting shapes and operations, adjusting transforms, and previewing the combined result in real time with auto-rebuild support.

## Architecture

```
CSGSystem (singleton)
  |
  +-- CreateBrush(shape, size) --> uint32_t brushId
  |     Shapes: Box, Cylinder, Sphere, Wedge, Cone
  |
  +-- SetBrushTransform(id, transform)
  +-- SetBrushOperation(id, Additive|Subtractive|Intersect)
  |
  +-- Boolean Operations:
  |     Union(A, B)     --> A + B (combined volume)
  |     Subtract(A, B)  --> A - B (carve B from A)
  |     Intersect(A, B) --> A n B (overlap only)
  |
  +-- GenerateMesh(solid) --> CSGMesh (vertices + indices)
  +-- BuildAll() --> combined mesh from all brushes

CSGEditorPanel (ImGui)
  +-- Shape selector, size controls, operation picker
  +-- Brush list with per-brush delete
  +-- Auto-rebuild toggle, manual rebuild button
  +-- Statistics display (brush count, triangle count)
```

## Key Classes

| Class | Description |
|-------|-------------|
| `CSGSystem` | Singleton managing brushes and boolean operations |
| `CSGEditorPanel` | ImGui editor panel for interactive CSG editing |
| `CSGSolid` | Collection of convex faces representing a solid volume |
| `CSGFace` | Single polygon face with vertices and normal |
| `CSGVertex` | Vertex with position, normal, and UV coordinates |
| `CSGMesh` | Output triangle mesh (vertex buffer + index buffer) |
| `CSGPlane` | BSP plane used for face splitting during boolean ops |
| `BrushShape` | Enum: Box, Cylinder, Sphere, Wedge, Cone |
| `CSGOperation` | Enum: Additive, Subtractive, Intersect |

## Usage

### Creating and Combining Brushes

```cpp
auto& csg = Spark::LevelDesign::CSGSystem::GetInstance();
csg.Initialize();

// Create a room (large box)
uint32_t room = csg.CreateBrush(Spark::LevelDesign::BrushShape::Box, {10, 3, 10});

// Create a doorway (small box positioned at wall)
uint32_t door = csg.CreateBrush(Spark::LevelDesign::BrushShape::Box, {1.2f, 2.5f, 0.5f});
csg.SetBrushTransform(door, {{5, 0, 0}, {0, 0, 0}, {1, 1, 1}});
csg.SetBrushOperation(door, Spark::LevelDesign::CSGOperation::Subtractive);

// Create a cylindrical pillar
uint32_t pillar = csg.CreateBrush(Spark::LevelDesign::BrushShape::Cylinder, {0.5f, 3.0f, 0});

// Build combined mesh from all brushes
Spark::LevelDesign::CSGMesh mesh = csg.BuildAll();
// mesh.vertices, mesh.indices, mesh.triangleCount
```

### Direct Boolean Operations

```cpp
auto& csg = Spark::LevelDesign::CSGSystem::GetInstance();

// Create two solids manually
auto boxSolid = /* ... */;
auto cylinderSolid = /* ... */;

// Subtract cylinder from box (e.g., create a hole)
auto result = csg.Subtract(boxSolid, cylinderSolid);

// Generate renderable mesh
auto mesh = csg.GenerateMesh(result);
```

### Tessellation Control

```cpp
// Increase cylinder smoothness (default: 16 segments)
csg.SetCylinderSegments(32);

// Increase sphere detail (default: 8 rings, 16 segments)
csg.SetSphereTessellation(16, 32);
```

## API Reference

### CSGSystem

| Method | Description |
|--------|-------------|
| `Initialize()` | Clear brushes, reset tessellation defaults |
| `Shutdown()` | Release all brushes |
| `CreateBrush(shape, size)` | Create a primitive brush, returns brush ID |
| `SetBrushTransform(id, transform)` | Set position, rotation, scale |
| `SetBrushOperation(id, op)` | Set Additive, Subtractive, or Intersect |
| `RemoveBrush(id)` | Delete a brush |
| `Union(A, B)` | Boolean union of two solids |
| `Subtract(A, B)` | Boolean subtraction (A minus B) |
| `Intersect(A, B)` | Boolean intersection (overlap only) |
| `GenerateMesh(solid)` | Triangulate a solid into vertex/index buffers |
| `BuildAll()` | Combine all brushes and return final mesh |
| `SetCylinderSegments(n)` | Set cylinder tessellation (min 3) |
| `SetSphereTessellation(rings, segs)` | Set sphere detail (min 2 rings, 3 segments) |

### CSGEditorPanel

| Method | Description |
|--------|-------------|
| `Initialize()` | Set up default brush shape, size, and auto-rebuild |
| `Render()` | Draw the ImGui interface (shape picker, brush list, build controls) |
| `Update(dt)` | Per-frame update |

## Configuration

| Setting | Default | Description |
|---------|---------|-------------|
| Cylinder segments | 16 | Number of sides for cylinder/cone primitives |
| Sphere rings | 8 | Latitude subdivisions for sphere primitives |
| Sphere segments | 16 | Longitude subdivisions for sphere primitives |
| Auto-rebuild | On | Automatically rebuild combined mesh when brushes change |

## Primitive Shapes

| Shape | Parameters | Description |
|-------|-----------|-------------|
| Box | width, height, depth | Axis-aligned box centered at origin |
| Cylinder | radius, height, -- | Vertical cylinder with configurable segments |
| Sphere | radius, --, -- | Tessellated sphere |
| Wedge | width, height, depth | Right-angle wedge (ramp) |
| Cone | radius, height, -- | Vertical cone with base at bottom |

## Related Systems

- [Physics System](../subsystems/Physics.md) -- CSG meshes can be used as collision geometry
- [Render Graph](../graphics/Render-Graph.md) -- CSG output meshes feed into the rendering pipeline
- [Editor Architecture](SparkEditor.md) -- CSGEditorPanel is one of the specialized editor panels
