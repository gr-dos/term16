/*
 * term.c - ANSI/VT100 terminal emulation on the 80x24 remote viewport
 *
 * Writes cells straight into scr_vram, rows 0 to TERM_ROWS - 1 only:
 * every cursor movement, erase, scroll and insert is clipped to the
 * viewport, so nothing the remote sends can reach the status bar on
 * row 24.  Unknown sequences are parsed to their end and ignored.
 *
 * Portable C: tests/unit/test_term.c runs it on the build host.
 */

#include "term16.h"

#define NPAR 16

enum { S_TEXT, S_ESC, S_INTER, S_CSI, S_STRING, S_STRESC, S_CHARSET,
       S_HASH };

static int row, col;
static unsigned char wrapnext;
static int top, bot;                    /* scroll region, inclusive */
static unsigned char autowrap, origin;
static unsigned char fg, bg, bold, ul, blink, rev;
static cell_t attr, erase;
static unsigned char gset[2], gl;       /* 1: DEC special graphics */
static unsigned char tabs[SCR_COLS];
static unsigned char lastch;

static struct {
    int row, col;
    unsigned char fg, bg, bold, ul, blink, rev, origin, g0, g1, gl;
} sv;

static unsigned char utf8;
static unsigned long ucs;
static unsigned char ulen;              /* continuation bytes still due */

static unsigned char state;
static unsigned params[NPAR];
static unsigned char nparam, priv, inter, charset_g;

int term_appcursor;

/* ANSI colour numbers to the CGA's */
static const unsigned char cga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

/* DEC special graphics, 5Fh to 7Eh, in code page 437 */
static const unsigned char decgfx[32] = {
    0x20, 0x04, 0xB1, 0xF9, 0xF9, 0xF9, 0xF9, 0xF8, 0xF1, 0xF9, 0xF9,
    0xD9, 0xBF, 0xDA, 0xC0, 0xC5, 0xC4, 0xC4, 0xC4, 0xC4, 0xC4, 0xC3,
    0xB4, 0xC1, 0xC2, 0xB3, 0xF3, 0xF2, 0xE3, 0xD8, 0x9C, 0xFA
};

static void set_attr(void)
{
    unsigned a;

    if (scr_mono) {
        a = rev ? 0x70 : ul ? 0x01 : 0x07;
        erase = 0x0720;
    } else {
        unsigned f = fg, b = bg;

        if (rev) {
            unsigned t = f & 7;

            f = b;
            b = t;
        }
        a = (b << 4) | f;
        erase = (cell_t)(((bg << 4) | 7) << 8 | ' ');
    }
    if (bold)
        a |= 0x08;
    if (blink)
        a |= 0x80;
    attr = (cell_t)(a << 8);
}

static void clear_rows(int from, int to)
{
    scr_fill((unsigned)from * SCR_COLS, erase,
             (unsigned)(to - from + 1) * SCR_COLS);
}

static void scroll_up(int t, int b, unsigned n)
{
    unsigned rows = (unsigned)(b - t + 1);

    if (n > rows)
        n = rows;
    scr_copy((unsigned)t * SCR_COLS, (unsigned)(t + n) * SCR_COLS,
             (rows - n) * SCR_COLS);
    clear_rows(b - (int)n + 1, b);
}

static void scroll_down(int t, int b, unsigned n)
{
    unsigned rows = (unsigned)(b - t + 1);

    if (n > rows)
        n = rows;
    scr_copy((unsigned)(t + n) * SCR_COLS, (unsigned)t * SCR_COLS,
             (rows - n) * SCR_COLS);
    clear_rows(t, t + (int)n - 1);
}

static void linefeed(void)
{
    wrapnext = 0;
    if (row == bot)
        scroll_up(top, bot, 1);
    else if (row < TERM_ROWS - 1)
        row++;
}

static void reverse_index(void)
{
    wrapnext = 0;
    if (row == top)
        scroll_down(top, bot, 1);
    else if (row > 0)
        row--;
}

static void put(unsigned char ch)
{
    if (wrapnext) {
        col = 0;
        linefeed();
    }
    scr_vram[row * SCR_COLS + col] = attr | ch;
    lastch = ch;
    if (col < SCR_COLS - 1)
        col++;
    else if (autowrap)
        wrapnext = 1;
}

