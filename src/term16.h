/*
 * term16.h - shared declarations
 *
 * Modules (docs/DESIGN.md section 2):
 *   main.c    command line, session loop, cleanup
 *   config.c  settings, their names and the status bar text (portable)
 *   uart.c    8250/16450/16550 UART, IRQ handler, receive ring buffer
 *   uprobe.c  UART detection (portable)
 *   screen.c  text video memory, cursor, speaker, keyboard, clock
 *   term.c    ANSI/VT100 parser and renderer (portable)
 *   cp437.c   UTF-8 to code page 437 (portable)
 *   menu.c    the configuration menu
 *   zmodem.c  the receive-only ZMODEM implementation
 *   zmcrc.c   ZMODEM CRCs and header encoding (portable)
 *   dlname.c  safe DOS names for received files (portable)
 *   dosfile.c files and directories for downloads
 *
 * "Portable" modules are also compiled on the build host for the unit
 * tests in tests/unit.
 */

#ifndef TERM16_H
#define TERM16_H

#ifdef __WATCOMC__
#define FAR __far
#else
#define FAR
#endif

#define SCR_COLS  80
#define SCR_ROWS  25
#define TERM_ROWS 24                    /* the remote viewport */
#define STATUS_ROW 24                   /* the local status bar */

typedef unsigned short cell_t;          /* attribute << 8 | character */

/* ------------------------------------------------------------------ */
/* config.c */

enum { PAR_NONE, PAR_ODD, PAR_EVEN, PAR_MARK, PAR_SPACE };
enum { FLOW_NONE, FLOW_RTSCTS, FLOW_XONXOFF };
enum { TERM_ANSI, TERM_UTF8 };
enum { ST_CLOSED, ST_OPEN, ST_ERROR };

#define DIR_MAX 80

struct config {
    int port;                           /* 1 to 4 */
    unsigned long baud;
    int data;                           /* 5 to 8 */
    int parity;
    int stop;                           /* 1 or 2 */
    int flow;
    int term;
    char dldir[DIR_MAX];
};

/* The settings the menu offers, in order */
enum { F_PORT, F_BAUD, F_DATA, F_PARITY, F_STOP, F_FLOW, F_TERM, F_DIR,
       F_COUNT };

extern const unsigned long cfg_bauds[];
extern const int cfg_nbauds;

void cfg_default(struct config *c);
int  cfg_parse(struct config *c, int field, const char *value);
const char *cfg_name(const struct config *c, int field, char *buf);
void cfg_step(struct config *c, int field, int dir);
void cfg_status(const struct config *c, int state, char *line);
int  cfg_same_line(const struct config *a, const struct config *b);

/* ------------------------------------------------------------------ */
/* uart.c */

#define RX_SIZE 4096                    /* a power of two */

int  uart_open(const struct config *c);  /* 0, or an error message index */
void uart_close(void);
int  uart_getc(void);                   /* a byte, or -1 if none */
unsigned uart_count(void);              /* bytes waiting */
int  uart_putc(int c);                  /* 0, or -1 if it timed out */
void uart_write(const char *s, unsigned n);
void uart_poll(void);
void uart_drain(void);                  /* wait until sent */
extern unsigned long uart_overruns;     /* bytes lost: ring or UART full */
extern volatile unsigned uart_received;
extern const char *uart_error;

/* uprobe.c */
typedef unsigned char (*port_in_fn)(unsigned port);
typedef void (*port_out_fn)(unsigned port, unsigned char v);
int  uart_probe(unsigned base, port_in_fn in, port_out_fn out,
                unsigned char *old_lcr);        /* 0, or -1 if none */

/* ------------------------------------------------------------------ */
/* screen.c (and the keyboard and the clock) */

extern cell_t FAR *scr_vram;
extern int scr_mono;

int  scr_init(void);
void scr_done(void);
void scr_copy(unsigned dst, unsigned src, unsigned n);  /* cells, overlap ok */
void scr_fill(unsigned at, cell_t cell, unsigned n);
void scr_text(int row, int col, const char *s, int attr);
void scr_cursor(int row, int col);
void scr_cursor_show(int on);
void scr_beep(void);
void scr_poll(void);
void scr_save(unsigned at, cell_t *buf, unsigned n);
void scr_restore(unsigned at, const cell_t *buf, unsigned n);

