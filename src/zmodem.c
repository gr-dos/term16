/*
 * zmodem.c - automatic, receive-only ZMODEM (docs/DESIGN.md section 7)
 *
 * Written from the protocol description (Chuck Forsberg, "The ZMODEM
 * Inter Application File Transfer Protocol", 1988); no code is taken
 * from lrzsz or any other implementation.
 *
 * zm_detect watches the terminal's input for a sender's ZRQINIT hex
 * header ("**", ZDLE, "B00"); zm_receive then answers ZRINIT and takes
 * files until ZFIN.  Each file is written to a .PRT name in the
 * download directory and renamed to its final, unused 8.3 name only
 * once complete, so an incomplete file is always the .PRT one.  Data
 * the sender repeats after an error is refused with ZRPOS until it
 * resends from the first missing byte.
 */

#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "term16.h"

#define BUF_MAX   1024                  /* longest data subpacket */
#define HOLD_MAX  16

/* Results of the readers, besides bytes, header types and frame ends */
#define R_TIMEOUT (-1)
#define R_ABORT   (-2)                  /* Esc at the keyboard */
#define R_CAN     (-3)                  /* the sender cancelled */
#define R_BAD     (-4)                  /* CRC, escape or length error */
#define GOT       0x100                 /* ZDLE frame end, in zdlread */

#define MAX_ERRORS 10                   /* in a row, then give up */
#define TO_HEADER  (10 * TICKS_PER_SEC)
#define TO_START   (5 * TICKS_PER_SEC)  /* before the first ZFILE */
#define MAX_START  4
#define TO_DATA    (10 * TICKS_PER_SEC)

/* ------------------------------------------------------------------ */
/* detection */

static unsigned char hold[HOLD_MAX];
static int nhold, dstate;

/*
 * Feed one received byte.  Returns 0 if it is held back as a possible
 * start of a ZMODEM header, 1 if the held bytes (now including this
 * one) should be shown (zm_pending), or 2 if a transfer is starting.
 */
int zm_detect(int c)
{
    static const char want[] = "\030B00";
    int ok;

    if (nhold < HOLD_MAX)
        hold[nhold++] = (unsigned char)c;
    if (dstate == 0)
        ok = c == ZPAD;
    else if (dstate == 1 && c == ZPAD)
        ok = 1, dstate = 0;             /* any number of pads */
    else
        ok = c == (unsigned char)want[dstate - 1];
    if (!ok) {
        dstate = 0;
        return 1;
    }
    if (++dstate == 5) {
        dstate = 0;
        nhold = 0;
        return 2;
    }
    return 0;
}

/* Move the held bytes to out (HOLD_MAX bytes); returns how many. */
int zm_pending(unsigned char *out)
{
    int n = nhold;

    memcpy(out, hold, (size_t)n);
    nhold = 0;
    dstate = 0;
    return n;
}

/* ------------------------------------------------------------------ */
/* progress view: a box in the middle of the remote viewport */

#define BOX_ROW  7
#define BOX_COL  9
#define BOX_W    62
#define BOX_H    9

static cell_t under[BOX_W * BOX_H];
static unsigned char box_attr;

static void box_line(int i, const char *s)
{
    static char line[BOX_W + 1];
    int k = 0;

    line[k++] = (char)0xBA;
    line[k++] = ' ';
    while (*s && k < BOX_W - 2)
        line[k++] = *s++;
    while (k < BOX_W - 1)
        line[k++] = ' ';
    line[k++] = (char)0xBA;
    line[k] = '\0';
    scr_text(BOX_ROW + i, BOX_COL, line, box_attr);
}

static void box_open(void)
{
    static char edge[BOX_W + 1];
    static const char title[] = " ZMODEM download ";
    int i, r;

    for (r = 0; r < BOX_H; r++)
        scr_save((unsigned)((BOX_ROW + r) * SCR_COLS + BOX_COL),
                 under + r * BOX_W, BOX_W);
    box_attr = (unsigned char)(scr_mono ? 0x70 : 0x1F);
    for (i = 0; i < BOX_W; i++)
        edge[i] = (char)0xCD;
    edge[BOX_W] = '\0';
    edge[0] = (char)0xC9;
    edge[BOX_W - 1] = (char)0xBB;
    memcpy(edge + 3, title, sizeof title - 1);
    scr_text(BOX_ROW, BOX_COL, edge, box_attr);
    for (i = 1; i < BOX_H - 1; i++)
        box_line(i, "");
    edge[0] = (char)0xC8;
    edge[BOX_W - 1] = (char)0xBC;
    for (i = 3; i < 3 + (int)sizeof title - 1; i++)
        edge[i] = (char)0xCD;
    scr_text(BOX_ROW + BOX_H - 1, BOX_COL, edge, box_attr);
    scr_cursor_show(0);
}

