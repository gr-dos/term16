/*
 * screen.c - text video memory, cursor, speaker, keyboard and clock
 *
 * The screen is written directly in video memory (B800h, or B000h in
 * monochrome mode 7).  The BIOS sets the mode, the cursor and its
 * shape.  Keys come from INT 16h, the clock from the BIOS tick count.
 *
 * The test build (-DTERM16_TEST) reads keys from the file named by
 * %T16KEYS% instead of the keyboard, and appends screen dumps to the
 * file named by %T16SCR%; see tests/run.py.
 */

#include <dos.h>
#include <stdlib.h>
#include <string.h>
#include "term16.h"

cell_t FAR *scr_vram;
int scr_mono;

static unsigned char page;
static unsigned char saved_row, saved_col;
static unsigned short saved_shape;
static cell_t *saved;                   /* the DOS screen, put back at exit */
static int restore_ok;
static unsigned beep_start;
static unsigned char beeping;

#define BIOS_BYTE(off) (*(unsigned char __far *)MK_FP(0x40, (off)))
#define BIOS_WORD(off) (*(volatile unsigned short __far *)MK_FP(0x40, (off)))
#define BIOS_TICKS     (*(volatile unsigned short __far *)MK_FP(0x40, 0x6C))

unsigned char inb(unsigned port);
#pragma aux inb = "in al,dx" parm [dx] value [al] modify exact [al];
void outb(unsigned port, unsigned char v);
#pragma aux outb = "out dx,al" parm [dx] [al] modify exact [];

void movsw_up(void __far *dst, const void __far *src, unsigned n);
#pragma aux movsw_up = \
    "push ds" "mov ds,dx" "rep movsw" "pop ds" \
    parm [es di] [dx si] [cx] modify exact [si di cx];
void movsw_down(void __far *dst, const void __far *src, unsigned n);
#pragma aux movsw_down = \
    "push ds" "mov ds,dx" "std" "rep movsw" "cld" "pop ds" \
    parm [es di] [dx si] [cx] modify exact [si di cx];
void stosw_fill(void __far *dst, unsigned cell, unsigned n);
#pragma aux stosw_fill = "rep stosw" \
    parm [es di] [ax] [cx] modify exact [di cx];

unsigned ticks(void)
{
    return BIOS_TICKS;
}

int scr_init(void)
{
    union REGS r;
    int mode;

    r.h.ah = 0x0F;
    int86(0x10, &r, &r);
    mode = r.h.al & 0x7F;
    page = r.h.bh;
    restore_ok = (mode == 2 || mode == 3 || mode == 7) &&
                 BIOS_WORD(0x4A) == SCR_COLS;
    if (restore_ok) {
        /* EGA and later: rows in the BIOS data area */
        r.h.ah = 0x12;
        r.h.bl = 0x10;
        int86(0x10, &r, &r);
        if (r.h.bl != 0x10 && BIOS_BYTE(0x84) + 1 != SCR_ROWS)
            restore_ok = 0;
    }
    if (!restore_ok) {
        /* graphics, 40 columns or more than 25 rows: 80x25 text */
        if (mode != 7)
            mode = (BIOS_WORD(0x10) & 0x30) == 0x30 ? 7 : 3;
        r.h.ah = 0x00;
        r.h.al = (unsigned char)mode;
        int86(0x10, &r, &r);
        page = 0;
    }
    scr_mono = mode == 7;
    scr_vram = MK_FP(scr_mono ? 0xB000 : 0xB800, BIOS_WORD(0x4E));

    r.h.ah = 0x03;
    r.h.bh = page;
    int86(0x10, &r, &r);
    saved_row = r.h.dh;
    saved_col = r.h.dl;
    saved_shape = r.x.cx;
    if (restore_ok) {
        saved = malloc(SCR_ROWS * SCR_COLS * sizeof *saved);
        if (saved != NULL)
            scr_save(0, saved, SCR_ROWS * SCR_COLS);
    }
    return 0;
}

void scr_done(void)
{
    if (beeping)
        outb(0x61, inb(0x61) & 0xFC);
    scr_cursor_show(1);
    if (saved != NULL) {
        scr_restore(0, saved, SCR_ROWS * SCR_COLS);
        free(saved);
        saved = NULL;
        scr_cursor(saved_row, saved_col);
    } else {
        scr_fill(0, 0x0720, SCR_ROWS * SCR_COLS);
        scr_cursor(0, 0);
    }
}

void scr_copy(unsigned dst, unsigned src, unsigned n)
{
    if (n == 0 || dst == src)
        return;
    if (dst < src)
        movsw_up(scr_vram + dst, scr_vram + src, n);
    else
        movsw_down(scr_vram + dst + n - 1, scr_vram + src + n - 1, n);
}

void scr_fill(unsigned at, cell_t cell, unsigned n)
{
    if (n != 0)
        stosw_fill(scr_vram + at, cell, n);
}

void scr_save(unsigned at, cell_t *buf, unsigned n)
{
    movsw_up(buf, scr_vram + at, n);
}

void scr_restore(unsigned at, const cell_t *buf, unsigned n)
{
    movsw_up(scr_vram + at, buf, n);
}

