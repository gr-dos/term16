/*
 * unit.c - host unit tests for the portable modules: config.c, term.c,
 * cp437.c, zmcrc.c and dlname.c.  `make unit` builds and runs it.
 */

#include <stdio.h>
#include <string.h>
#include "term16.h"

static int failures, checks;

#define CHECK(cond) check((cond), #cond, __LINE__)

static void check(int ok, const char *what, int line)
{
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL line %d: %s\n", line, what);
    }
}

/* ------------------------------------------------------------------ */
/* the screen and the program, as term.c sees them */

static cell_t screen[SCR_ROWS * SCR_COLS];
cell_t *scr_vram = screen;
int scr_mono;
static int cur_row, cur_col, beeps, cursor_on = 1;
static char replies[64];

void scr_copy(unsigned dst, unsigned src, unsigned n)
{
    memmove(screen + dst, screen + src, n * sizeof *screen);
}

void scr_fill(unsigned at, cell_t cell, unsigned n)
{
    while (n--)
        screen[at++] = cell;
}

void scr_cursor(int row, int col) { cur_row = row; cur_col = col; }
void scr_cursor_show(int on) { cursor_on = on; }
void scr_beep(void) { beeps++; }

void term_reply(const char *s)
{
    strcat(replies, s);
}

static void feed(const char *s)
{
    term_write((const unsigned char *)s, (unsigned)strlen(s));
    term_cursor();
}

/* Row r as text, trailing blanks removed */
static const char *row(int r)
{
    static char buf[SCR_COLS + 1];
    int c, n = 0;

    for (c = 0; c < SCR_COLS; c++) {
        buf[c] = (char)(screen[r * SCR_COLS + c] & 0xFF);
        if (buf[c] != ' ')
            n = c + 1;
    }
    buf[n] = '\0';
    return buf;
}

static unsigned attr_at(int r, int c)
{
    return screen[r * SCR_COLS + c] >> 8;
}