static void box_close(void)
{
    int r;

    for (r = 0; r < BOX_H; r++)
        scr_restore((unsigned)((BOX_ROW + r) * SCR_COLS + BOX_COL),
                    under + r * BOX_W, BOX_W);
    scr_cursor_show(1);
}

static char *cat_num(char *p, unsigned long n)
{
    char tmp[12];
    int i = 0;

    while (*p)
        p++;
    do {
        tmp[i++] = (char)('0' + n % 10);
        n /= 10;
    } while (n);
    while (i)
        *p++ = tmp[--i];
    *p = '\0';
    return p;
}

/* Transfer state, for the view */
static char shown_name[13];
static char sender_name[40];
static unsigned long pos, size;
static int have_size;
static unsigned files, errors;

static void show_file(void)
{
    char line[64];

    strcpy(line, "File:      ");
    strcat(line, shown_name);
    if (sender_name[0] != '\0') {
        strcat(line, "  (");
        strcat(line, sender_name);
        strcat(line, ")");
    }
    box_line(1, line);
}

static void show_progress(void)
{
    char line[64];

    strcpy(line, "Received:  ");
    cat_num(line, pos);
    if (have_size) {
        strcat(line, " of ");
        cat_num(line, size);
        strcat(line, " bytes  ");
        cat_num(line, size == 0 ? 100 :
                pos >= size ? 100 :
                size > 40000000UL ? pos / (size / 100) : pos * 100 / size);
        strcat(line, "%");
    } else
        strcat(line, " bytes");
    box_line(2, line);
    strcpy(line, "Errors:    ");
    cat_num(line, errors);
    strcat(line, "   Files done: ");
    cat_num(line, files);
    box_line(3, line);
}

static void show_status(const char *a, const char *b)
{
    char line[64];

    strcpy(line, a);
    if (b != NULL)
        strcat(line, b);
    box_line(5, line);
}

/* ------------------------------------------------------------------ */
/* reading */

static unsigned timeout, header_timeout;

unsigned zm_sessions;
static unsigned char crc32_data;        /* the last binary header's CRC */

/* A raw byte, R_TIMEOUT or R_ABORT. */
static int zgetc(void)
{
    unsigned start;
    int c = uart_getc();

    if (c >= 0)
        return c;
    start = ticks();
    for (;;) {
        c = uart_getc();
        if (c >= 0)
            return c;
        scr_poll();
        if (key_get() == K_ESC)
            return R_ABORT;
        if ((unsigned)(ticks() - start) >= timeout)
            return R_TIMEOUT;
    }
}

/* A byte with ZDLE escapes removed, GOT | a frame end, or an error.
 * Bare XON and XOFF are flow control noise and skipped. */
static int zdlread(void)
{
    int c, cans;

    for (;;) {
        c = zgetc();
        if (c < 0)
            return c;
        if (c == ZDLE)
            break;
        if ((c & 0x7F) == 0x11 || (c & 0x7F) == 0x13)
            continue;
        return c;
    }
    cans = 1;
    for (;;) {
        c = zgetc();
        if (c < 0)
            return c;
        if (c == ZDLE) {                /* CAN: five in a row cancel */
            if (++cans >= 5)
                return R_CAN;
            continue;
        }
        if ((c & 0x7F) == 0x11 || (c & 0x7F) == 0x13)
            continue;
        if (cans > 1)
            return R_BAD;               /* ZDLE ZDLE is never data */
        switch (c) {
        case ZCRCE:
        case ZCRCG:
        case ZCRCQ:
        case ZCRCW:
            return GOT | c;
        case ZRUB0:
            return 0x7F;
        case ZRUB1:
            return 0xFF;
        }
        if ((c & 0x60) == 0x40)
            return c ^ 0x40;
        return R_BAD;
    }
}