/* Text at row, col with attribute attr, clipped at the right edge. */
void scr_text(int row, int col, const char *s, int attr)
{
    cell_t FAR *p = scr_vram + row * SCR_COLS + col;
    cell_t a = (cell_t)attr << 8;

    while (*s && col++ < SCR_COLS)
        *p++ = a | (unsigned char)*s++;
}

void scr_cursor(int row, int col)
{
    union REGS r;

    r.h.ah = 0x02;
    r.h.bh = page;
    r.h.dh = (unsigned char)row;
    r.h.dl = (unsigned char)col;
    int86(0x10, &r, &r);
}

void scr_cursor_show(int on)
{
    union REGS r;

    r.h.ah = 0x01;
    r.x.cx = on ? saved_shape : 0x2000;
    int86(0x10, &r, &r);
}

/* A short click of about 800 Hz that does not stop the program;
 * scr_poll ends it. */
void scr_beep(void)
{
    unsigned div = (unsigned)(1193182UL / 800);

    outb(0x43, 0xB6);
    outb(0x42, (unsigned char)div);
    outb(0x42, (unsigned char)(div >> 8));
    outb(0x61, inb(0x61) | 3);
    beep_start = ticks();
    beeping = 1;
}

void scr_poll(void)
{
    if (beeping && (unsigned)(ticks() - beep_start) >= 2) {
        outb(0x61, inb(0x61) & 0xFC);
        beeping = 0;
    }
}

/* ------------------------------------------------------------------ */
/* keyboard */

/* Translate an INT 16h key (AH scan code, AL character). */
static int translate(unsigned ax)
{
    unsigned char al = (unsigned char)ax, ah = (unsigned char)(ax >> 8);

    if (ax == 0)
        return 0;                       /* Ctrl+Break */
    if (al == 0xE0 && ah != 0)
        return KEY_EXT(ah);             /* grey keys, enhanced keyboard */
    if (al != 0)
        return al;
    if (ah == 0x03)
        return K_NUL;
    return KEY_EXT(ah);
}

#ifndef TERM16_TEST

int key_get(void)
{
    static int enhanced = -1;
    union REGS r;

    if (enhanced < 0)
        enhanced = (BIOS_BYTE(0x96) & 0x10) != 0;
    /* INT 16h function 1 answers in ZF, which int86 does not return:
     * compare the BIOS keyboard buffer's head and tail instead. */
    if (BIOS_WORD(0x1A) == BIOS_WORD(0x1C))
        return 0;
    r.h.ah = enhanced ? 0x10 : 0x00;
    int86(0x16, &r, &r);
    return translate(r.x.ax);
}

#else

/*
 * Test keyboard.  %T16KEYS% is a file of little-endian 16-bit words:
 * an INT 16h key (scan code in the high byte), or one of
 *   FFFFh  wait until nothing has been received for half a second,
 *          counting from now
 *   FFFEh  append the screen to %T16SCR%
 *   FFFDh  wait two seconds
 *   FFFCh  wait until one more ZMODEM session has ended
 * Each key is given only once nothing has arrived for a tick.
 */

#include <fcntl.h>
#include <io.h>

static int keyfd = -2;
static unsigned last_rx, seen_rx, zm_waited;
static unsigned wait_until;
static unsigned char waiting;

static void dump_screen(void)
{
    static char line[SCR_COLS + 2];
    const char *name = getenv("T16SCR");
    int fd, row, col;

    if (name == NULL)
        return;
    fd = open(name, O_WRONLY | O_CREAT | O_APPEND | O_BINARY, 0x180);
    if (fd < 0)
        return;
    for (row = 0; row < SCR_ROWS; row++) {
        for (col = 0; col < SCR_COLS; col++)
            line[col] = (char)scr_vram[row * SCR_COLS + col];
        line[SCR_COLS] = '\r';
        line[SCR_COLS + 1] = '\n';
        write(fd, line, sizeof line);
    }
    write(fd, "----\r\n", 6);
    close(fd);
}

int key_get(void)
{
    unsigned char w[2];
    unsigned k, now = ticks();

    if (keyfd == -2) {
        const char *name = getenv("T16KEYS");

        keyfd = name != NULL ? open(name, O_RDONLY | O_BINARY) : -1;
        last_rx = now;
    }
    if (uart_received != seen_rx) {
        seen_rx = uart_received;
        last_rx = now;
    }
    if (waiting) {
        if (waiting == 1 && (unsigned)(now - last_rx) < 9)
            return 0;
        if (waiting == 2 && (int)(now - wait_until) < 0)
            return 0;
        if (waiting == 3 && zm_sessions <= zm_waited)
            return 0;
        if (waiting == 3)
            zm_waited++;
        waiting = 0;
    }
    if ((unsigned)(now - last_rx) < 1)
        return 0;
    if (keyfd < 0 || read(keyfd, w, 2) != 2)
        return 0;
    k = w[0] | (w[1] << 8);
    switch (k) {
    case 0xFFFF:
        waiting = 1;
        last_rx = now;                  /* quiet from now on */
        return 0;
    case 0xFFFE:
        dump_screen();
        return 0;
    case 0xFFFC:
        waiting = 3;
        return 0;
    case 0xFFFD:
        waiting = 2;
        wait_until = now + 2 * TICKS_PER_SEC;
        return 0;
    }
    return translate(k);
}

#endif