static void goto_rc(int r, int c)
{
    int lo = 0, hi = TERM_ROWS - 1;

    if (origin) {
        r += top;
        lo = top;
        hi = bot;
    }
    row = r < lo ? lo : r > hi ? hi : r;
    col = c < 0 ? 0 : c > SCR_COLS - 1 ? SCR_COLS - 1 : c;
    wrapnext = 0;
}

static void save_cursor(void)
{
    sv.row = row;
    sv.col = col;
    sv.fg = fg;
    sv.bg = bg;
    sv.bold = bold;
    sv.ul = ul;
    sv.blink = blink;
    sv.rev = rev;
    sv.origin = origin;
    sv.g0 = gset[0];
    sv.g1 = gset[1];
    sv.gl = gl;
}

static void restore_cursor(void)
{
    row = sv.row;
    col = sv.col;
    fg = sv.fg;
    bg = sv.bg;
    bold = sv.bold;
    ul = sv.ul;
    blink = sv.blink;
    rev = sv.rev;
    origin = sv.origin;
    gset[0] = sv.g0;
    gset[1] = sv.g1;
    gl = sv.gl;
    wrapnext = 0;
    set_attr();
}

static void reset_modes(void)
{
    int i;

    top = 0;
    bot = TERM_ROWS - 1;
    autowrap = 1;
    origin = 0;
    fg = 7;
    bg = 0;
    bold = ul = blink = rev = 0;
    gset[0] = gset[1] = 0;
    gl = 0;
    term_appcursor = 0;
    for (i = 0; i < SCR_COLS; i++)
        tabs[i] = (unsigned char)(i % 8 == 0 && i != 0);
    set_attr();
    row = col = 0;
    wrapnext = 0;
    save_cursor();
}

void term_reset(int use_utf8)
{
    utf8 = (unsigned char)use_utf8;
    ulen = 0;
    state = S_TEXT;
    reset_modes();
    scr_cursor_show(1);
    clear_rows(0, TERM_ROWS - 1);
}

/* Switch between code page 437 and UTF-8, keeping the screen */
void term_mode(int use_utf8)
{
    utf8 = (unsigned char)use_utf8;
    ulen = 0;
}

void term_clear(void)
{
    clear_rows(0, TERM_ROWS - 1);
    goto_rc(0, 0);
}

void term_cursor(void)
{
    scr_cursor(row, col);
}

static void reply_num(char *p, unsigned n)
{
    char tmp[6];
    int i = 0;

    do {
        tmp[i++] = (char)('0' + n % 10);
        n /= 10;
    } while (n);
    while (i)
        *p++ = tmp[--i];
    *p = '\0';
}

static char *end_of(char *p)
{
    while (*p)
        p++;
    return p;
}

static unsigned par(int i, unsigned def)
{
    return i < nparam && params[i] != 0 ? params[i] : def;
}

static void sgr_color(unsigned n, unsigned char *c, int is_fg)
{
    if (n < 8)
        *c = cga[n];
    else if (n < 16 && is_fg)
        *c = (unsigned char)(cga[n - 8] | 8);
    else if (n < 16)
        *c = cga[n - 8];
}

static void sgr(void)
{
    int i;

    if (nparam == 0)
        nparam = 1, params[0] = 0;
    for (i = 0; i < nparam; i++) {
        unsigned p = params[i];

        if (p == 0) {
            fg = 7;
            bg = 0;
            bold = ul = blink = rev = 0;
        } else if (p == 1)
            bold = 1;
        else if (p == 4)
            ul = 1;
        else if (p == 5 || p == 6)
            blink = 1;
        else if (p == 7)
            rev = 1;
        else if (p == 21 || p == 22)
            bold = 0;
        else if (p == 24)
            ul = 0;
        else if (p == 25)
            blink = 0;
        else if (p == 27)
            rev = 0;
        else if (p >= 30 && p <= 37)
            fg = cga[p - 30];
        else if (p == 39)
            fg = 7;
        else if (p >= 40 && p <= 47)
            bg = cga[p - 40];
        else if (p == 49)
            bg = 0;
        else if (p >= 90 && p <= 97)
            fg = (unsigned char)(cga[p - 90] | 8);
        else if (p >= 100 && p <= 107)
            bg = cga[p - 100];
        else if (p == 38 || p == 48) {
            /* 38;5;n and 38;2;r;g;b: the first 16 of 256 colours only */
            if (i + 2 < nparam && params[i + 1] == 5) {
                sgr_color(params[i + 2], p == 38 ? &fg : &bg, p == 38);
                i += 2;
            } else if (i + 1 < nparam && params[i + 1] == 2)
                i += 4;
            else
                i = nparam;
        }
    }
    set_attr();
}

