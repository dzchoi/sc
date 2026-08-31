#include <cassert>              // for assert()
#include <cstddef>              // for ptrdiff_t, size_t

#include "canvas.hpp"           // for Canvas, Draw
#include "sc_config.hpp"        // for SC configuration constants

extern "C" {
#include "win.h"                // for xdrawline()
}



// Minimal UTF-8 decoder. Returns bytes consumed; writes a code point (unicode char) to
// *out. Never reads past `pend`, and never reports a length that would run past it
// either.
static int utf8_next(const unsigned char* p, const unsigned char* pend, Rune* out)
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
static int scan_ext(std::string_view s, int* dot_cp_idx, ptrdiff_t* dot_byte_off)
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
static int ext_len(int dot_idx, int n)
{
    if ( dot_idx <= 0 ) return 0;
    const int l = n - dot_idx - 1;
    return l <= kMaxLenExt ? l : 0;
}



Draw::Draw(Canvas& canvas)
: m_canvas(canvas), m_span(m_canvas.m_width)
{}

Draw& Draw::pad(int left, int right)
{
    assert( left >= 0 && right >= 0 );
    m_pad_left = left;
    m_pad_right = right;
    return *this;
}

Draw& Draw::put(Rune u, ushort mode)
{
    assert( m_y >= 0 && m_y < m_canvas.m_height && m_x >= 0 && m_x < m_canvas.m_width );
    m_canvas.cell(m_y, m_x++) = Glyph{u, mode, m_fg, m_bg};
    return *this;
}

Draw& Draw::fill(Rune u, ushort mode)
{
    assert( m_y >= 0 && m_y < m_canvas.m_height );
    const int xend = std::min(m_x + m_span, m_canvas.m_width);
    while ( m_x < xend )
        m_canvas.cell(m_y, m_x++) = Glyph{u, mode, m_fg, m_bg};

    m_span = m_canvas.m_width;
    // Note: m_align is not cleared.
    return *this;
}

bool Draw::put_left_ellipsized(std::string_view s, int xend, ushort mode)
{
    if ( xend <= m_x ) return false;

    int dot_cp_idx;
    ptrdiff_t dot_byte_off;
    const int n_cps = scan_ext(s, &dot_cp_idx, &dot_byte_off);
    const int span = xend - m_x;
    if ( n_cps <= span ) return false;  // fits; let the caller stream it normally

    // Doesn't fit: emit a prefix, an ellipsis, and (if Keep::Both and short enough) the
    // extension minus its dot, all in a single left-to-right pass.
    bool m_keepext = false;
    int l_prefix = span - 1;  // budget with ellipsis only
    if ( m_keep == Keep::Both ) {
        const int l_ext = ext_len(dot_cp_idx, n_cps);
        const int budget = span - l_ext - 1;  // ellipsis + extension
        if ( budget >= 0 ) {
            m_keepext = true;
            l_prefix = budget;
        }
    }

    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const auto* pend = p + s.size();
    for ( int i = 0 ; i < l_prefix && p < pend ; ++i ) {
        Rune u;
        p += utf8_next(p, pend, &u);
        m_canvas.cell(m_y, m_x++) = Glyph{u, mode, m_fg, m_bg};
    }

    m_canvas.cell(m_y, m_x++) = Glyph{kEllipsis, mode, m_fg, m_bg};

    if ( m_keepext ) {
        p = reinterpret_cast<const unsigned char*>(s.data()) + dot_byte_off + 1;  // skip '.'
        while ( p < pend && m_x < xend ) {
            Rune u;
            p += utf8_next(p, pend, &u);
            m_canvas.cell(m_y, m_x++) = Glyph{u, mode, m_fg, m_bg};
        }
    }
    return true;
}

