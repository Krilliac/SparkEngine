/**
 * @file EditorIcons.h
 * @brief Font Awesome 6 Free Solid icon constants for the Spark Engine Editor.
 *
 * Each define is the UTF-8 encoding of one codepoint in fa-solid-900.ttf (SparkEditor/Fonts), which
 * Spark::Ui::LoadFontSet merges into every face for the private-use block ICON_FA_MIN..ICON_FA_MAX.
 * The trailing comment names the codepoint and the font's own glyph name ("U+F07C folder-open");
 * tools/ui/check_icon_glyphs.py fails when the bytes, the comment and the font disagree or when a
 * codepoint is missing from the font, so a wrong escape can no longer render another icon.
 */

#pragma once

// FontAwesome 6 glyph range
#define ICON_FA_MIN 0xf000
#define ICON_FA_MAX 0xf8ff

// === Transport / Play Controls ===
#define ICON_FA_PLAY "\xef\x81\x8b"         // U+F04B play
#define ICON_FA_PAUSE "\xef\x81\x8c"        // U+F04C pause
#define ICON_FA_STOP "\xef\x81\x8d"         // U+F04D stop
#define ICON_FA_FORWARD "\xef\x81\x8e"      // U+F04E forward
#define ICON_FA_BACKWARD "\xef\x81\x8a"     // U+F04A backward
#define ICON_FA_STEP_FORWARD "\xef\x81\x91" // U+F051 forward-step

// === Transform Tools ===
#define ICON_FA_ARROWS_ALT "\xef\x82\xb2" // U+F0B2 up-down-left-right (move)
#define ICON_FA_SYNC_ALT "\xef\x80\xa1"   // U+F021 arrows-rotate (rotate)
#define ICON_FA_EXPAND_ALT "\xef\x90\xa4" // U+F424 up-right-and-down-left-from-center (scale - expand)
#define ICON_FA_EXPAND "\xef\x81\xa5"     // U+F065 expand

// === Scene Objects ===
#define ICON_FA_CUBE "\xef\x86\xb2"          // U+F1B2 cube
#define ICON_FA_CAMERA "\xef\x80\xb0"        // U+F030 camera
#define ICON_FA_LIGHTBULB "\xef\x83\xab"     // U+F0EB lightbulb
#define ICON_FA_SUN "\xef\x86\x85"           // U+F185 sun
#define ICON_FA_GLOBE "\xef\x82\xac"         // U+F0AC globe
#define ICON_FA_CROSSHAIRS "\xef\x81\x9b"    // U+F05B crosshairs
#define ICON_FA_USER "\xef\x80\x87"          // U+F007 user
#define ICON_FA_MOUNTAIN "\xef\x9b\xbc"      // U+F6FC mountain
#define ICON_FA_TREE "\xef\x86\xbb"          // U+F1BB tree
#define ICON_FA_CIRCLE "\xef\x84\x91"        // U+F111 circle
#define ICON_FA_VECTOR_SQUARE "\xef\x97\x8b" // U+F5CB vector-square

// === File System ===
#define ICON_FA_FOLDER "\xef\x81\xbb"      // U+F07B folder
#define ICON_FA_FOLDER_OPEN "\xef\x81\xbc" // U+F07C folder-open
#define ICON_FA_FILE "\xef\x85\x9b"        // U+F15B file
#define ICON_FA_FILE_CODE "\xef\x87\x89"   // U+F1C9 file-code
#define ICON_FA_FILE_IMAGE "\xef\x87\x85"  // U+F1C5 file-image
#define ICON_FA_FILE_AUDIO "\xef\x87\x87"  // U+F1C7 file-audio
#define ICON_FA_IMAGE "\xef\x80\xbe"       // U+F03E image

// === Actions ===
#define ICON_FA_EDIT "\xef\x81\x84"     // U+F044 pen-to-square
#define ICON_FA_SAVE "\xef\x83\x87"     // U+F0C7 floppy-disk
#define ICON_FA_UNDO "\xef\x83\xa2"     // U+F0E2 arrow-rotate-left
#define ICON_FA_REDO "\xef\x80\x9e"     // U+F01E arrow-rotate-right
#define ICON_FA_TRASH "\xef\x87\xb8"    // U+F1F8 trash
#define ICON_FA_COPY "\xef\x83\x85"     // U+F0C5 copy
#define ICON_FA_PLUS "\xef\x81\xa7"     // U+F067 plus
#define ICON_FA_MINUS "\xef\x81\xa8"    // U+F068 minus
#define ICON_FA_SEARCH "\xef\x80\x82"   // U+F002 magnifying-glass
#define ICON_FA_COG "\xef\x80\x93"      // U+F013 gear
#define ICON_FA_COGS "\xef\x82\x85"     // U+F085 gears
#define ICON_FA_DOWNLOAD "\xef\x80\x99" // U+F019 download
#define ICON_FA_UPLOAD "\xef\x82\x93"   // U+F093 upload
#define ICON_FA_EXCHANGE "\xef\x83\xac" // U+F0EC arrow-right-arrow-left

