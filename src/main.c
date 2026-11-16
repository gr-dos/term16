/*
 * term16 - serial terminal for 16-bit DOS
 *
 * main.c: the command line, the session loop and cleanup.  With no
 * arguments the configuration menu opens; with settings on the command
 * line the session starts at once (docs/DESIGN.md section 4).
 */

#include <dos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "term16.h"

#ifndef VERSION
#define VERSION "unknown"
#endif

static struct config cfg;
static int state = ST_CLOSED;

static void err(const char *what, const char *arg)
{
    fputs("term16: ", stderr);
    fputs(what, stderr);
    if (arg != NULL) {
        fputs(" '", stderr);
        fputs(arg, stderr);
        fputs("'", stderr);
    }
    fputs("\n", stderr);
}

static void usage(void)
{
    fputs(
        "Usage: term16 [OPTION]...\n"
        "Serial terminal with automatic ZMODEM download.\n"
        "With no options, opens the settings menu; with settings, starts\n"
        "the session at once.\n"
        "\n"
        "  -p, --port=PORT        COM1 to COM4 (default COM1)\n"
        "  -b, --baud=RATE        110 to 115200 (default 9600)\n"
        "      --data=BITS        data bits, 5 to 8 (default 8)\n"
        "      --parity=PARITY    none, odd, even, mark or space (default none)\n"
        "      --stop=BITS        stop bits, 1 or 2 (default 1)\n"
        "      --flow=FLOW        none, rtscts or xonxoff (default none)\n"
        "  -t, --term=MODE        ansi (code page 437) or utf8 (default ansi)\n"
        "  -d, --download-dir=DIR directory for ZMODEM downloads\n"
        "                         (default: the current directory)\n"
        "  -m, --menu             open the menu even with settings given\n"
        "  -h, --help             display this help and exit\n"
        "      --version          output version information and exit\n"
        "\n"
        "In a session, Alt-M opens the menu and Alt-X exits.  Files sent\n"
        "with ZMODEM (sz on the remote host) are saved automatically; Esc\n"
        "cancels a transfer.\n", stdout);
}

/* ------------------------------------------------------------------ */
/* command line */

static const struct {
    const char *name;
    char letter;
    int field;                          /* F_*, or -1 for no value */
} options[] = {
    { "port", 'p', F_PORT },
    { "baud", 'b', F_BAUD },
    { "data", 0, F_DATA },
    { "parity", 0, F_PARITY },
    { "stop", 0, F_STOP },
    { "flow", 0, F_FLOW },
    { "term", 't', F_TERM },
    { "download-dir", 'd', F_DIR },
};

#define NOPTIONS (sizeof options / sizeof options[0])

enum { ARGS_SESSION, ARGS_MENU, ARGS_DONE, ARGS_BAD };

static int parse_args(int argc, char **argv)
{
    int i, settings = 0, menu = 0;
    unsigned k;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i], *val = NULL;
        int field = -1;

        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0 ||
            strcmp(a, "-?") == 0 || strcmp(a, "/?") == 0) {
            usage();
            return ARGS_DONE;
        }
        if (strcmp(a, "--version") == 0) {
            fputs("term16 (gr-dos term16) " VERSION "\n", stdout);
            return ARGS_DONE;
        }
        if (strcmp(a, "--menu") == 0 || strcmp(a, "-m") == 0) {
            menu = 1;
            continue;
        }
        if (a[0] == '-' && a[1] == '-') {
            const char *eq = strchr(a + 2, '=');
            size_t n = eq != NULL ? (size_t)(eq - a - 2) : strlen(a + 2);

            for (k = 0; k < NOPTIONS; k++)
                if (strlen(options[k].name) == n &&
                    strncmp(options[k].name, a + 2, n) == 0)
                    break;
            if (k == NOPTIONS) {
                err("unrecognized option", a);
                return ARGS_BAD;
            }
            field = options[k].field;
            if (eq != NULL)
                val = eq + 1;
        } else if (a[0] == '-' && a[1] != '\0') {
            for (k = 0; k < NOPTIONS; k++)
                if (options[k].letter == a[1])
                    break;
            if (k == NOPTIONS) {
                err("unrecognized option", a);
                return ARGS_BAD;
            }
            field = options[k].field;
            if (a[2] != '\0')
                val = a + 2;
        } else {
            err("extra operand", a);
            return ARGS_BAD;
        }
        if (val == NULL) {
            if (++i >= argc) {
                err("option requires an argument", a);
                return ARGS_BAD;
            }
            val = argv[i];
        }
        if (field == F_DIR) {
            if (df_fullpath(cfg.dldir, val) < 0 || !df_isdir(cfg.dldir)) {
                err("download directory not found", val);
                return ARGS_BAD;
            }
        } else if (cfg_parse(&cfg, field, val) < 0) {
            err("invalid value", val);
            return ARGS_BAD;
        }
        settings = 1;
    }
    return settings && !menu ? ARGS_SESSION : ARGS_MENU;
}

