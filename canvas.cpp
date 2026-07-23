#include <cassert>
#include <cstddef>

#include "canvas.hpp"

extern "C" {
#include "win.h"        // xdrawline()
}

namespace {

// Minimal UTF-8 decoder. Returns bytes consumed; writes a code point (unicode char) to
// *out. Never reads past `pend`, and never reports a length that would run past it
// either.
int utf8_next(const unsigned char* p, const unsigned char* pend, Rune* out)
{
    if (*p < 0x80) {
        *out = *p;
        return 1;
    }

    const ptrdiff_t avail = pend - p;
    if ((*p & 0xe0) == 0xc0 && avail >= 2) {
        *out = (Rune(*p & 0x1f) << 6) | (p[1] & 0x3f);
        return 2;
    }

    if ((*p & 0xf0) == 0xe0 && avail >= 3) {
        *out = (Rune(*p & 0x0f) << 12)  |
               (Rune(p[1] & 0x3f) << 6) | (p[2] & 0x3f);
        return 3;
    }

    if ((*p & 0xf8) == 0xf0 && avail >= 4) {
        *out = (Rune(*p & 0x07) << 18)   |
               (Rune(p[1] & 0x3f) << 12) |
               (Rune(p[2] & 0x3f) << 6)  | (p[3] & 0x3f);
        return 4;
    }

    *out = '?';
    return 1;
}

constexpr Rune kEllipsis = 0x2026;
constexpr int kMaxExtLen = 5;

// Scans a whole UTF-8 string in one pass: counts how many code points `s` contains,
// and finds the last '.' code point in `s` (if any) to abbreviate around. Returns the
// code point count. If a dot is found, *dot_cp_idx and *dot_byte_off are set to its
// position (code point index and byte offset, respectively); otherwise *dot_cp_idx is
// left at -1.
int scan_ext(std::string_view s, int* dot_cp_idx, ptrdiff_t* dot_byte_off)
{
    *dot_cp_idx = -1;
    *dot_byte_off = -1;

    const auto* base = reinterpret_cast<const unsigned char*>(s.data());
    const unsigned char* p = base;
    const unsigned char* pend = p + s.size();
    int n = 0;
    while (p < pend) {
        const unsigned char* p0 = p;
        Rune u;
        p += utf8_next(p, pend, &u);
        if (u == '.') {
            *dot_cp_idx = n;
            *dot_byte_off = p0 - base;
        }
        ++n;
    }
    return n;
}

}  // namespace



Draw::Draw(Canvas& canvas)
: canvas_(canvas), span_(canvas_.width_)
{}

Draw& Draw::put(Rune u, ushort mode)
{
    assert(y_ >= 0 && y_ < canvas_.height_ && x_ >= 0 && x_ < canvas_.width_);
    canvas_.cell(y_, x_++) = Glyph{u, mode, fg_, bg_};
    return *this;
}

Draw& Draw::fill(Rune u, ushort mode)
{
    assert(y_ >= 0 && y_ < canvas_.height_);
    const int xend = std::min(x_ + span_, canvas_.width_);
    while (x_ < xend)
        canvas_.cell(y_, x_++) = Glyph{u, mode, fg_, bg_};

    span_ = canvas_.width_;
    // Note: align_ is not cleared.
    return *this;
}