static void erase_line_part(int from, int to)
{
    scr_fill((unsigned)(row * SCR_COLS + from), erase,
             (unsigned)(to - from + 1));
}

static void set_mode(int on)
{
    int i;

    for (i = 0; i < nparam; i++) {
        unsigned p = params[i];

        if (priv != '?')
            continue;                   /* ANSI modes: none supported */
        switch (p) {
        case 1:
            term_appcursor = on;
            break;
        case 6:
            origin = (unsigned char)on;
            goto_rc(0, 0);
            break;
        case 7:
            autowrap = (unsigned char)on;
            if (!on)
                wrapnext = 0;
            break;
        case 25:
            scr_cursor_show(on);
            break;
        case 47:
        case 1047:
        case 1049:
            /* No second screen: start and leave it with a clear one */
            if (on && p == 1049)
                save_cursor();
            clear_rows(0, TERM_ROWS - 1);
            if (!on && p == 1049)
                restore_cursor();
            break;
        }
    }
}

static void csi(unsigned char f)
{
    char buf[16];
    unsigned n = par(0, 1);
    int i;

    if (inter != 0) {
        if (inter == '!' && f == 'p')   /* DECSTR soft reset */
            reset_modes();
        return;
    }
    if (priv != 0 && priv != '?' && f != 'c')
        return;
    switch (f) {
    case 'A':
        i = row >= top ? top : 0;
        row = row - (int)n < i ? i : row - (int)n;
        wrapnext = 0;
        break;
    case 'B':
    case 'e':
        i = row <= bot ? bot : TERM_ROWS - 1;
        row = row + (int)n > i ? i : row + (int)n;
        wrapnext = 0;
        break;
    case 'C':
    case 'a':
        col = col + (int)n > SCR_COLS - 1 ? SCR_COLS - 1 : col + (int)n;
        wrapnext = 0;
        break;
    case 'D':
        col = col - (int)n < 0 ? 0 : col - (int)n;
        wrapnext = 0;
        break;
    case 'E':
    case 'F':
        i = f == 'E' ? (row <= bot ? bot : TERM_ROWS - 1)
                     : (row >= top ? top : 0);
        if (f == 'E')
            row = row + (int)n > i ? i : row + (int)n;
        else
            row = row - (int)n < i ? i : row - (int)n;
        col = 0;
        wrapnext = 0;
        break;
    case 'G':
    case '`':
        col = (int)n > SCR_COLS ? SCR_COLS - 1 : (int)n - 1;
        wrapnext = 0;
        break;
    case 'H':
    case 'f':
        goto_rc((int)par(0, 1) - 1, (int)par(1, 1) - 1);
        break;
    case 'd':
        goto_rc((int)n - 1, col);
        break;
    case 'J':
        switch (par(0, 0)) {
        case 0:
            erase_line_part(col, SCR_COLS - 1);
            if (row < TERM_ROWS - 1)
                clear_rows(row + 1, TERM_ROWS - 1);
            break;
        case 1:
            if (row > 0)
                clear_rows(0, row - 1);
            erase_line_part(0, col);
            break;
        default:
            clear_rows(0, TERM_ROWS - 1);
            break;
        }
        wrapnext = 0;
        break;
    case 'K':
        switch (par(0, 0)) {
        case 0:
            erase_line_part(col, SCR_COLS - 1);
            break;
        case 1:
            erase_line_part(0, col);
            break;
        default:
            erase_line_part(0, SCR_COLS - 1);
            break;
        }
        wrapnext = 0;
        break;
    case 'L':
    case 'M':
        if (row >= top && row <= bot) {
            if (f == 'L')
                scroll_down(row, bot, n);
            else
                scroll_up(row, bot, n);
            col = 0;
            wrapnext = 0;
        }
        break;
    case 'P':
    case '@':
    case 'X':
        if (n > (unsigned)(SCR_COLS - col))
            n = (unsigned)(SCR_COLS - col);
        i = row * SCR_COLS + col;
        if (f == 'P') {
            scr_copy((unsigned)i, (unsigned)i + n, SCR_COLS - col - n);
            scr_fill((unsigned)(row * SCR_COLS + SCR_COLS) - n, erase, n);
        } else if (f == '@') {
            scr_copy((unsigned)i + n, (unsigned)i, SCR_COLS - col - n);
            scr_fill((unsigned)i, erase, n);
        } else
            scr_fill((unsigned)i, erase, n);
        wrapnext = 0;
        break;
    case 'S':
        scroll_up(top, bot, n);
        break;
    case 'T':
        if (nparam <= 1)
            scroll_down(top, bot, n);
        break;
    case 'b':
        if (n > SCR_COLS * TERM_ROWS)
            n = SCR_COLS * TERM_ROWS;
        while (n--)
            put(lastch);
        break;
    case 'I':
    case 'Z':
        while (n--) {
            if (f == 'I') {
                while (col < SCR_COLS - 1 && !tabs[++col])
                    ;
            } else {
                while (col > 0 && !tabs[--col])
                    ;
            }
        }
        wrapnext = 0;
        break;
    case 'g':
        if (par(0, 0) == 0)
            tabs[col] = 0;
        else if (par(0, 0) == 3)
            for (i = 0; i < SCR_COLS; i++)
                tabs[i] = 0;
        break;
    case 'm':
        sgr();
        break;
    case 'r':
        if (priv)
            break;
        {
            int t = (int)par(0, 1), b = (int)par(1, TERM_ROWS);

            if (b > TERM_ROWS)
                b = TERM_ROWS;
            if (t < b) {
                top = t - 1;
                bot = b - 1;
                goto_rc(0, 0);
            }
        }
        break;
    case 's':
        if (!priv)
            save_cursor();
        break;
    case 'u':
        if (!priv)
            restore_cursor();
        break;
    case 'h':
        set_mode(1);
        break;
    case 'l':
        set_mode(0);
        break;
    case 'n':
        if (priv)
            break;
        if (par(0, 0) == 5)
            term_reply("\033[0n");
        else if (par(0, 0) == 6) {
            buf[0] = '\033';
            buf[1] = '[';
            reply_num(buf + 2, (unsigned)(row - (origin ? top : 0) + 1));
            i = (int)(end_of(buf) - buf);
            buf[i++] = ';';
            reply_num(buf + i, (unsigned)(col + 1));
            i = (int)(end_of(buf) - buf);
            buf[i++] = 'R';
            buf[i] = '\0';
            term_reply(buf);
        }
        break;
    case 'c':
        if (priv == 0 && par(0, 0) == 0)
            term_reply("\033[?1;2c");   /* VT100 with advanced video */
        break;
    }
}

