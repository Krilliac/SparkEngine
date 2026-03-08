/**
 * @file EditorIcons.h
 * @brief FontAwesome 6 icon constants for the Spark Engine Editor
 * @author Spark Engine Team
 * @date 2025
 *
 * Icon codepoints from FontAwesome 6 Free Solid (fa-solid-900.ttf).
 * These map to UTF-8 encoded strings for use with ImGui text rendering.
 */

#pragma once

// FontAwesome 6 glyph range
#define ICON_FA_MIN 0xf000
#define ICON_FA_MAX 0xf8ff

// === Transport / Play Controls ===
#define ICON_FA_PLAY "\xef\x81\x8b"         // U+F04B
#define ICON_FA_PAUSE "\xef\x81\x8c"        // U+F04C
#define ICON_FA_STOP "\xef\x81\x8d"         // U+F04D
#define ICON_FA_FORWARD "\xef\x81\x8e"      // U+F04E
#define ICON_FA_BACKWARD "\xef\x81\x8a"     // U+F04A
#define ICON_FA_STEP_FORWARD "\xef\x81\x91" // U+F051

// === Transform Tools ===
#define ICON_FA_ARROWS_ALT "\xef\x81\xb2" // U+F0B2 (move)
#define ICON_FA_SYNC_ALT "\xef\x80\xa1"   // U+F021 (rotate)
#define ICON_FA_EXPAND_ALT "\xef\x81\xa4" // U+F064 (scale - expand)
#define ICON_FA_EXPAND "\xef\x81\xa5"     // U+F065

// === Scene Objects ===
#define ICON_FA_CUBE "\xef\x86\xb2"          // U+F1B2
#define ICON_FA_CAMERA "\xef\x80\xb0"        // U+F030
#define ICON_FA_LIGHTBULB "\xef\x83\xab"     // U+F0EB
#define ICON_FA_SUN "\xef\x86\x85"           // U+F185
#define ICON_FA_GLOBE "\xef\x82\xac"         // U+F0AC
#define ICON_FA_CROSSHAIRS "\xef\x81\x9b"    // U+F05B
#define ICON_FA_USER "\xef\x80\x87"          // U+F007
#define ICON_FA_MOUNTAIN "\xef\x9b\xbc"      // U+F6FC
#define ICON_FA_TREE "\xef\x86\xbb"          // U+F1BB
#define ICON_FA_CIRCLE "\xef\x84\x91"        // U+F111
#define ICON_FA_VECTOR_SQUARE "\xef\x97\xab" // U+F5CB

// === File System ===
#define ICON_FA_FOLDER "\xef\x81\xbc"      // U+F07C
#define ICON_FA_FOLDER_OPEN "\xef\x81\x96" // U+F076 (alt open folder)
#define ICON_FA_FILE "\xef\x85\x9b"        // U+F15B
#define ICON_FA_FILE_CODE "\xef\x87\x89"   // U+F1C9
#define ICON_FA_FILE_IMAGE "\xef\x87\x85"  // U+F1C5
#define ICON_FA_FILE_AUDIO "\xef\x87\x87"  // U+F1C7
#define ICON_FA_IMAGE "\xef\x80\xbe"       // U+F03E

// === Actions ===
#define ICON_FA_SAVE "\xef\x83\x87"     // U+F0C7
#define ICON_FA_UNDO "\xef\x83\xa2"     // U+F0E2
#define ICON_FA_REDO "\xef\x80\x9e"     // U+F01E
#define ICON_FA_TRASH "\xef\x87\xb8"    // U+F1F8
#define ICON_FA_COPY "\xef\x83\x85"     // U+F0C5
#define ICON_FA_PLUS "\xef\x81\xa7"     // U+F067
#define ICON_FA_MINUS "\xef\x81\xa8"    // U+F068
#define ICON_FA_SEARCH "\xef\x80\x82"   // U+F002
#define ICON_FA_COG "\xef\x80\x93"      // U+F013
#define ICON_FA_COGS "\xef\x82\x85"     // U+F085
#define ICON_FA_DOWNLOAD "\xef\x80\x99" // U+F019
#define ICON_FA_UPLOAD "\xef\x82\x93"   // U+F093