// === View / Visibility ===
#define ICON_FA_EYE "\xef\x81\xae"        // U+F06E eye
#define ICON_FA_EYE_SLASH "\xef\x81\xb0"  // U+F070 eye-slash
#define ICON_FA_LOCK "\xef\x80\xa3"       // U+F023 lock
#define ICON_FA_UNLOCK "\xef\x82\x9c"     // U+F09C unlock
#define ICON_FA_TH "\xef\x80\x8a"         // U+F00A table-cells (grid)
#define ICON_FA_BORDER_ALL "\xef\xa1\x8c" // U+F84C border-all

// === FPS HUD ===
#define ICON_FA_HEART "\xef\x80\x84"     // U+F004 heart
#define ICON_FA_HEARTBEAT "\xef\x88\x9e" // U+F21E heart-pulse
#define ICON_FA_SKULL "\xef\x95\x8c"     // U+F54C skull

// === Status / Feedback ===
#define ICON_FA_CHECK "\xef\x80\x8c"       // U+F00C check
#define ICON_FA_TIMES "\xef\x80\x8d"       // U+F00D xmark
#define ICON_FA_EXCLAMATION "\xef\x84\xaa" // U+F12A exclamation
#define ICON_FA_INFO_CIRCLE "\xef\x81\x9a" // U+F05A circle-info
#define ICON_FA_BUG "\xef\x86\x88"         // U+F188 bug
#define ICON_FA_BOLT "\xef\x83\xa7"        // U+F0E7 bolt (spark!)
#define ICON_FA_FIRE "\xef\x81\xad"        // U+F06D fire

// === Font Awesome 6 names used by the Spark UI status and chrome widgets ===
#define ICON_FA_CIRCLE_CHECK "\xef\x81\x98"         // U+F058 circle-check
#define ICON_FA_TRIANGLE_EXCLAMATION "\xef\x81\xb1" // U+F071 triangle-exclamation
#define ICON_FA_CIRCLE_XMARK "\xef\x81\x97"         // U+F057 circle-xmark
#define ICON_FA_CIRCLE_INFO "\xef\x81\x9a"          // U+F05A circle-info
#define ICON_FA_CIRCLE_QUESTION "\xef\x81\x99"      // U+F059 circle-question
#define ICON_FA_MAGNIFYING_GLASS "\xef\x80\x82"     // U+F002 magnifying-glass
#define ICON_FA_ELLIPSIS "\xef\x85\x81"             // U+F141 ellipsis
#define ICON_FA_XMARK "\xef\x80\x8d"                // U+F00D xmark

// === Communication / Dialogue ===
#define ICON_FA_COMMENT "\xef\x81\xb5"  // U+F075 comment
#define ICON_FA_COMMENTS "\xef\x82\x86" // U+F086 comments

// === Panels ===
#define ICON_FA_SITEMAP "\xef\x83\xa8"   // U+F0E8 sitemap (hierarchy)
#define ICON_FA_SLIDERS "\xef\x87\x9e"   // U+F1DE sliders (inspector/properties)
#define ICON_FA_TERMINAL "\xef\x84\xa0"  // U+F120 terminal (console)
#define ICON_FA_CHART_BAR "\xef\x82\x80" // U+F080 chart-bar (profiler)
#define ICON_FA_PALETTE "\xef\x94\xbf"   // U+F53F palette (material editor)
#define ICON_FA_MAP "\xef\x89\xb9"       // U+F279 map (terrain)
#define ICON_FA_GAMEPAD "\xef\x84\x9b"   // U+F11B gamepad (game view)
#define ICON_FA_HAMMER "\xef\x9b\xa3"    // U+F6E3 hammer (build)

// === FPS-Specific ===
#define ICON_FA_BOMB "\xef\x87\xa2"           // U+F1E2 bomb (explosive)
#define ICON_FA_BULLSEYE "\xef\x85\x80"       // U+F140 bullseye (target/objective)
#define ICON_FA_FLAG "\xef\x80\xa4"           // U+F024 flag (spawn/flag)
#define ICON_FA_SHIELD "\xef\x84\xb2"         // U+F132 shield (armor)
#define ICON_FA_ROCKET "\xef\x84\xb5"         // U+F135 rocket
#define ICON_FA_LOCATION_ARROW "\xef\x84\xa4" // U+F124 location-arrow