static void esc(unsigned char c)
{
    state = S_TEXT;
    switch (c) {
    case '[':
        state = S_CSI;
        nparam = 0;
        priv = 0;
        inter = 0;
        params[0] = 0;
        break;
    case ']':
    case 'P':
    case 'X':
    case '^':
    case '_':
        state = S_STRING;
        break;
    case '(':
    case ')':
        charset_g = (unsigned char)(c == ')');
        state = S_CHARSET;
        break;
    case '*':
    case '+':
        charset_g = 2;
        state = S_CHARSET;
        break;
    case '#':
        state = S_HASH;
        break;
    case '7':
        save_cursor();
        break;
    case '8':
        restore_cursor();
        break;
    case 'D':
        linefeed();
        break;
    case 'E':
        col = 0;
        linefeed();
        break;
    case 'M':
        reverse_index();
        break;
    case 'H':
        tabs[col] = 1;
        break;
    case 'c':
        reset_modes();
        scr_cursor_show(1);
        clear_rows(0, TERM_ROWS - 1);
        break;
    case 'Z':
        term_reply("\033[?1;2c");
        break;
    default:
        if (c >= 0x20 && c <= 0x2F)
            state = S_INTER;            /* nF: skip to the final byte */
        break;
    }
}

static void control(unsigned char c)
{
    switch (c) {
    case 0x07:
        scr_beep();
        break;
    case 0x08:
        if (wrapnext)
            wrapnext = 0;
        else if (col > 0)
            col--;
        break;
    case 0x09:
        while (col < SCR_COLS - 1 && !tabs[++col])
            ;
        break;
    case 0x0A:
    case 0x0B:
    case 0x0C:
        linefeed();
        break;
    case 0x0D:
        col = 0;
        wrapnext = 0;
        break;
    case 0x0E:
        gl = 1;
        break;
    case 0x0F:
        gl = 0;
        break;
    case 0x18:
    case 0x1A:
        state = S_TEXT;
        break;
    case 0x1B:
        state = S_ESC;
        break;
    }
}