/* Keys: a character 1-255, or KEY_EXT(scan) for a key without one */
#define KEY_EXT(scan) (0x100 | (scan))
#define K_UP     KEY_EXT(0x48)
#define K_DOWN   KEY_EXT(0x50)
#define K_LEFT   KEY_EXT(0x4B)
#define K_RIGHT  KEY_EXT(0x4D)
#define K_HOME   KEY_EXT(0x47)
#define K_END    KEY_EXT(0x4F)
#define K_PGUP   KEY_EXT(0x49)
#define K_PGDN   KEY_EXT(0x51)
#define K_INS    KEY_EXT(0x52)
#define K_DEL    KEY_EXT(0x53)
#define K_ALT_M  KEY_EXT(0x32)
#define K_ALT_X  KEY_EXT(0x2D)
#define K_BTAB   KEY_EXT(0x0F)
#define K_F1     KEY_EXT(0x3B)          /* F1-F10 are 3Bh-44h */
#define K_NUL    KEY_EXT(0x03)          /* Ctrl+2 / Ctrl+@ */
#define K_ESC    0x1B

int key_get(void);                      /* a key, or 0 if none waiting */
unsigned ticks(void);                   /* BIOS clock, 18.2 per second */
#define TICKS_PER_SEC 18

/* ------------------------------------------------------------------ */
/* term.c */

void term_reset(int utf8);
void term_clear(void);
void term_mode(int utf8);
void term_write(const unsigned char *s, unsigned n);
void term_cursor(void);
extern int term_appcursor;               /* DECCKM: ESC O A cursor keys */

/* Provided by the program: send a reply (status reports) to the remote */
void term_reply(const char *s);

/* cp437.c */
int cp437_from_ucs(unsigned long ucs);   /* a CP437 byte, or -1 */

/* ------------------------------------------------------------------ */
/* menu.c */

enum { MENU_START, MENU_RESUME, MENU_EXIT };
int menu_run(struct config *c, int in_session, int state, const char *msg);

/* ------------------------------------------------------------------ */
/* zmcrc.c */

#define ZPAD   '*'
#define ZDLE   0x18
#define ZBIN   'A'
#define ZHEX   'B'
#define ZBIN32 'C'

enum { ZRQINIT, ZRINIT, ZSINIT, ZACK, ZFILE, ZSKIP, ZNAK, ZABORT, ZFIN,
       ZRPOS, ZDATA, ZEOF, ZFERR, ZCRC, ZCHALLENGE, ZCOMPL, ZCAN,
       ZFREECNT, ZCOMMAND, ZSTDERR };

#define ZCRCE 'h'
#define ZCRCG 'i'
#define ZCRCQ 'j'
#define ZCRCW 'k'
#define ZRUB0 'l'
#define ZRUB1 'm'

#define CANFDX  0x01
#define CANOVIO 0x02
#define CANFC32 0x20

void zm_crc_init(void);
unsigned zm_crc16(unsigned crc, unsigned char c);
unsigned long zm_crc32(unsigned long crc, unsigned char c);
#define ZM_CRC32_GOOD 0xDEBB20E3UL
int  zm_hexheader(char *out, int type, unsigned long arg);

/* zmodem.c */
int  zm_detect(int c);                  /* feed a byte; see zmodem.c */
int  zm_pending(unsigned char *out);     /* bytes held back by zm_detect */
void zm_receive(const struct config *c);
extern unsigned zm_sessions;            /* finished, for the tests */

/* ------------------------------------------------------------------ */
/* dlname.c */

/* Make a safe 8.3 name (at most 12 characters, upper case) for the
 * sender's name; returns 0, always producing a usable name. */
void dl_safename(const char *sender, char *base, char *ext);
/* Find a name that does not exist yet: base.ext, base~1.ext, ...
 * exists(path) says whether a file of that name is there.  Returns 0
 * with the name (without directory) in out, or -1. */
int dl_unique(const char *dir, const char *base, const char *ext,
              int (*exists)(const char *path), char *out);
void dl_join(char *path, const char *dir, const char *name);

/* dosfile.c */
int  df_exists(const char *path);
int  df_isdir(const char *path);
int  df_rename(const char *from, const char *to);
int  df_unlink(const char *path);
int  df_fullpath(char *out, const char *path);

#endif