/* ------------------------------------------------------------------ */
/* handlers */

static void __interrupt __far on_break(void)
{
    /* Ctrl+Break and Ctrl+C: ignored, so DOS never ends the program
     * with the UART interrupt still hooked. */
}

static int __cdecl __far on_harderr(unsigned deverror, unsigned errcode,
                                    unsigned __far *devhdr)
{
    (void)deverror;
    (void)errcode;
    (void)devhdr;
    return _HARDERR_FAIL;               /* the DOS call fails instead */
}

/* ------------------------------------------------------------------ */
/* session */

void term_reply(const char *s)
{
    uart_write(s, (unsigned)strlen(s));
}

static void draw_status(void)
{
    char line[SCR_COLS + 1];

    cfg_status(&cfg, state, line);
    scr_text(STATUS_ROW, 0, line, 0x70);
}

static void send_str(const char *s)
{
    uart_write(s, (unsigned)strlen(s));
}

/* Alt+letter scan codes from 10h, for Meta (ESC letter) */
static const char alt_letters[] =
    "qwertyuiop\0\0\0\0asdfghjkl\0\0\0\0\0zxcvbnm";

static void send_key(int k)
{
    static char seq[8];
    unsigned scan;

    if (k < 0x100) {
        seq[0] = (char)(k == 0x08 ? 0x7F : k == 0x7F ? 0x08 : k);
        uart_write(seq, 1);
        return;
    }
    if (k == K_NUL) {
        seq[0] = 0;
        uart_write(seq, 1);
        return;
    }
    switch (k) {
    case K_UP:    send_str(term_appcursor ? "\033OA" : "\033[A"); return;
    case K_DOWN:  send_str(term_appcursor ? "\033OB" : "\033[B"); return;
    case K_RIGHT: send_str(term_appcursor ? "\033OC" : "\033[C"); return;
    case K_LEFT:  send_str(term_appcursor ? "\033OD" : "\033[D"); return;
    case K_HOME:  send_str("\033[1~"); return;
    case K_INS:   send_str("\033[2~"); return;
    case K_DEL:   send_str("\033[3~"); return;
    case K_END:   send_str("\033[4~"); return;
    case K_PGUP:  send_str("\033[5~"); return;
    case K_PGDN:  send_str("\033[6~"); return;
    case K_BTAB:  send_str("\033[Z"); return;
    }
    scan = (unsigned)k & 0xFF;
    if (scan >= 0x3B && scan <= 0x3E) {             /* F1-F4 */
        strcpy(seq, "\033OP");
        seq[2] = (char)('P' + scan - 0x3B);
        send_str(seq);
        return;
    }
    if ((scan >= 0x3F && scan <= 0x44) || scan == 0x85 || scan == 0x86) {
        /* F5-F10, F11, F12 */
        static const char codes[] = "151718192021" "2324";
        int i = scan >= 0x85 ? (int)scan - 0x85 + 6 : (int)scan - 0x3F;

        strcpy(seq, "\033[15~");
        seq[2] = codes[i * 2];
        seq[3] = codes[i * 2 + 1];
        send_str(seq);
        return;
    }
    if (scan >= 0x10 && scan < 0x10 + sizeof alt_letters - 1 &&
        alt_letters[scan - 0x10] != 0) {
        seq[0] = 0x1B;
        seq[1] = alt_letters[scan - 0x10];
        uart_write(seq, 2);
    }
}