static int hexval(int c)
{
    c &= 0x7F;
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* The next header: its type with its four data bytes in hdr, or an
 * error.  Anything before "*", ZDLE is skipped, up to a limit. */
static int recv_header(unsigned char *hdr)
{
    unsigned char b[9];
    unsigned garbage = 0;
    int c, i, n, cans = 0, fmt;

    timeout = header_timeout;
    for (;;) {
        c = zgetc();
        if (c < 0)
            return c;
        if (c == ZDLE) {
            if (++cans >= 5)
                return R_CAN;
        } else
            cans = 0;
        if ((c & 0x7F) != ZPAD) {
            if (++garbage > 2 * RX_SIZE + BUF_MAX * 2)
                return R_BAD;
            continue;
        }
        do
            c = zgetc();
        while ((c & 0x7F) == ZPAD && c >= 0);
        if (c < 0)
            return c;
        if (c != ZDLE)
            continue;
        fmt = zgetc();
        if (fmt < 0)
            return fmt;
        if (fmt == ZBIN || fmt == ZBIN32 || fmt == ZHEX)
            break;
        cans = fmt == ZDLE ? 2 : 0;
    }

    if (fmt == ZHEX) {
        unsigned crc = 0;

        for (i = 0; i < 7; i++) {
            int hi, lo;

            if ((c = zgetc()) < 0)
                return c;
            hi = hexval(c);
            if ((c = zgetc()) < 0)
                return c;
            lo = hexval(c);
            if (hi < 0 || lo < 0)
                return R_BAD;
            b[i] = (unsigned char)(hi << 4 | lo);
            crc = zm_crc16(crc, b[i]);
        }
        if (crc != 0)
            return R_BAD;
    } else {
        n = fmt == ZBIN32 ? 9 : 7;
        for (i = 0; i < n; i++) {
            c = zdlread();
            if (c < 0)
                return c;
            if (c & GOT)
                return R_BAD;
            b[i] = (unsigned char)c;
        }
        if (fmt == ZBIN32) {
            unsigned long crc = 0xFFFFFFFFUL;

            for (i = 0; i < 9; i++)
                crc = zm_crc32(crc, b[i]);
            if (crc != ZM_CRC32_GOOD)
                return R_BAD;
        } else {
            unsigned crc = 0;

            for (i = 0; i < 7; i++)
                crc = zm_crc16(crc, b[i]);
            if (crc != 0)
                return R_BAD;
        }
        crc32_data = (unsigned char)(fmt == ZBIN32);
    }
    memcpy(hdr, b + 1, 4);
    return b[0];
}

/* A data subpacket into buf: its frame end (ZCRCE, ZCRCG, ZCRCQ or
 * ZCRCW) with its length in *len, or an error. */
static int recv_data(unsigned char *buf, unsigned *len)
{
    unsigned n = 0, crc16 = 0;
    unsigned long crc32 = 0xFFFFFFFFUL;
    int c, i, end;

    timeout = TO_DATA;
    for (;;) {
        c = zdlread();
        if (c < 0)
            return c;
        if (c & GOT)
            break;
        if (n >= BUF_MAX)
            return R_BAD;
        buf[n++] = (unsigned char)c;
        if (crc32_data)
            crc32 = zm_crc32(crc32, (unsigned char)c);
        else
            crc16 = zm_crc16(crc16, (unsigned char)c);
    }
    end = c & 0xFF;
    if (crc32_data) {
        crc32 = zm_crc32(crc32, (unsigned char)end);
        for (i = 0; i < 4; i++) {
            c = zdlread();
            if (c < 0)
                return c;
            if (c & GOT)
                return R_BAD;
            crc32 = zm_crc32(crc32, (unsigned char)c);
        }
        if (crc32 != ZM_CRC32_GOOD)
            return R_BAD;
    } else {
        crc16 = zm_crc16(crc16, (unsigned char)end);
        for (i = 0; i < 2; i++) {
            c = zdlread();
            if (c < 0)
                return c;
            if (c & GOT)
                return R_BAD;
            crc16 = zm_crc16(crc16, (unsigned char)c);
        }
        if (crc16 != 0)
            return R_BAD;
    }
    *len = n;
    return end;
}

/* ------------------------------------------------------------------ */
/* sending */

static void send_hex(int type, unsigned long arg)
{
    char h[24];

    uart_write(h, (unsigned)zm_hexheader(h, type, arg));
}

static void send_rinit(void)
{
    send_hex(ZRINIT, (unsigned long)(CANFDX | CANOVIO | CANFC32) << 24);
}

/* Cancel the sender, then discard what it still sends (up to a second
 * of quiet, at most five seconds). */
static void send_cancel(void)
{
    static const char seq[] =
        "\030\030\030\030\030\030\030\030\030\030"
        "\010\010\010\010\010\010\010\010\010\010";
    unsigned start = ticks(), quiet = start;

    uart_write(seq, sizeof seq - 1);
    while ((unsigned)(ticks() - quiet) < TICKS_PER_SEC &&
           (unsigned)(ticks() - start) < 5 * TICKS_PER_SEC)
        if (uart_getc() >= 0)
            quiet = ticks();
}

static unsigned long hdr_pos(const unsigned char *h)
{
    return (unsigned long)h[0] | (unsigned long)h[1] << 8 |
           (unsigned long)h[2] << 16 | (unsigned long)h[3] << 24;
}

/* ------------------------------------------------------------------ */
/* receiving */

static unsigned char buf[BUF_MAX + 1];
static const char *fail_msg;
static char part_path[DIR_MAX + 14], part_name[13];
static char base[10], ext[4];
static unsigned char part_complete;     /* the kept .PRT is all there */

static void why(int r)
{
    fail_msg = r == R_ABORT ? "Cancelled at the keyboard." :
               r == R_CAN ? "Cancelled by the sender." :
               r == R_TIMEOUT ? "The sender stopped responding." :
               "Too many errors.";
}

/* After a ZFILE header and its subpacket in buf: receive the file.
 * Returns 0 when done, or -1 to end the session (fail_msg says why). */
static int receive_file(const struct config *c, unsigned n)
{
    unsigned char hdr[4];
    char final[13], path[DIR_MAX + 14];
    unsigned len, bad = 0;
    int fd, t, r;

    buf[n] = '\0';
    {
        const char *info = (const char *)buf + strlen((char *)buf) + 1;
        size_t k = strlen((char *)buf);

        if (k >= sizeof sender_name)
            k = sizeof sender_name - 1;
        memcpy(sender_name, buf, k);
        sender_name[k] = '\0';
        have_size = info < (const char *)buf + n &&
                    *info >= '0' && *info <= '9';
        size = have_size ? strtoul(info, NULL, 10) : 0;
    }
    dl_safename((const char *)buf, base, ext);
    if (dl_unique(c->dldir, base, "PRT", df_exists, part_name) < 0) {
        fail_msg = "No unused file name in the download directory.";
        send_cancel();
        return -1;
    }
    dl_join(part_path, c->dldir, part_name);
    fd = open(part_path, O_WRONLY | O_CREAT | O_EXCL | O_BINARY,
              S_IREAD | S_IWRITE);
    if (fd < 0) {
        fail_msg = "Cannot create a file in the download directory.";
        send_cancel();
        return -1;
    }
    strcpy(shown_name, part_name);
    pos = 0;
    show_file();
    show_progress();
    show_status("Receiving...", NULL);
    send_hex(ZRPOS, 0);

    for (;;) {
        t = recv_header(hdr);
        if (t == ZDATA) {
            if (hdr_pos(hdr) != pos) {
                if (++bad > MAX_ERRORS)
                    goto fail_errors;
                errors++;
                send_hex(ZRPOS, pos);
                continue;
            }
            for (;;) {
                r = recv_data(buf, &len);
                if (r < 0) {
                    if (r == R_ABORT || r == R_CAN) {
                        why(r);
                        goto fail;
                    }
                    errors++;
                    if (++bad > MAX_ERRORS)
                        goto fail_errors;
                    show_progress();
                    send_hex(ZRPOS, pos);
                    break;
                }
                bad = 0;
                if (len != 0 && write(fd, buf, len) != (int)len) {
                    fail_msg = "Disk full or write error.";
                    send_cancel();
                    goto fail_quiet;
                }
                pos += len;
                show_progress();
                if (r == ZCRCW || r == ZCRCQ)
                    send_hex(ZACK, pos);
                if (r == ZCRCW || r == ZCRCE)
                    break;
            }
            continue;
        }
        switch (t) {
        case ZEOF:
            if (hdr_pos(hdr) != pos)
                continue;               /* stale; data still to come */
            close(fd);
            /* Count the file only once it has its final name; if it
             * cannot get one, keep the complete .PRT and stop. */
            if (dl_unique(c->dldir, base, ext, df_exists, final) < 0) {
                fail_msg = "No unused name for the received file.";
                goto fail_renamed;
            }
            dl_join(path, c->dldir, final);
            if (df_rename(part_path, path) < 0) {
                fail_msg = "Cannot rename the received file.";
                goto fail_renamed;
            }
            strcpy(shown_name, final);
            files++;
            show_file();
            show_progress();
            show_status("Saved as ", shown_name);
            return 0;
        case ZFILE:
            /* It did not hear our ZRPOS: skip its subpacket, answer again */
            recv_data(buf, &len);
            send_hex(ZRPOS, pos);
            continue;
        case ZNAK:
            send_hex(ZRPOS, pos);
            continue;
        case R_ABORT:
        case R_CAN:
            why(t);
            goto fail;
        case ZFIN:
        case ZSKIP:
        case ZABORT:
        case ZFERR:
            fail_msg = "The sender ended the transfer early.";
            goto fail;
        }
        /* timeout, bad header, or a header that makes no sense here */
        errors++;
        show_progress();
        if (++bad > MAX_ERRORS)
            goto fail_errors;
        send_hex(ZRPOS, pos);
    }

fail_errors:
    why(t == R_TIMEOUT ? R_TIMEOUT : R_BAD);
fail:
    send_cancel();
fail_quiet:
    close(fd);
    if (pos == 0)
        df_unlink(part_path);
    else
        strcpy(shown_name, part_name);
    return -1;

fail_renamed:
    send_cancel();
    part_complete = 1;
    strcpy(shown_name, part_name);
    return -1;
}

void zm_receive(const struct config *c)
{
    unsigned char hdr[4];
    unsigned len, tries = 0, wait;
    int t, r, ok = 0;
    char line[64];

    files = errors = 0;
    shown_name[0] = sender_name[0] = '\0';
    part_name[0] = '\0';
    part_complete = 0;
    pos = size = 0;
    have_size = 0;
    fail_msg = NULL;
    box_open();
    box_line(7, "Esc cancels");
    show_status("Starting...", NULL);

    if (!df_isdir(c->dldir)) {
        fail_msg = "The download directory does not exist.";
        send_cancel();
        goto done;
    }

    send_rinit();
    header_timeout = TO_START;
    for (;;) {
        t = recv_header(hdr);
        switch (t) {
        case ZRQINIT:
            if (++tries > MAX_ERRORS) {
                why(R_TIMEOUT);
                send_cancel();
                goto done;
            }
            send_rinit();
            continue;
        case ZSINIT:
            r = recv_data(buf, &len);
            if (r < 0) {
                send_hex(ZNAK, 0);
                continue;
            }
            send_hex(ZACK, 1);
            continue;
        case ZFILE:
            r = recv_data(buf, &len);
            if (r < 0) {
                if (r == R_ABORT || r == R_CAN) {
                    why(r);
                    if (r == R_ABORT)
                        send_cancel();
                    goto done;
                }
                errors++;
                send_rinit();
                continue;
            }
            header_timeout = TO_HEADER;
            if (receive_file(c, len) < 0)
                goto done;
            tries = 0;
            send_rinit();
            continue;
        case ZFIN:
            send_hex(ZFIN, 0);
            /* The sender ends with the rest of its header and "OO";
             * anything else is terminal data, which main.c shows. */
            timeout = TICKS_PER_SEC;
            for (r = 0, len = 0; r < 2 && len < 8; len++) {
                int b = zgetc();

                if (b < 0)
                    break;
                if (b == 'O')
                    r++;
                else if ((b & 0x7F) != '\r' && (b & 0x7F) != '\n' &&
                         (b & 0x7F) != 0x11) {
                    zm_detect(b);
                    break;
                }
            }
            ok = 1;
            goto done;
        case ZFREECNT:
            send_hex(ZACK, 0);
            continue;
        case ZCOMMAND:
            r = recv_data(buf, &len);
            send_hex(ZCOMPL, 1);        /* never run: report failure */
            continue;
        case R_ABORT:
        case R_CAN:
            why(t);
            if (t == R_ABORT)
                send_cancel();
            goto done;
        }
        /* timeout, a bad header, or one out of place */
        if (++tries > (header_timeout == TO_START ? MAX_START : MAX_ERRORS)) {
            why(t == R_TIMEOUT ? R_TIMEOUT : R_BAD);
            send_cancel();
            goto done;
        }
        if (t == ZDATA || t == ZEOF)
            continue;                   /* leftovers of a finished file */
        send_rinit();
    }

done:
    if (ok) {
        strcpy(line, "Done: ");
        cat_num(line, files);
        strcat(line, files == 1 ? " file received." : " files received.");
        show_status(line, NULL);
        wait = 2 * TICKS_PER_SEC;
    } else {
        show_status("Failed: ", fail_msg);
        if (part_name[0] != '\0' && strcmp(shown_name, part_name) == 0) {
            strcpy(line, part_complete ? "Received file kept as " :
                                         "Incomplete file kept as ");
            strcat(line, part_name);
            box_line(6, line);
        }
        wait = 10 * TICKS_PER_SEC;
    }
    show_progress();
    box_line(7, "Press any key to continue");
    {
        unsigned start = ticks();

        while ((unsigned)(ticks() - start) < wait && key_get() == 0)
            scr_poll();
    }
    box_close();
    zm_sessions++;
}