Draw& Draw::put(std::string_view s, ushort mode)
{
    assert( m_y >= 0 && m_y < m_canvas.m_height );

    // Fast-forward m_x to to_x, clearing cells with ' ' along the way if ATTR_CLEAR_FIELD
    // is set in the mode.
    auto skip_or_fill = [&](int to_x) {
        if ( (mode & ATTR_CLEAR_FIELD) != 0 )
            while ( m_x < to_x ) {
                Glyph& g = m_canvas.cell(m_y, m_x++);
                g.u = ' ';
                g.mode = mode;
                g.fg = m_fg;
                g.bg = m_bg;
            }
        else
            m_x = to_x;
    };

    const int xend = std::min(m_x + m_span, m_canvas.m_width);
    const int span = xend - m_x;
    assert( m_pad_left <= span && m_pad_right <= span - m_pad_left );
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
    const unsigned char* pend = p + s.size();

    if ( m_pad_left == 0 && m_pad_right == 0 &&
         m_align == Align::Left && m_keep != Keep::Mid && m_keep != Keep::Right ) {
        // A Left field with at most a single trailing ellipsis (Keep::Default/Left/Both)
        // can be streamed glyph-by-glyph; we never need to know its total width up
        // front.
        if ( m_keep == Keep::Default || !put_left_ellipsized(s, xend, mode) ) {
            while ( p < pend && m_x < xend ) {
                Rune u;
                p += utf8_next(p, pend, &u);
                m_canvas.cell(m_y, m_x++) = Glyph{u, mode, m_fg, m_bg};
            }
        }

        skip_or_fill(xend);  // right-hand blanks, if the text didn't fill the field.
    }

    else {
        // Padding, right/mid alignment, and a Left field with a Mid/Right ellipsize()
        // all require the text's total width, so decode them fully first.
        std::vector<Glyph> buf;
        buf.reserve(s.size());  // upper bound on code point count; never overflows
        while ( p < pend ) {
            Rune u;
            p += utf8_next(p, pend, &u);
            buf.push_back(Glyph{u, mode, m_fg, m_bg});
        }

        const int n = static_cast<int>(buf.size());
        const int inner_span = span - m_pad_left - m_pad_right;

        auto put_padding = [&](int count) {
            while ( count-- > 0 )
                m_canvas.cell(m_y, m_x++) = Glyph{' ', mode, m_fg, m_bg};
        };

        const bool fits = (n <= inner_span);
        if ( fits ) {
            // Align the padded text as one unit. These spaces are decoration owned by
            // the field, so they remain adjacent to the text rather than filling the
            // unused parts of the field.
            int lead = span - n - m_pad_left - m_pad_right;
            if ( m_align == Align::Left ) lead = 0;
            else if ( m_align == Align::Mid ) lead /= 2;
            skip_or_fill(m_x + lead);
        }

        // Padding sets the text boundary before clipping is decided, so overflow
        // cannot overwrite either padded edge.
        put_padding(m_pad_left);
        const int inner_xend = m_x + (fits ? n : inner_span);

        if ( fits ) {
            for ( const Glyph& g : buf ) m_canvas.cell(m_y, m_x++) = g;
        }

        else if ( m_keep == Keep::Default ) {
            // Overflow, no ellipsis: hard-cut per m_align's own implicit direction.
            // (An unpadded Align::Left never reaches here with Keep::Default.)
            const int skip =
                (m_align == Align::Right) ? n - inner_span :
                (m_align == Align::Mid) ? (n - inner_span) / 2 : 0;
            for ( int i = skip ; i < n && m_x < inner_xend ; ++i )
                m_canvas.cell(m_y, m_x++) = buf[i];
        }

        else {
            // Overflow, ellipsized: m_keep decides the cut on its own, regardless of
            // m_align.
            const bool head_cut = (m_keep == Keep::Right || m_keep == Keep::Mid);
            const bool tail_cut =
                (m_keep == Keep::Left || m_keep == Keep::Mid || m_keep == Keep::Both);

            int ext_idx = -1, l_ext = 0;  // Keep::Both extension index/length
            if ( m_keep == Keep::Both ) {
                int dot_idx = -1;
                for ( int i = 0 ; i < n ; ++i )
                    if ( buf[i].u == '.' ) dot_idx = i;
                l_ext = ext_len(dot_idx, n);
                if ( l_ext > 0 )
                    ext_idx = dot_idx + 1;
            }

            int budget = inner_span - head_cut - tail_cut - l_ext;
            if ( budget < 0 ) budget = 0;
            const int start =
                (m_keep == Keep::Right) ? n - budget :
                (m_keep == Keep::Mid) ? (n - budget) / 2 : 0;

            if ( head_cut && m_x < inner_xend )
                m_canvas.cell(m_y, m_x++) = Glyph{kEllipsis, mode, m_fg, m_bg};
            for ( int i = 0 ; i < budget && m_x < inner_xend ; ++i )
                m_canvas.cell(m_y, m_x++) = buf[start + i];
            if ( tail_cut && m_x < inner_xend ) {
                m_canvas.cell(m_y, m_x++) = Glyph{kEllipsis, mode, m_fg, m_bg};
                if ( ext_idx >= 0 )
                    for ( int i = ext_idx ; i < n && m_x < inner_xend ; ++i )
                        m_canvas.cell(m_y, m_x++) = buf[i];
            }
        }

        skip_or_fill(inner_xend);
        put_padding(m_pad_right);
        skip_or_fill(xend);  // trailing blanks, if the field wasn't fully written.
    }

    // Reset the field state.
    m_align = Align::Left;
    m_span = m_canvas.m_width;
    m_keep = Keep::Default;
    m_pad_left = m_pad_right = 0;
    return *this;
}



void Canvas::reset(int top, int left, int width, int height, int term_cols)
{
    m_top = top;
    m_left = left;
    m_width = width;
    m_height = height;
    m_term_cols = term_cols;

    // The buffer only ever grows; it never shrinks. This prevents unnecessary
    // reallocations when the panel is toggled between hidden and visible states.
    const size_t need = static_cast<size_t>(m_height) * m_term_cols;
    if ( need > m_linebuf.size() ) m_linebuf.resize(need);
}

void Canvas::present()
{
    for ( int y = 0 ; y < m_height ; ++y )
        // Draw the corresponding segment of row_ptr(y) into columns
        // [m_left, m_left + m_width) for each terminal row in the range
        // [m_top, m_top + m_height).
        xdrawline(row_ptr(y), m_left, m_top + y, m_left + m_width);
}