/* Open the port for cfg; on failure state is ST_ERROR and *msg says why */
static int open_port(const char **msg)
{
    if (uart_open(&cfg) < 0) {
        state = ST_ERROR;
        *msg = uart_error;
        return -1;
    }
    state = ST_OPEN;
    return 0;
}

/* The menu during a session; 0 to go on, -1 to exit. */
static int session_menu(void)
{
    cell_t *screen = malloc(SCR_COLS * TERM_ROWS * sizeof *screen);
    const char *msg = NULL;
    struct config old = cfg;
    int r;

    if (screen != NULL)
        scr_save(0, screen, SCR_COLS * TERM_ROWS);
    for (;;) {
        r = menu_run(&cfg, 1, state, msg);
        if (r == MENU_EXIT) {
            free(screen);
            return -1;
        }
        if (r == MENU_RESUME)
            break;
        if (!cfg_same_line(&old, &cfg) || state != ST_OPEN) {
            uart_close();
            state = ST_CLOSED;
            if (open_port(&msg) < 0)
                continue;
        }
        break;
    }
    if (screen != NULL) {
        scr_restore(0, screen, SCR_COLS * TERM_ROWS);
        free(screen);
    } else
        term_clear();
    term_mode(cfg.term == TERM_UTF8);
    draw_status();
    return 0;
}

static void session(void)
{
    static unsigned char out[128];
    unsigned last = ticks();
    int k;

    term_reset(cfg.term == TERM_UTF8);
    draw_status();
    term_cursor();
    for (;;) {
        unsigned n = 0;
        int c, r;

        while (n < sizeof out - 16 && (c = uart_getc()) >= 0) {
            r = zm_detect(c);
            if (r == 1)
                n += (unsigned)zm_pending(out + n);
            else if (r == 2) {
                term_write(out, n);
                n = 0;
                zm_receive(&cfg);
                draw_status();
                n = (unsigned)zm_pending(out);
            }
        }
        if (n != 0) {
            term_write(out, n);
            term_cursor();
            last = ticks();
        } else if ((unsigned)(ticks() - last) >= 3) {
            /* A held '*' that started nothing */
            n = (unsigned)zm_pending(out);
            if (n != 0) {
                term_write(out, n);
                term_cursor();
            }
            last = ticks();
        }
        uart_poll();
        scr_poll();
        k = key_get();
        if (k == 0)
            continue;
        if (k == K_ALT_X)
            return;
        if (k == K_ALT_M) {
            if (session_menu() < 0)
                return;
            term_cursor();
            continue;
        }
        if (state == ST_OPEN)
            send_key(k);
    }
}

int main(int argc, char **argv)
{
    const char *msg = NULL;
    int r;

    cfg_default(&cfg);
    if (df_fullpath(cfg.dldir, ".") < 0)
        strcpy(cfg.dldir, "C:\\");
    r = parse_args(argc, argv);
    if (r == ARGS_DONE)
        return 0;
    if (r == ARGS_BAD) {
        fputs("Try 'term16 --help' for more information.\n", stderr);
        return 1;
    }

    zm_crc_init();
    _dos_setvect(0x23, on_break);
    _harderr(on_harderr);
    if (r == ARGS_SESSION) {
        if (uart_open(&cfg) < 0) {
            err(uart_error, NULL);
            return 1;
        }
        state = ST_OPEN;
    }
    scr_init();

    while (state != ST_OPEN) {
        if (menu_run(&cfg, 0, state, msg) != MENU_START) {
            scr_done();
            return 0;
        }
        open_port(&msg);
    }
    session();
    uart_close();
    scr_done();
    return 0;
}