static void test_term(void)
{
    int i;
    char buf[64];

    /* the status bar row must never change */
    for (i = 0; i < SCR_COLS; i++)
        screen[STATUS_ROW * SCR_COLS + i] = 0x7000 | 'S';

    term_reset(0);
    feed("hello\r\nworld");
    CHECK(strcmp(row(0), "hello") == 0);
    CHECK(strcmp(row(1), "world") == 0);
    CHECK(cur_row == 1 && cur_col == 5);

    feed("\033[2J\033[3;4HX\033[HY");
    CHECK(strcmp(row(0), "Y") == 0);
    CHECK(strcmp(row(2), "   X") == 0);

    /* clipping to the viewport */
    feed("\033[99;99H");
    CHECK(cur_row == TERM_ROWS - 1 && cur_col == SCR_COLS - 1);
    feed("\033[25;1HA\033[100BB\033[J\033[2J\033[1;30r\033[S\033[200T\033[L");
    feed("\033[24;1H\n\n\n\033M\033[200M\033#8");
    for (i = 0; i < SCR_COLS; i++)
        CHECK(screen[STATUS_ROW * SCR_COLS + i] == (0x7000 | 'S'));

    /* scrolling */
    term_reset(0);
    for (i = 0; i < 30; i++) {
        sprintf(buf, "line %d\r\n", i);
        feed(buf);
    }
    CHECK(strcmp(row(0), "line 7") == 0);
    CHECK(strcmp(row(22), "line 29") == 0);
    CHECK(strcmp(row(23), "") == 0);

    /* scroll region */
    term_reset(0);
    feed("top\033[2;4r\033[2;1Ha\r\nb\r\nc\r\nd");
    CHECK(strcmp(row(0), "top") == 0);
    CHECK(strcmp(row(1), "b") == 0 && strcmp(row(3), "d") == 0);
    feed("\033[r");

    /* deferred wrap */
    term_reset(0);
    for (i = 0; i < SCR_COLS; i++)
        feed("x");
    CHECK(cur_row == 0 && cur_col == SCR_COLS - 1);
    feed("y");
    CHECK(strcmp(row(1), "y") == 0);
    feed("\033[?7l\033[3;1H");
    for (i = 0; i < SCR_COLS + 5; i++)
        feed("z");
    CHECK(strcmp(row(3), "") == 0);

    /* attributes */
    term_reset(0);
    feed("\033[1;31;44mA\033[0mB\033[7mC\033[m\033[94mD");
    CHECK(attr_at(0, 0) == 0x1C);       /* bright red on blue */
    CHECK(attr_at(0, 1) == 0x07);
    CHECK(attr_at(0, 2) == 0x70);
    CHECK(attr_at(0, 3) == 0x09);
    scr_mono = 1;
    term_reset(0);
    feed("\033[4mU\033[0;1mB");
    CHECK(attr_at(0, 0) == 0x01 && attr_at(0, 1) == 0x0F);
    scr_mono = 0;

    /* replies */
    term_reset(0);
    replies[0] = '\0';
    feed("\033[5;7H\033[6n\033[c\033[5n");
    CHECK(strcmp(replies, "\033[5;7R\033[?1;2c\033[0n") == 0);

    /* erase, insert, delete */
    term_reset(0);
    feed("abcdefgh\033[1;3H\033[K");
    CHECK(strcmp(row(0), "ab") == 0);
    feed("\033[2;1Habcdefgh\033[2;3H\033[2P");
    CHECK(strcmp(row(1), "abefgh") == 0);
    feed("\033[2;3H\033[3@");
    CHECK(strcmp(row(1), "ab   efgh") == 0);
    feed("\033[2;1H\033[2X");
    CHECK(strcmp(row(1), "     efgh") == 0);
    feed("\033[1;2H\033[1K");
    CHECK(strcmp(row(0), "") == 0);

    /* tabs, backspace, save and restore */
    term_reset(0);
    feed("a\tb\010c\0337\033[10;10H\0338d");
    CHECK(strcmp(row(0), "a       cd") == 0);

    /* unknown and string sequences are swallowed */
    term_reset(0);
    feed("\033]0;title\007\033P1$r\033\\\033[?1049h\033[>c\033[38;2;1;2;3mok\033Zq");
    CHECK(strcmp(row(0), "okq") == 0);

    /* DEC graphics */
    term_reset(0);
    feed("\033(0lqk\033(Bq\016x\017x");
    CHECK((screen[0] & 0xFF) == 0xDA && (screen[1] & 0xFF) == 0xC4 &&
          (screen[2] & 0xFF) == 0xBF && (screen[3] & 0xFF) == 'q');
    CHECK((screen[4] & 0xFF) == 'x');   /* G1 is ASCII too */

    /* code page 437 and UTF-8 */
    term_reset(0);
    feed("\xC9\xCD\xBB");
    CHECK((screen[0] & 0xFF) == 0xC9 && (screen[2] & 0xFF) == 0xBB);
    term_reset(1);
    feed("\xE2\x94\x8C\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80z\xC3");
    feed("y");
    CHECK((screen[0] & 0xFF) == 0xDA);  /* U+250C */
    CHECK((screen[1] & 0xFF) == 0x82);  /* e acute */
    CHECK((screen[2] & 0xFF) == 'E');   /* euro, approximated */
    CHECK((screen[3] & 0xFF) == '?');   /* emoji */
    CHECK((screen[4] & 0xFF) == 'z');
    CHECK((screen[5] & 0xFF) == '?' && (screen[6] & 0xFF) == 'y');

    /* bell, cursor keys mode, cursor visibility */
    beeps = 0;
    feed("\007\033[?1h\033[?25l");
    CHECK(beeps == 1 && term_appcursor == 1 && cursor_on == 0);
    feed("\033c");
    CHECK(term_appcursor == 0 && cursor_on == 1);
    for (i = 0; i < SCR_COLS; i++)
        CHECK(screen[STATUS_ROW * SCR_COLS + i] == (0x7000 | 'S'));
}

static void test_cp437(void)
{
    CHECK(cp437_from_ucs('A') == 'A');
    CHECK(cp437_from_ucs(0x00C7) == 0x80);
    CHECK(cp437_from_ucs(0x2588) == 0xDB);
    CHECK(cp437_from_ucs(0x263A) == 0x01);
    CHECK(cp437_from_ucs(0x00A0) == 0xFF);
    CHECK(cp437_from_ucs(0x2019) == '\'');
    CHECK(cp437_from_ucs(0x256D) == 0xDA);
    CHECK(cp437_from_ucs(0x4E2D) == -1);
    CHECK(cp437_from_ucs(0x1F600UL) == -1);
}