// === Arrows / Navigation ===
#define ICON_FA_LEVEL_UP_ALT "\xef\x8e\xbf"  // U+F3BF turn-up
#define ICON_FA_CHEVRON_RIGHT "\xef\x81\x94" // U+F054 chevron-right
#define ICON_FA_CHEVRON_DOWN "\xef\x81\xb8"  // U+F078 chevron-down
#define ICON_FA_CHEVRON_LEFT "\xef\x81\x93"  // U+F053 chevron-left
#define ICON_FA_ANGLE_RIGHT "\xef\x84\x85"   // U+F105 angle-right
#define ICON_FA_ANGLE_DOWN "\xef\x84\x87"    // U+F107 angle-down
#define ICON_FA_BARS "\xef\x83\x89"          // U+F0C9 bars (hamburger menu)

// === Layout / Window ===
#define ICON_FA_COLUMNS "\xef\x83\x9b"         // U+F0DB table-columns
#define ICON_FA_WINDOW_MAXIMIZE "\xef\x8b\x90" // U+F2D0 window-maximize
#define ICON_FA_COMPRESS "\xef\x81\xa6"        // U+F066 compress

// === Tools / Snap ===
#define ICON_FA_MAGNET "\xef\x81\xb6" // U+F076 magnet

// === Metrics / Data ===
#define ICON_FA_TACHOMETER_ALT "\xef\x8f\xbd" // U+F3FD gauge-high
#define ICON_FA_DATABASE "\xef\x87\x80"       // U+F1C0 database
#define ICON_FA_MEMORY "\xef\x94\xb8"         // U+F538 memory

// === Additional Icons ===
#define ICON_FA_RUNNING "\xef\x9c\x8c"       // U+F70C person-running
#define ICON_FA_CODE "\xef\x84\xa1"          // U+F121 code
#define ICON_FA_CUBES "\xef\x86\xb3"         // U+F1B3 cubes
#define ICON_FA_WIFI "\xef\x87\xab"          // U+F1EB wifi
#define ICON_FA_FONT "\xef\x80\xb1"          // U+F031 font
#define ICON_FA_GRID "\xef\x80\x8a"          // U+F00A table-cells (alias for TH/grid)
#define ICON_FA_HOME "\xef\x80\x95"          // U+F015 house
#define ICON_FA_MOUSE_POINTER "\xef\x89\x85" // U+F245 arrow-pointer
#define ICON_FA_PAINT_BRUSH "\xef\x87\xbc"   // U+F1FC paintbrush
#define ICON_FA_SHAPES "\xef\x98\x9f"        // U+F61F shapes
#define ICON_FA_VOLUME_UP "\xef\x80\xa8"     // U+F028 volume-high

// === 2D Editor Icons ===
#define ICON_FA_ERASER "\xef\x84\xad"       // U+F12D eraser
#define ICON_FA_FILL_DRIP "\xef\x95\xb6"    // U+F576 fill-drip
#define ICON_FA_EYE_DROPPER "\xef\x87\xbb"  // U+F1FB eye-dropper
#define ICON_FA_SHIELD_ALT "\xef\x8f\xad"   // U+F3ED shield-halved
#define ICON_FA_SEARCH_PLUS "\xef\x80\x8e"  // U+F00E magnifying-glass-plus
#define ICON_FA_SEARCH_MINUS "\xef\x80\x90" // U+F010 magnifying-glass-minus
#define ICON_FA_MAGIC "\xef\x83\x90"        // U+F0D0 wand-magic
#define ICON_FA_LAYER_GROUP "\xef\x97\xbd"  // U+F5FD layer-group
#define ICON_FA_CROP "\xef\x84\xa5"         // U+F125 crop

// === Prefab & History Icons ===
#define ICON_FA_HISTORY "\xef\x87\x9a"  // U+F1DA clock-rotate-left
#define ICON_FA_BOX "\xef\x91\xa6"      // U+F466 box
#define ICON_FA_BOXES "\xef\x91\xa8"    // U+F468 boxes-stacked
#define ICON_FA_CLOCK "\xef\x80\x97"    // U+F017 clock
#define ICON_FA_LIST_ALT "\xef\x80\xa2" // U+F022 rectangle-list

// === Animation Editor Icons ===
#define ICON_FA_FILM "\xef\x80\x88"        // U+F008 film
#define ICON_FA_STREAM "\xef\x95\x90"      // U+F550 bars-staggered
#define ICON_FA_PLUS_SQUARE "\xef\x83\xbe" // U+F0FE square-plus
#define ICON_FA_SLIDERS_H "\xef\x87\x9e"   // U+F1DE sliders (horizontal sliders)

