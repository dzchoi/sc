// See LICENSE for license details.
//
// All configurations for the file-manager panel.

#pragma once

#include <cstdint>

using Rune = uint_least32_t;  // from st.h



// ----- NC-style panel layout -----
// The row structure inside the frames is:  | Name | Size | Date | Time |
constexpr int kMinColsName  = 12;  // classic DOS filename format 8.3
constexpr int kColsSize     = 7;  // "1048576" / "1023.9M" / "SUB-DIR"
constexpr int kColsDate     = 8;  // "MM/DD/YY"
constexpr int kColsTime     = 6;  // "HH:MMp"
constexpr int kRowsPanelFrame = 5;  // header (2) + footer (3)
constexpr int kMinRowsPanel = kRowsPanelFrame + 1;  // + footer (3)

constexpr int kMaxLenExt    = 5;

// ----- Panel geometry defaults -----
constexpr int kFracHeight   = 2;   // panel takes (N - 1) / N of terminal rows
constexpr int kFracWidth    = 2;   // panel takes half of terminal cols
constexpr int kMinRows      = 12;  // minimum terminal rows to show the panel
constexpr int kMinCols      = 80;  // minimum terminal cols to show the panel

// The first preprompt request must establish the panel snapshot before Shell::init()
// returns and normal input handling begins.
constexpr int kFirstPrepromptTimeoutMs = 1000;

static_assert(kFracWidth == 2);

// ----- colors (indices into the 256-color palette) --------------------------
// 0:  black
// 1:  red
// 2:  green
// 3:  yellow
// 4:  blue
// 5:  magenta
// 6:  cyan
// 7:  white (light gray)
// 8:  bright black (dark gray)
// 9:  bright red
// 10: bright green
// 11: bright yellow
// 12: bright blue
// 13: bright magenta
// 14: bright cyan
// 15: bright white
constexpr uint32_t kFgDefault   = 7;
constexpr uint32_t kBgDefault   = 4;
constexpr uint32_t kFgSelected  = 3;
constexpr uint32_t kFgFrame     = 6;

// ----- frame glyphs (Unicode box drawing) -----------------------------------
constexpr Rune kFrameH  = 0x2500;  // ─
constexpr Rune kFrameV  = 0x2502;  // │
constexpr Rune kFrameTL = 0x250c;  // ┌
constexpr Rune kFrameTR = 0x2510;  // ┐
constexpr Rune kFrameBL = 0x2514;  // └
constexpr Rune kFrameBR = 0x2518;  // ┘
constexpr Rune kFrameLT = 0x251c;  // ├
constexpr Rune kFrameRT = 0x2524;  // ┤
constexpr Rune kFrameTT = 0x252c;  // ┬
constexpr Rune kFrameBT = 0x2534;  // ┴

constexpr Rune kEllipsis = 0x2026;  // '…'