static void test_config(void)
{
    struct config c;
    char line[SCR_COLS + 1], buf[16];

    cfg_default(&c);
    CHECK(c.port == 1 && c.baud == 9600 && c.data == 8 && c.stop == 1);
    CHECK(cfg_parse(&c, F_PORT, "com2") == 0 && c.port == 2);
    CHECK(cfg_parse(&c, F_PORT, "4") == 0 && c.port == 4);
    CHECK(cfg_parse(&c, F_PORT, "COM5") < 0);
    CHECK(cfg_parse(&c, F_BAUD, "115200") == 0 && c.baud == 115200UL);
    CHECK(cfg_parse(&c, F_BAUD, "1234") < 0);
    CHECK(cfg_parse(&c, F_BAUD, "") < 0);
    CHECK(cfg_parse(&c, F_DATA, "7") == 0 && c.data == 7);
    CHECK(cfg_parse(&c, F_DATA, "9") < 0);
    CHECK(cfg_parse(&c, F_PARITY, "Even") == 0 && c.parity == PAR_EVEN);
    CHECK(cfg_parse(&c, F_PARITY, "o") == 0 && c.parity == PAR_ODD);
    CHECK(cfg_parse(&c, F_PARITY, "x") < 0);
    CHECK(cfg_parse(&c, F_STOP, "2") == 0 && c.stop == 2);
    CHECK(cfg_parse(&c, F_STOP, "3") < 0);
    CHECK(cfg_parse(&c, F_FLOW, "RTS/CTS") == 0 && c.flow == FLOW_RTSCTS);
    CHECK(cfg_parse(&c, F_FLOW, "xonxoff") == 0 && c.flow == FLOW_XONXOFF);
    CHECK(cfg_parse(&c, F_FLOW, "maybe") < 0);
    CHECK(cfg_parse(&c, F_TERM, "UTF-8") == 0 && c.term == TERM_UTF8);
    CHECK(cfg_parse(&c, F_TERM, "vt52") < 0);

    cfg_status(&c, ST_OPEN, line);
    CHECK(strlen(line) == SCR_COLS);
    CHECK(strncmp(line, " COM4  115200  7O2  Flow:XON/XOFF  UTF8  OPEN ", 46) == 0);
    CHECK(strcmp(line + SCR_COLS - 23, "Alt-M Menu  Alt-X Exit ") == 0);
    strcpy(c.dldir, "C:\\SECRET");
    cfg_status(&c, ST_ERROR, line);
    CHECK(strstr(line, "SECRET") == NULL && strstr(line, "ERROR") != NULL);

    cfg_default(&c);
    cfg_step(&c, F_BAUD, 1);
    CHECK(c.baud == 19200);
    cfg_step(&c, F_PORT, -1);
    CHECK(c.port == 4);
    cfg_step(&c, F_PARITY, -1);
    CHECK(c.parity == PAR_SPACE);
    CHECK(strcmp(cfg_name(&c, F_PORT, buf), "COM4") == 0);
    CHECK(strcmp(cfg_name(&c, F_BAUD, buf), "19200") == 0);
}

static void test_crc(void)
{
    const char *s = "123456789";
    unsigned c16 = 0;
    unsigned long c32 = 0xFFFFFFFFUL;
    char h[24];
    int n;

    zm_crc_init();
    for (; *s; s++) {
        c16 = zm_crc16(c16, (unsigned char)*s);
        c32 = zm_crc32(c32, (unsigned char)*s);
    }
    CHECK(c16 == 0x31C3);               /* CRC-16/XMODEM check value */
    CHECK((c32 ^ 0xFFFFFFFFUL) == 0xCBF43926UL);  /* CRC-32 check value */

    n = zm_hexheader(h, ZRINIT, (unsigned long)(CANFDX | CANOVIO | CANFC32) << 24);
    CHECK(n == 21);
    CHECK(memcmp(h, "**\030B0100000023be50\r\212\021", 21) == 0);
    n = zm_hexheader(h, ZFIN, 0);
    CHECK(n == 20 && memcmp(h, "**\030B0800000000022d\r\212", 20) == 0);
}

static int fake_exists(const char *path)
{
    static const char *const there[] = {
        "D:\\DL\\REPORT.TXT", "D:\\DL\\REPORT~1.TXT", "D:\\DL\\LONGNAME",
        NULL
    };
    int i;

    for (i = 0; there[i]; i++)
        if (strcmp(path, there[i]) == 0)
            return 1;
    return 0;
}