// === Networking / Server Icons ===
#define ICON_FA_SERVER "\xef\x88\xb3"        // U+F233 server
#define ICON_FA_PLUG "\xef\x87\xa6"          // U+F1E6 plug
#define ICON_FA_USERS "\xef\x83\x80"         // U+F0C0 users
#define ICON_FA_SPINNER "\xef\x84\x90"       // U+F110 spinner
#define ICON_FA_REFRESH "\xef\x80\xa1"       // U+F021 arrows-rotate
#define ICON_FA_NETWORK_WIRED "\xef\x9b\xbf" // U+F6FF network-wired

// === Weather Icons ===
#define ICON_FA_CLOUD "\xef\x83\x82"      // U+F0C2 cloud
#define ICON_FA_CLOUD_RAIN "\xef\x9c\xbd" // U+F73D cloud-rain
#define ICON_FA_CLOUD_SUN "\xef\x9b\x84"  // U+F6C4 cloud-sun
#define ICON_FA_SNOWFLAKE "\xef\x8b\x9c"  // U+F2DC snowflake
#define ICON_FA_SMOG "\xef\x9d\x9f"       // U+F75F smog
#define ICON_FA_WIND "\xef\x9c\xae"       // U+F72E wind

// === Editor Tool Icons ===
#define ICON_FA_ADJUST "\xef\x81\x82"       // U+F042 circle-half-stroke
#define ICON_FA_ARROW_UP "\xef\x81\xa2"     // U+F062 arrow-up
#define ICON_FA_ARROW_DOWN "\xef\x81\xa3"   // U+F063 arrow-down
#define ICON_FA_ARROW_RIGHT "\xef\x81\xa1"  // U+F061 arrow-right
#define ICON_FA_FILTER "\xef\x82\xb0"       // U+F0B0 filter
#define ICON_FA_LIST "\xef\x80\xba"         // U+F03A list
#define ICON_FA_QUESTION "\xef\x84\xa8"     // U+F128 question
#define ICON_FA_HAND_POINTER "\xef\x89\x9a" // U+F25A hand-pointer
#define ICON_FA_STAMP "\xef\x96\xbf"        // U+F5BF stamp
#define ICON_FA_TH_LARGE "\xef\x80\x89"     // U+F009 table-cells-large

// === AI / Logic Icons ===
#define ICON_FA_BRAIN "\xef\x97\x9c"           // U+F5DC brain
#define ICON_FA_PROJECT_DIAGRAM "\xef\x95\x82" // U+F542 diagram-project
#define ICON_FA_CODE_BRANCH "\xef\x84\xa6"     // U+F126 code-branch
#define ICON_FA_CHALKBOARD "\xef\x94\x9b"      // U+F51B chalkboard

// === Path / Spline Icons ===
#define ICON_FA_BEZIER_CURVE "\xef\x95\x9b" // U+F55B bezier-curve
#define ICON_FA_ROUTE "\xef\x93\x97"        // U+F4D7 route

// === Volume / Probe Icons ===
#define ICON_FA_WATER "\xef\x9d\xb3"      // U+F773 water
#define ICON_FA_DOT_CIRCLE "\xef\x86\x92" // U+F192 circle-dot

// === Module / Plugin Icons ===
#define ICON_FA_PUZZLE_PIECE "\xef\x84\xae" // U+F12E puzzle-piece

// === Settings / Category Icons ===
#define ICON_FA_DESKTOP "\xef\x84\x88"              // U+F108 desktop
#define ICON_FA_ATOM "\xef\x97\x92"                 // U+F5D2 atom
#define ICON_FA_VIDEO "\xef\x80\xbd"                // U+F03D video
#define ICON_FA_PENCIL_ALT "\xef\x8c\x83"           // U+F303 pencil
#define ICON_FA_EXCLAMATION_TRIANGLE "\xef\x81\xb1" // U+F071 triangle-exclamation
#define ICON_FA_PEN "\xef\x8c\x84"                  // U+F304 pen
#define ICON_FA_FILE_EXPORT "\xef\x95\xae"          // U+F56E file-export
#define ICON_FA_FILE_IMPORT "\xef\x95\xaf"          // U+F56F file-import
#define ICON_FA_BOOKMARK "\xef\x80\xae"             // U+F02E bookmark
#define ICON_FA_CHECK_CIRCLE "\xef\x81\x98"         // U+F058 circle-check
#define ICON_FA_HEADPHONES "\xef\x80\xa5"           // U+F025 headphones
#define ICON_FA_WAVE_SQUARE "\xef\xa0\xbe"          // U+F83E wave-square
