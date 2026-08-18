// See LICENSE for license details.

#pragma once

#include <cstdint>              // for uint32_t
#include <string>               // for std::string, std::string_view
#include <utility>              // for std::forward()
#include <vector>               // for std::vector<>

extern "C" {
#include "st.h"                 // for Glyph, Rune, ATTR_*, ...
}



// Additional ATTR: Clears the entire region of a left/right/mid-aligned field.
constexpr ushort ATTR_CLEAR_FIELD = ATTR_WDUMMY << 1;

class Canvas;

class Draw {
public:
    Draw(Canvas& canvas);
    Draw(const Draw&) =delete;
    Draw& operator=(const Draw&) =delete;
    Draw(Draw&&) =default;
    Draw& operator=(Draw&&) =default;

    // Moves cursor.
    Draw& move(int x) { m_x = x; return *this; }
    Draw& move(int x, int y) { m_x = x; m_y = y; return *this; }

    Draw& color(uint32_t fg) { m_fg = fg; return *this; }
    Draw& color(uint32_t fg, uint32_t bg) { m_fg = fg; m_bg = bg; return *this; }

    // Sets up a field of `span` cells starting at the current cursor (m_x); the text
    // given to the next put() is positioned left/right/centered within the field if it
    // fits. If it doesn't fit and ellipsize() wasn't also called, it's hard-cut - from
    // the right, left, or both ends, respectively - to fit.
    Draw& left(int span) { m_align = Align::Left; m_span = span; return *this; }
    Draw& right(int span) { m_align = Align::Right; m_span = span; return *this; }
    Draw& mid(int span) { m_align = Align::Mid; m_span = span; return *this; }

    // Which part(s) of the text survive when it overflows the field, marking the cut
    // with a mid-string ellipsis ('…' U+2026) instead of a hard cut: Keep::Left/Right/
    // Mid keep the head/tail/middle slice (one ellipsis, in place of the discarded
    // part); Keep::Both keeps the head and, if the text has a short extension (<=
    // kMaxExtLen chars after the last '.', ignoring a leading dot as in ".bashrc"), the
    // extension too (one ellipsis in between, dot dropped) - with no usable extension,
    // Keep::Both behaves like Keep::Left. Once given, ellipsize() decides the cut on
    // its own; left()/mid()/right() then only affect where the text sits when it *fits*
    // the field.
    enum class Keep { Default, Left, Mid, Right, Both };
    Draw& ellipsize(Keep keep) { m_keep = keep; return *this; }

    // Puts a single glyph at (m_x, m_y).
    Draw& put(Rune u, ushort mode = ATTR_NULL);

    // Puts a UTF-8 text at (m_x, m_y) in a field (`m_span` cells wide); then advances
    // cursor and clears `m_span`.
    Draw& put(std::string_view s, ushort mode = ATTR_NULL);

    // Fills the whole field with a repeated glyph.
    Draw& fill(Rune u, ushort mode = ATTR_NULL);

    // Changes the color temporarily. E.g. draw.with_fg(3, [](Draw& d){ d.put("..."); })
    template <typename F>
    Draw& with_fg(uint32_t fg, F&& body) {
        uint32_t saved = m_fg;
        m_fg = fg;
        std::forward<F>(body)(*this);
        m_fg = saved;
        return *this;
    }

private:
    enum class Align { Left, Right, Mid };

    // Handles a Left field's Keep::Left/Both ellipsis case for text overflowing the
    // `xend`-cell boundary, writing glyphs and advancing m_x as it goes. Returns false
    // without writing anything if the text already fits the field - the caller should
    // fall back to normal left-aligned streaming in that case.
    bool put_left_ellipsized(std::string_view s, int xend, ushort mode);

    Canvas& m_canvas;
    int m_x = 0, m_y = 0;  // cursor within the panel
    uint32_t m_fg = 0, m_bg = 0;
    Align m_align = Align::Left;
    Keep m_keep = Keep::Default;
    int m_span;  // field width
};

// Canvas: A rectangular drawing surface positioned within the terminal grid.
//
// It owns its coordinates (`left`), boundaries (`width` x `height`), and backing line
// buffer, exposing drawing primitives in surface-local space where x is in [0, width)
// and y is in [0, height). 
//
// To support absolute row-indexing functions like `xdrawline()`, the buffer is allocated
// at the terminal's full column count. This ensures that `row_ptr(y)` is safely
// indexable up to `left + width` regardless of where the canvas is horizontally anchored.

class Canvas {
public:
    Canvas() =default;
    Canvas(const Canvas&) =delete;
    Canvas& operator=(const Canvas&) =delete;
    Canvas(Canvas&&) =default;
    Canvas& operator=(Canvas&&) =default;

    // (Re)positions or resizes the surface.
    void reset(int top, int left, int width, int height, int term_cols);

    int top()    const { return m_top; }
    int left()   const { return m_left; }
    int width()  const { return m_width; }
    int height() const { return m_height; }

    Draw draw() { return Draw(*this); }

    // Presents every row via xdrawline(). No-op if m_width/m_height are 0.
    void present();

private:
    inline static std::vector<Glyph> m_linebuf;  // m_height * m_term_cols glyphs

    friend class Draw;

    inline static int m_term_cols = 0;
    int m_top = 0;
    int m_left = 0;
    int m_width = 0;
    int m_height = 0;

    // Raw pointer to row y, for handing to xdrawline(row_ptr(y), left(), y, right()).
    static Glyph* row_ptr(int y) { return m_linebuf.data() + y * m_term_cols; }

    // Reference to the glyph at surface-local (x, y), i.e. row_ptr(y)[m_left + x].
    Glyph& cell(int y, int x) { return row_ptr(y)[m_left + x]; }
};