static void test_names(void)
{
    char base[10], ext[4], out[13], path[100];

    dl_safename("../../etc/passwd", base, ext);
    CHECK(strcmp(base, "PASSWD") == 0 && ext[0] == '\0');
    dl_safename("C:\\DOS\\COMMAND.COM", base, ext);
    CHECK(strcmp(base, "COMMAND") == 0 && strcmp(ext, "COM") == 0);
    dl_safename("archive.tar.gz", base, ext);
    CHECK(strcmp(base, "ARCHIVE_") == 0 && strcmp(ext, "GZ") == 0);
    dl_safename("..", base, ext);
    CHECK(strcmp(base, "FILE") == 0);
    dl_safename("/", base, ext);
    CHECK(strcmp(base, "FILE") == 0);
    dl_safename(".bashrc", base, ext);
    CHECK(strcmp(base, "BASHRC") == 0);
    dl_safename("nul.txt", base, ext);
    CHECK(strcmp(base, "_NUL") == 0 && strcmp(ext, "TXT") == 0);
    dl_safename("clock$", base, ext);
    CHECK(strcmp(base, "_CLOCK$") == 0);
    dl_safename("a+b=c[1].html", base, ext);
    CHECK(strcmp(base, "A_B_C_1_") == 0 && strcmp(ext, "HTM") == 0);

    CHECK(dl_unique("D:\\DL", "REPORT", "TXT", fake_exists, out) == 0);
    CHECK(strcmp(out, "REPORT~2.TXT") == 0);
    CHECK(dl_unique("D:\\DL", "LONGNAME", "", fake_exists, out) == 0);
    CHECK(strcmp(out, "LONGNA~1") == 0);
    CHECK(dl_unique("D:\\DL", "NEW", "TXT", fake_exists, out) == 0);
    CHECK(strcmp(out, "NEW.TXT") == 0);
    dl_join(path, "C:\\", "X.TXT");
    CHECK(strcmp(path, "C:\\X.TXT") == 0);
    dl_join(path, "C:\\DL", "X.TXT");
    CHECK(strcmp(path, "C:\\DL\\X.TXT") == 0);
}

/* uprobe.c, against simulated I/O ports.  The LCR at port 0x103 reads
 * back (written & keep) | stuck; every LCR write is recorded. */
static unsigned char lcr_now, lcr_keep, lcr_stuck;
static int lcr_writes;

static unsigned char fake_in(unsigned port)
{
    return port == 0x103 ? (unsigned char)((lcr_now & lcr_keep) | lcr_stuck)
                         : 0xFF;
}

static void fake_out(unsigned port, unsigned char v)
{
    if (port == 0x103) {
        lcr_now = v;
        lcr_writes++;
    }
}

static int probe(unsigned char start, unsigned char keep,
                 unsigned char stuck, unsigned char *old)
{
    lcr_now = start;
    lcr_keep = keep;
    lcr_stuck = stuck;
    lcr_writes = 0;
    return uart_probe(0x100, fake_in, fake_out, old);
}

static void test_probe(void)
{
    unsigned char old;

    /* A UART: the old LCR is kept for uart_close, 8N1 is left set */
    CHECK(probe(0x07, 0xFF, 0x00, &old) == 0);
    CHECK(old == 0x07 && lcr_now == 0x03);

    /* A register that ignores writes: fails at the first readback,
     * and what was there is written back */
    CHECK(probe(0x07, 0x00, 0x45, &old) == -1);
    CHECK(old == 0x45 && lcr_now == 0x45 && lcr_writes == 2);

    /* Bits stuck at 1: 1Bh reads back but 03h does not */
    CHECK(probe(0x1B, 0xFF, 0x18, &old) == -1);
    CHECK(old == 0x1B && lcr_now == 0x1B && lcr_writes == 3);

    /* Bits stuck at 0: fails at the first readback, LCR restored */
    CHECK(probe(0x03, 0x07, 0x00, &old) == -1);
    CHECK(lcr_now == 0x03 && lcr_writes == 2);
}

int main(void)
{
    test_term();
    test_cp437();
    test_config();
    test_crc();
    test_names();
    test_probe();
    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