bool Draw::put_ellipsized(std::string_view s, int xend, ushort mode)
{
    if (xend <= x_) return false;

    int dot_cp_idx;
    ptrdiff_t dot_byte_off;
    const int n_cps = scan_ext(s, &dot_cp_idx, &dot_byte_off);
    const int span = xend - x_;
    if (n_cps <= span) return false;  // fits; let the caller stream it normally

    // Doesn't fit: emit a prefix, an ellipsis, and (if short enough) the extension
    // minus its dot, all in a single left-to-right pass.
    bool keep_ext = false;
    int l_prefix = span - 1;  // budget with ellipsis only
    if (dot_cp_idx > 0) {  // > 0 excludes a leading dot, as in ".bashrc".
        const int l_ext = n_cps - dot_cp_idx - 1;  // chars after the dot
        if (l_ext <= kMaxExtLen) {
            const int budget = span - l_ext - 1;  // ellipsis + extension
            if (budget >= 0) {
                keep_ext = true;
                l_prefix = budget;
            }
        }
    }

    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const auto* pend = p + s.size();
    for (int i = 0; i < l_prefix && p < pend; ++i) {
        Rune u;
        p += utf8_next(p, pend, &u);
        canvas_.cell(y_, x_++) = Glyph{u, mode, fg_, bg_};
    }

    canvas_.cell(y_, x_++) = Glyph{kEllipsis, mode, fg_, bg_};

    if (keep_ext) {
        p = reinterpret_cast<const unsigned char*>(s.data()) + dot_byte_off + 1;  // skip '.'
        while (p < pend && x_ < xend) {
            Rune u;
            p += utf8_next(p, pend, &u);
            canvas_.cell(y_, x_++) = Glyph{u, mode, fg_, bg_};
        }
    }
    return true;
}

Draw& Draw::put(std::string_view s, ushort mode)
{
    assert(y_ >= 0 && y_ < canvas_.height_);

    // Fast-forward x_ to to_x, clearing cells with ' ' along the way if ATTR_CLEAR_FIELD
    // is set in the mode.
    auto skip_or_fill = [&](int to_x) {
        if ((mode & ATTR_CLEAR_FIELD) != 0)
            while (x_ < to_x) {
                Glyph& g = canvas_.cell(y_, x_++);
                g.u = ' ';
                g.mode = mode;
                g.fg = fg_;
                g.bg = bg_;
            }
        else
            x_ = to_x;
    };

    const int xend = std::min(x_ + span_, canvas_.width_);
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
    const unsigned char* pend = p + s.size();

    if (align_ == Align::Left || align_ == Align::Abbr) {
        // Left-aligned text can be streamed glyph-by-glyph; we never need to know its
        // total width up front, since overflow is simply cut off at xend (unless
        // Align::Abbr asks for a nicer abbreviation instead; see put_ellipsized()).
        if (align_ == Align::Left || !put_ellipsized(s, xend, mode)) {
            while (p < pend && x_ < xend) {
                Rune u;
                p += utf8_next(p, pend, &u);
                canvas_.cell(y_, x_++) = Glyph{u, mode, fg_, bg_};
            }
        }

        skip_or_fill(xend);  // right-hand blanks, if the text didn't fill the field.
    }

    else {
        // right- or mid-aligned: decode fully first to learn the text's width, since
        // that determines where within the field it starts.
        std::vector<Glyph> buf;
        buf.reserve(span_);
        while (p < pend) {
            Rune u;
            p += utf8_next(p, pend, &u);
            buf.push_back(Glyph{u, mode, fg_, bg_});
        }

        int slack = xend - x_ - static_cast<int>(buf.size());
        if (align_ == Align::Mid) slack /= 2;
        int skip = 0;  // code-points dropped from the front when the text overflows.
        if (slack > 0)
            skip_or_fill(x_ + slack);  // leading blanks for right- or mid-aligned text
        else
            skip = -slack;

        for (auto it = buf.cbegin() + skip; it != buf.cend() && x_ < xend; ++it)
            canvas_.cell(y_, x_++) = *it;

        skip_or_fill(xend);  // trailing blanks for mid-aligned text
    }

    // Reset the alignment.
    align_ = Align::Left;
    span_ = canvas_.width_;
    return *this;
}



void Canvas::reset(int top, int left, int width, int height, int term_cols)
{
    top_ = top;
    left_ = left;
    width_ = width;
    height_ = height;
    term_cols_ = term_cols;

    // The buffer only ever grows; it never shrinks. This prevents unnecessary
    // reallocations when the panel is toggled between hidden and visible states.
    const size_t need = static_cast<size_t>(height_) * term_cols_;
    if (need > linebuf_.size()) linebuf_.resize(need);
}

void Canvas::present()
{
    for (int y = 0; y < height_; ++y)
        // Draw the corresponding segment of row_ptr(y) into columns
        // [left_, left_ + width_) for each terminal row in the range
        // [top_, top_ + height_).
        xdrawline(row_ptr(y), left_, top_ + y, left_ + width_);
}
