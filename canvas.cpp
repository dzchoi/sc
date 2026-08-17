#include <cassert>              // for assert()
#include <cstddef>              // for ptrdiff_t, size_t

#include "canvas.hpp"           // for Canvas, Draw
#include "sc_config.hpp"        // for SC configuration constants



extern "C" {
#include "win.h"                // for xdrawline()
}

namespace {

// Minimal UTF-8 decoder. Returns bytes consumed; writes a code point (unicode char) to
// *out. Never reads past `pend`, and never reports a length that would run past it
// either.
int utf8_next(const unsigned char* p, const unsigned char* pend, Rune* out)
{
    if ( *p < 0x80 ) {
        *out = *p;
        return 1;
    }

    const ptrdiff_t avail = pend - p;
    if ( (*p & 0xe0) == 0xc0 && avail >= 2 ) {
        *out = (Rune(*p & 0x1f) << 6) | (p[1] & 0x3f);
        return 2;
    }

    if ( (*p & 0xf0) == 0xe0 && avail >= 3 ) {
        *out = (Rune(*p & 0x0f) << 12)  |
               (Rune(p[1] & 0x3f) << 6) | (p[2] & 0x3f);
        return 3;
    }

    if ( (*p & 0xf8) == 0xf0 && avail >= 4 ) {
        *out = (Rune(*p & 0x07) << 18)   |
               (Rune(p[1] & 0x3f) << 12) |
               (Rune(p[2] & 0x3f) << 6)  | (p[3] & 0x3f);
        return 4;
    }

    *out = '?';
    return 1;
}

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
    while ( p < pend ) {
        const unsigned char* p0 = p;
        Rune u;
        p += utf8_next(p, pend, &u);
        if ( u == '.' ) {
            *dot_cp_idx = n;
            *dot_byte_off = p0 - base;
        }
        ++n;
    }
    return n;
}

// Code-point length of a short extension worth preserving after a '.' at code-point
// index `dot_idx` (or -1 if there's no dot), out of `n` total code points; 0 if there's
// no usable extension (no dot, a leading dot as in ".bashrc", or longer than
// kMaxLenExt).
int ext_len(int dot_idx, int n)
{
    if ( dot_idx <= 0 ) return 0;
    const int l = n - dot_idx - 1;
    return l <= kMaxLenExt ? l : 0;
}

}  // namespace



Draw::Draw(Canvas& canvas)
: canvas_(canvas), span_(canvas_.width_)
{}

Draw& Draw::put(Rune u, ushort mode)
{
    assert( y_ >= 0 && y_ < canvas_.height_ && x_ >= 0 && x_ < canvas_.width_ );
    canvas_.cell(y_, x_++) = Glyph{u, mode, fg_, bg_};
    return *this;
}

Draw& Draw::fill(Rune u, ushort mode)
{
    assert( y_ >= 0 && y_ < canvas_.height_ );
    const int xend = std::min(x_ + span_, canvas_.width_);
    while ( x_ < xend )
        canvas_.cell(y_, x_++) = Glyph{u, mode, fg_, bg_};

    span_ = canvas_.width_;
    // Note: align_ is not cleared.
    return *this;
}

bool Draw::put_left_ellipsized(std::string_view s, int xend, ushort mode)
{
    if ( xend <= x_ ) return false;

    int dot_cp_idx;
    ptrdiff_t dot_byte_off;
    const int n_cps = scan_ext(s, &dot_cp_idx, &dot_byte_off);
    const int span = xend - x_;
    if ( n_cps <= span ) return false;  // fits; let the caller stream it normally

    // Doesn't fit: emit a prefix, an ellipsis, and (if Keep::Both and short enough) the
    // extension minus its dot, all in a single left-to-right pass.
    bool keep_ext = false;
    int l_prefix = span - 1;  // budget with ellipsis only
    if ( keep_ == Keep::Both ) {
        const int l_ext = ext_len(dot_cp_idx, n_cps);
        const int budget = span - l_ext - 1;  // ellipsis + extension
        if ( budget >= 0 ) {
            keep_ext = true;
            l_prefix = budget;
        }
    }

    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const auto* pend = p + s.size();
    for ( int i = 0 ; i < l_prefix && p < pend ; ++i ) {
        Rune u;
        p += utf8_next(p, pend, &u);
        canvas_.cell(y_, x_++) = Glyph{u, mode, fg_, bg_};
    }

    canvas_.cell(y_, x_++) = Glyph{kEllipsis, mode, fg_, bg_};

    if ( keep_ext ) {
        p = reinterpret_cast<const unsigned char*>(s.data()) + dot_byte_off + 1;  // skip '.'
        while ( p < pend && x_ < xend ) {
            Rune u;
            p += utf8_next(p, pend, &u);
            canvas_.cell(y_, x_++) = Glyph{u, mode, fg_, bg_};
        }
    }
    return true;
}

