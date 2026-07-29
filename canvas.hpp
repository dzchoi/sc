// See LICENSE for license details.

#pragma once

#include <string>
#include <vector>

extern "C" {
#include "st.h"         // Line, Glyph, Rune, ushort, ATTR_*
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
    Draw& move(int x) { x_ = x; return *this; }
    Draw& move(int x, int y) { x_ = x; y_ = y; return *this; }

    Draw& color(uint32_t fg) { fg_ = fg; return *this; }
    Draw& color(uint32_t fg, uint32_t bg) { fg_ = fg; bg_ = bg; return *this; }

    // Sets up a field of `span` cells starting at the current cursor (x_); the text
    // given to the next put() is positioned left/right/centered within the field if it
    // fits. If it doesn't fit and ellipsize() wasn't also called, it's hard-cut - from
    // the right, left, or both ends, respectively - to fit.
    Draw& left(int span) { align_ = Align::Left; span_ = span; return *this; }
    Draw& right(int span) { align_ = Align::Right; span_ = span; return *this; }
    Draw& mid(int span) { align_ = Align::Mid; span_ = span; return *this; }

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
    Draw& ellipsize(Keep keep) { keep_ = keep; return *this; }

    // Puts a single glyph at (x_, y_).
    Draw& put(Rune u, ushort mode = ATTR_NULL);

    // Puts a UTF-8 text at (x_, y_) in a field (`span_` cells wide); then advances
    // cursor and clears `span_`.
    Draw& put(std::string_view s, ushort mode = ATTR_NULL);

    // Fills the whole field with a repeated glyph.
    Draw& fill(Rune u, ushort mode = ATTR_NULL);

    // Changes the color temporarily. E.g. draw.with_fg(3, [](Draw& d){ d.put("..."); })
    template <typename F>
    Draw& with_fg(uint32_t fg, F&& body) {
        uint32_t saved = fg_;
        fg_ = fg;
        std::forward<F>(body)(*this);
        fg_ = saved;
        return *this;
    }

private:
    enum class Align { Left, Right, Mid };

    // Handles a Left field's Keep::Left/Both ellipsis case for text overflowing the
    // `xend`-cell boundary, writing glyphs and advancing x_ as it goes. Returns false
    // without writing anything if the text already fits the field - the caller should
    // fall back to normal left-aligned streaming in that case.
    bool put_left_ellipsized(std::string_view s, int xend, ushort mode);

    Canvas& canvas_;
    int x_ = 0, y_ = 0;  // cursor within the panel
    uint32_t fg_ = 0, bg_ = 0;
    Align align_ = Align::Left;
    Keep keep_ = Keep::Default;
    int span_;  // field width
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

    int top()    const { return top_; }
    int left()   const { return left_; }
    int width()  const { return width_; }
    int height() const { return height_; }

    Draw draw() { return Draw(*this); }

    // Presents every row via xdrawline(). No-op if width_/height_ are 0.
    // Todo: Make present()/row_ptr() const.
    void present();

private:
    inline static std::vector<Glyph> linebuf_;  // height_ * term_cols_ glyphs

    friend class Draw;

    inline static int term_cols_ = 0;
    int top_ = 0;
    int left_ = 0;
    int width_ = 0;
    int height_ = 0;

    // Raw pointer to row y, for handing to xdrawline(row_ptr(y), left(), y, right()).
    static Glyph* row_ptr(int y) { return linebuf_.data() + y * term_cols_; }

    // Reference to the glyph at surface-local (x, y), i.e. row_ptr(y)[left_ + x].
    Glyph& cell(int y, int x) { return row_ptr(y)[left_ + x]; }
};