// === View / Visibility ===
#define ICON_FA_EYE "\xef\x81\xae"        // U+F06E
#define ICON_FA_EYE_SLASH "\xef\x81\xb0"  // U+F070
#define ICON_FA_LOCK "\xef\x80\xa3"       // U+F023
#define ICON_FA_UNLOCK "\xef\x82\x9c"     // U+F09C
#define ICON_FA_TH "\xef\x80\x8a"         // U+F00A (grid)
#define ICON_FA_BORDER_ALL "\xef\xa1\x8c" // U+F84C

// === FPS HUD ===
#define ICON_FA_HEART "\xef\x80\x84"     // U+F004
#define ICON_FA_HEARTBEAT "\xef\x88\x9e" // U+F21E
#define ICON_FA_SKULL "\xef\x95\x8c"     // U+F54C

// === Status / Feedback ===
#define ICON_FA_CHECK "\xef\x80\x8c"       // U+F00C
#define ICON_FA_TIMES "\xef\x80\x8d"       // U+F00D
#define ICON_FA_EXCLAMATION "\xef\x84\xaa" // U+F12A
#define ICON_FA_INFO_CIRCLE "\xef\x81\x9a" // U+F05A
#define ICON_FA_BUG "\xef\x86\x88"         // U+F188
#define ICON_FA_BOLT "\xef\x83\xa7"        // U+F0E7 (spark!)
#define ICON_FA_FIRE "\xef\x81\xad"        // U+F06D

// === Panels ===
#define ICON_FA_SITEMAP "\xef\x83\xa8"   // U+F0E8 (hierarchy)
#define ICON_FA_SLIDERS "\xef\x87\x9e"   // U+F1DE (inspector/properties)
#define ICON_FA_TERMINAL "\xef\x84\xa0"  // U+F120 (console)
#define ICON_FA_CHART_BAR "\xef\x82\x80" // U+F080 (profiler)
#define ICON_FA_PALETTE "\xef\x96\xbf"   // U+F53F (material editor)
#define ICON_FA_MAP "\xef\x89\xb9"       // U+F279 (terrain)
#define ICON_FA_GAMEPAD "\xef\x84\x9b"   // U+F11B (game view)
#define ICON_FA_HAMMER "\xef\x9b\xa3"    // U+F6E3 (build)

// === FPS-Specific ===
#define ICON_FA_BOMB "\xef\x87\xa2"           // U+F1E2 (explosive)
#define ICON_FA_BULLSEYE "\xef\x85\x80"       // U+F140 (target/objective)
#define ICON_FA_FLAG "\xef\x80\xa4"           // U+F024 (spawn/flag)
#define ICON_FA_SHIELD "\xef\x84\xb2"         // U+F132 (armor)
#define ICON_FA_ROCKET "\xef\x84\xb5"         // U+F135
#define ICON_FA_LOCATION_ARROW "\xef\x84\xa4" // U+F124

// === Arrows / Navigation ===
#define ICON_FA_CHEVRON_RIGHT "\xef\x81\x94" // U+F054
#define ICON_FA_CHEVRON_DOWN "\xef\x81\xb8"  // U+F078
#define ICON_FA_CHEVRON_LEFT "\xef\x81\x93"  // U+F053
#define ICON_FA_ANGLE_RIGHT "\xef\x84\x85"   // U+F105
#define ICON_FA_ANGLE_DOWN "\xef\x84\x87"    // U+F107
#define ICON_FA_BARS "\xef\x83\x89"          // U+F0C9 (hamburger menu)

// === Layout / Window ===
#define ICON_FA_COLUMNS "\xef\x83\x9b"         // U+F0DB
#define ICON_FA_WINDOW_MAXIMIZE "\xef\x8b\x90" // U+F2D0
#define ICON_FA_COMPRESS "\xef\x81\xa6"        // U+F066

// === Additional Icons ===
#define ICON_FA_CODE "\xef\x84\xa1"          // U+F121
#define ICON_FA_FONT "\xef\x80\xb1"          // U+F031
#define ICON_FA_GRID "\xef\x80\x8a"          // U+F00A (alias for TH/grid)
#define ICON_FA_HOME "\xef\x80\x95"          // U+F015
#define ICON_FA_MOUSE_POINTER "\xef\x89\x85" // U+F245
#define ICON_FA_PAINT_BRUSH "\xef\x87\xbc"   // U+F1FC
#define ICON_FA_SHAPES "\xef\x98\x9f"        // U+F61F
#define ICON_FA_VOLUME_UP "\xef\x80\xa8"     // U+F028