static void graphic(unsigned char c)
{
    if (gset[gl] && c >= 0x5F && c <= 0x7E)
        c = decgfx[c - 0x5F];
    put(c);
}

static void byte(unsigned char c)
{
    /* Strings (OSC, DCS, ...) end at BEL or ESC \ and are ignored */
    if (state == S_STRING) {
        if (c == 0x1B)
            state = S_STRESC;
        else if (c == 0x07 || c == 0x18 || c == 0x1A)
            state = S_TEXT;
        return;
    }
    if (state == S_STRESC) {
        state = c == '\\' ? S_TEXT : S_STRING;
        return;
    }

    if (c < 0x20) {
        if (ulen) {
            ulen = 0;
            put('?');
        }
        control(c);
        return;
    }
    if (c == 0x7F)
        return;

    switch (state) {
    case S_ESC:
        esc(c);
        return;
    case S_INTER:
        if (c >= 0x30)
            state = S_TEXT;
        return;
    case S_CHARSET:
        if (charset_g < 2)
            gset[charset_g] = (unsigned char)(c == '0');
        state = S_TEXT;
        return;
    case S_HASH:
        if (c == '8') {                 /* DECALN: fill with E */
            scr_fill(0, (cell_t)(0x0700 | 'E'), SCR_COLS * TERM_ROWS);
        }
        state = S_TEXT;
        return;
    case S_CSI:
        if (c >= '0' && c <= '9') {
            if (nparam == 0)
                nparam = 1;
            if (nparam <= NPAR && params[nparam - 1] < 10000)
                params[nparam - 1] = params[nparam - 1] * 10 + (c - '0');
        } else if (c == ';' || c == ':') {
            if (nparam == 0)
                nparam = 1;
            if (nparam < NPAR)
                params[nparam] = 0;
            nparam++;
        } else if (c >= 0x3C && c <= 0x3F) {
            if (nparam == 0 && priv == 0)
                priv = c;
        } else if (c >= 0x20 && c <= 0x2F) {
            inter = c;
        } else if (c >= 0x40 && c <= 0x7E) {
            if (nparam > NPAR)
                nparam = NPAR;
            state = S_TEXT;
            csi(c);
        } else
            state = S_TEXT;
        return;
    }

    /* text */
    if (c < 0x80) {
        if (ulen) {
            ulen = 0;
            put('?');
        }
        graphic(c);
        return;
    }
    if (!utf8) {
        put(c);                         /* code page 437 */
        return;
    }
    if (c < 0xC0) {
        if (ulen == 0) {
            put('?');
            return;
        }
        ucs = (ucs << 6) | (c & 0x3F);
        if (--ulen == 0) {
            int g = cp437_from_ucs(ucs);

            if (ucs >= 0x80 && ucs < 0xA0)
                return;                 /* C1 controls */
            put(g < 0 ? '?' : (unsigned char)g);
        }
        return;
    }
    if (ulen)
        put('?');
    if (c < 0xE0) {
        ucs = c & 0x1F;
        ulen = 1;
    } else if (c < 0xF0) {
        ucs = c & 0x0F;
        ulen = 2;
    } else if (c < 0xF8) {
        ucs = c & 0x07;
        ulen = 3;
    } else {
        ulen = 0;
        put('?');
    }
}

void term_write(const unsigned char *s, unsigned n)
{
    while (n-- > 0)
        byte(*s++);
}