Draw& Draw::put(std::string_view s, ushort mode)
{
    assert( y_ >= 0 && y_ < canvas_.height_ );

    // Fast-forward x_ to to_x, clearing cells with ' ' along the way if ATTR_CLEAR_FIELD
    // is set in the mode.
    auto skip_or_fill = [&](int to_x) {
        if ( (mode & ATTR_CLEAR_FIELD) != 0 )
            while ( x_ < to_x ) {
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

    if ( align_ == Align::Left && keep_ != Keep::Mid && keep_ != Keep::Right ) {
        // A Left field with at most a single trailing ellipsis (Keep::Default/Left/Both)
        // can be streamed glyph-by-glyph; we never need to know its total width up
        // front.
        if ( keep_ == Keep::Default || !put_left_ellipsized(s, xend, mode) ) {
            while ( p < pend && x_ < xend ) {
                Rune u;
                p += utf8_next(p, pend, &u);
                canvas_.cell(y_, x_++) = Glyph{u, mode, fg_, bg_};
            }
        }

        skip_or_fill(xend);  // right-hand blanks, if the text didn't fill the field.
    }

    else {
        // Right-/mid-aligned, or a Left field with a Mid/Right ellipsize(): decode
        // fully first, since the starting position depends on the text's total width.
        std::vector<Glyph> buf;
        buf.reserve(s.size());  // upper bound on code point count; never overflows
        while ( p < pend ) {
            Rune u;
            p += utf8_next(p, pend, &u);
            buf.push_back(Glyph{u, mode, fg_, bg_});
        }

        const int n = static_cast<int>(buf.size());
        const int span = xend - x_;
        if ( n <= span ) {
            // Fits: position within the field per align_; ellipsize() only matters on
            // overflow.
            int lead = span - n;
            if ( align_ == Align::Left ) lead = 0;
            else if ( align_ == Align::Mid ) lead /= 2;
            skip_or_fill(x_ + lead);
            for ( const Glyph& g : buf ) canvas_.cell(y_, x_++) = g;
        }

        else if ( keep_ == Keep::Default ) {
            // Overflow, no ellipsis: hard-cut per align_'s own implicit direction.
            // (Align::Left never reaches here with Keep::Default - see above.)
            const int skip = (align_ == Align::Right) ? n - span : (n - span) / 2;
            for ( int i = skip ; i < n && x_ < xend ; ++i )
                canvas_.cell(y_, x_++) = buf[i];
        }

        else {
            // Overflow, ellipsized: keep_ decides the cut on its own, regardless of
            // align_.
            const bool head_cut = (keep_ == Keep::Right || keep_ == Keep::Mid);
            const bool tail_cut =
                (keep_ == Keep::Left || keep_ == Keep::Mid || keep_ == Keep::Both);

            int ext_idx = -1, l_ext = 0;  // Keep::Both: index/length of a kept extension
            if ( keep_ == Keep::Both ) {
                int dot_idx = -1;
                for ( int i = 0 ; i < n ; ++i )
                    if ( buf[i].u == '.' ) dot_idx = i;
                l_ext = ext_len(dot_idx, n);
                if ( l_ext > 0 )
                    ext_idx = dot_idx + 1;
            }

            int budget = span - head_cut - tail_cut - l_ext;
            if ( budget < 0 ) budget = 0;
            const int start =
                (keep_ == Keep::Right) ? n - budget :
                (keep_ == Keep::Mid) ? (n - budget) / 2 : 0;

            if ( head_cut && x_ < xend )
                canvas_.cell(y_, x_++) = Glyph{kEllipsis, mode, fg_, bg_};
            for ( int i = 0 ; i < budget && x_ < xend ; ++i )
                canvas_.cell(y_, x_++) = buf[start + i];
            if ( tail_cut && x_ < xend ) {
                canvas_.cell(y_, x_++) = Glyph{kEllipsis, mode, fg_, bg_};
                if ( ext_idx >= 0 )
                    for ( int i = ext_idx ; i < n && x_ < xend ; ++i )
                        canvas_.cell(y_, x_++) = buf[i];
            }
        }

        skip_or_fill(xend);  // trailing blanks, if the field wasn't fully written.
    }

    // Reset the field state.
    align_ = Align::Left;
    span_ = canvas_.width_;
    keep_ = Keep::Default;
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
    if ( need > linebuf_.size() ) linebuf_.resize(need);
}

void Canvas::present()
{
    for ( int y = 0 ; y < height_ ; ++y )
        // Draw the corresponding segment of row_ptr(y) into columns
        // [left_, left_ + width_) for each terminal row in the range
        // [top_, top_ + height_).
        xdrawline(row_ptr(y), left_, top_ + y, left_ + width_);
}
