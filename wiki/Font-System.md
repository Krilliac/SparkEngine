# Font System

Runtime font rendering system with TrueType loading, glyph atlas generation, and batched text layout for HUDs, dialogue, and UI overlays.

**Source:** `SparkEngine/Source/Engine/Text/FontSystem.h`

## Overview

The Font System provides a complete text rendering pipeline built on top of stb_truetype. It loads TrueType fonts from disk, rasterizes glyphs into a packed atlas texture, and produces per-glyph quads that the renderer can batch into draw calls. Each font is loaded at a specific pixel size and assigned a unique `FontId` for later reference.

Text layout supports word wrapping via `maxWidth`, configurable line spacing and letter spacing, and left/center/right alignment. The system can measure text bounds without generating quads, which is useful for UI layout calculations. Bold and italic styles are synthesized (weight adjustment and shear transform) rather than requiring separate font files.

The glyph atlas uses a simple row-based packing algorithm. When a row fills up, the packer advances to the next row. If the atlas runs out of space a warning is logged. ASCII glyphs (32-126) are pre-rasterized by default on load; additional glyphs are rasterized on demand. The atlas exposes a `dirty` flag so the renderer knows when to re-upload the texture to the GPU.

## Key Classes

| Class / Struct | Description |
|---|---|
| `FontSystem` | Singleton that manages font loading, glyph rasterization, text layout, and measurement |
| `TextStyle` | Parameters for text rendering: font, size, color, alignment, spacing, bold/italic |
| `GlyphMetrics` | Per-glyph metrics including advance, bearing, dimensions, and atlas UV coordinates |
| `TextQuad` | Output quad for a single visible glyph with screen position, UVs, and color |
| `TextBounds` | Bounding box result from `MeasureText` including width, height, and line count |
| `GlyphAtlas` | Single-channel bitmap atlas with packing state and dirty flag |
| `FontInfo` | Metadata for a loaded font: name, path, size, ascent, descent, glyph count |

## Usage

```cpp
auto& fonts = Spark::Text::FontSystem::GetInstance();
fonts.Initialize();

// Load a font at 24px
auto fontId = fonts.LoadFont("Assets/Fonts/Roboto.ttf", 24.0f);

// Configure text style
Spark::Text::TextStyle style;
style.fontId = fontId;
style.fontSize = 32.0f;
style.color = {1.0f, 0.8f, 0.0f, 1.0f};
style.alignment = Spark::Text::TextAlignment::Center;
style.maxWidth = 400.0f;  // enable word wrapping

// Generate quads for rendering
auto quads = fonts.LayoutText("Hello, World!", 100.0f, 200.0f, style);
// Submit quads to sprite batch or text renderer...

// Measure without rendering (useful for UI layout)
auto bounds = fonts.MeasureText("Score: 1234", style);
float boxWidth = bounds.width;
float boxHeight = bounds.height;

// Check if atlas needs GPU re-upload
if (auto* atlas = fonts.GetAtlas(fontId); atlas && atlas->dirty)
{
    UploadAtlasTexture(atlas->pixels.data(), atlas->width, atlas->height);
}
```

## API Reference

### FontSystem Methods

| Method | Return | Description |
|---|---|---|
| `Initialize()` | `void` | Initialize the font system and clear all state |
| `Shutdown()` | `void` | Release all fonts and shut down |
| `LoadFont(path, size, preloadAscii)` | `FontId` | Load a TrueType font file at the given pixel size |
| `UnloadFont(fontId)` | `void` | Unload a font and free its atlas |
| `SetActiveFont(fontId)` | `void` | Set the default font for layout calls |
| `GetActiveFont()` | `FontId` | Get the current default font |
| `LayoutText(text, x, y, style)` | `vector<TextQuad>` | Layout text into renderable glyph quads |
| `MeasureText(text, style)` | `TextBounds` | Measure text bounding box without generating quads |
| `GetFontInfo(fontId)` | `const FontInfo*` | Get metadata for a loaded font |
| `GetAtlas(fontId)` | `const GlyphAtlas*` | Get atlas texture data for GPU upload |
| `GetFontCount()` | `size_t` | Number of loaded fonts |
| `GetLoadedFonts()` | `vector<FontId>` | All loaded font IDs |
| `SetDefaultAtlasSize(size)` | `void` | Set atlas dimensions for newly loaded fonts |

### TextStyle Fields

| Field | Type | Default | Description |
|---|---|---|---|
| `fontId` | `FontId` | `INVALID_FONT_ID` | Font to use (0 uses active font) |
| `fontSize` | `float` | `24.0f` | Render size in pixels |
| `color` | `TextColor` | White | RGBA text color |
| `alignment` | `TextAlignment` | `Left` | Horizontal alignment |
| `lineSpacing` | `float` | `1.2f` | Line height multiplier |
| `letterSpacing` | `float` | `0.0f` | Extra space between characters |
| `maxWidth` | `float` | `0.0f` | Max width for word wrapping (0 = no wrap) |
| `bold` | `bool` | `false` | Synthesized bold |
| `italic` | `bool` | `false` | Synthesized italic (shear) |

## Related Systems

- [UI System](UI-System.md) -- uses FontSystem for in-game UI text rendering
- [2D Rendering](2D-Systems.md) -- sprite batch can render text quads
- [Localization System](Localization-System.md) -- provides localized strings for text layout
- [Dialogue System](Dialogue-System.md) -- renders dialogue text via FontSystem
