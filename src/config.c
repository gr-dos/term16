/*
 * config.c - the session settings, their names, and the status bar
 *
 * Portable C: also built on the host for tests/unit.
 */

#include <string.h>
#include "term16.h"

const unsigned long cfg_bauds[] = {
    110, 150, 300, 600, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200
};
const int cfg_nbauds = sizeof cfg_bauds / sizeof cfg_bauds[0];

static const char *const parity_names[] = {
    "none", "odd", "even", "mark", "space"
};
static const char *const flow_names[] = { "none", "rtscts", "xonxoff" };
static const char *const flow_shown[] = { "None", "RTS/CTS", "XON/XOFF" };
static const char *const term_names[] = { "ansi", "utf8" };
static const char *const term_shown[] = { "ANSI", "UTF8" };

void cfg_default(struct config *c)
{
    c->port = 1;
    c->baud = 9600;
    c->data = 8;
    c->parity = PAR_NONE;
    c->stop = 1;
    c->flow = FLOW_NONE;
    c->term = TERM_ANSI;
    c->dldir[0] = '\0';
}

/* Whether a and b are equal ignoring ASCII case */
static int eqi(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = *a, y = *b;

        if (x >= 'A' && x <= 'Z')
            x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z')
            y += 'a' - 'A';
        if (x != y)
            return 0;
        if (x == '\0')
            return 1;
    }
}

/* Decimal digits only, no sign; -1 if not a number or too large. */
static long parse_num(const char *s)
{
    unsigned long n = 0;

    if (*s == '\0')
        return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || n > 1000000UL)
            return -1;
        n = n * 10 + (*s - '0');
    }
    return (long)n;
}

/* Index of s in names (case-insensitive, or a unique prefix of at
 * least one letter); -1 if none. */
static int lookup(const char *const *names, int count, const char *s)
{
    int i, found = -1;
    size_t n = strlen(s);

    for (i = 0; i < count; i++) {
        const char *a = names[i];
        size_t k;

        for (k = 0; k < n && a[k]; k++) {
            char x = s[k];

            if (x >= 'A' && x <= 'Z')
                x += 'a' - 'A';
            if (x != a[k])
                break;
        }
        if (k == n && n > 0) {
            if (a[k] == '\0')
                return i;               /* exact */
            if (found >= 0)
                return -1;              /* ambiguous */
            found = i;
        }
    }
    return found;
}

/* Set one field from its command-line spelling; 0, or -1 if invalid.
 * F_DIR is only copied (main.c checks the directory). */
int cfg_parse(struct config *c, int field, const char *v)
{
    long n;
    int i;

    switch (field) {
    case F_PORT:
        if ((v[0] == 'C' || v[0] == 'c') && (v[1] == 'O' || v[1] == 'o') &&
            (v[2] == 'M' || v[2] == 'm'))
            v += 3;
        n = parse_num(v);
        if (n < 1 || n > 4)
            return -1;
        c->port = (int)n;
        return 0;
    case F_BAUD:
        n = parse_num(v);
        for (i = 0; i < cfg_nbauds; i++)
            if ((unsigned long)n == cfg_bauds[i]) {
                c->baud = cfg_bauds[i];
                return 0;
            }
        return -1;
    case F_DATA:
        n = parse_num(v);
        if (n < 5 || n > 8)
            return -1;
        c->data = (int)n;
        return 0;
    case F_PARITY:
        i = lookup(parity_names, 5, v);
        if (i < 0 && v[0] != '\0' && v[1] == '\0') {
            /* single letters N O E M S */
            static const char letters[] = "noems";
            char x = v[0] | 0x20;
            const char *p = strchr(letters, x);

            if (p != NULL)
                i = (int)(p - letters);
        }
        if (i < 0)
            return -1;
        c->parity = i;
        return 0;
    case F_STOP:
        n = parse_num(v);
        if (n != 1 && n != 2)
            return -1;
        c->stop = (int)n;
        return 0;
    case F_FLOW:
        if (eqi(v, "rts/cts") || eqi(v, "hardware") || eqi(v, "hw"))
            i = FLOW_RTSCTS;
        else if (eqi(v, "xon/xoff") || eqi(v, "software") ||
                 eqi(v, "sw"))
            i = FLOW_XONXOFF;
        else
            i = lookup(flow_names, 3, v);
        if (i < 0)
            return -1;
        c->flow = i;
        return 0;
    case F_TERM:
        if (eqi(v, "utf-8"))
            i = TERM_UTF8;
        else
            i = lookup(term_names, 2, v);
        if (i < 0)
            return -1;
        c->term = i;
        return 0;
    case F_DIR:
        if (strlen(v) >= DIR_MAX || v[0] == '\0')
            return -1;
        strcpy(c->dldir, v);
        return 0;
    }
    return -1;
}

static char *put_ul(char *p, unsigned long n)
{
    char tmp[12];
    int i = 0;

    do {
        tmp[i++] = (char)('0' + n % 10);
        n /= 10;
    } while (n != 0);
    while (i > 0)
        *p++ = tmp[--i];
    *p = '\0';
    return p;
}

/* The value of a field as the menu shows it, in buf (at least 16
 * bytes); F_DIR returns the directory itself. */
const char *cfg_name(const struct config *c, int field, char *buf)
{
    static const char *const parity_shown[] = {
        "None", "Odd", "Even", "Mark", "Space"
    };

    switch (field) {
    case F_PORT:
        strcpy(buf, "COM");
        buf[3] = (char)('0' + c->port);
        buf[4] = '\0';
        return buf;
    case F_BAUD:
        put_ul(buf, c->baud);
        return buf;
    case F_DATA:
        put_ul(buf, (unsigned long)c->data);
        return buf;
    case F_PARITY:
        return parity_shown[c->parity];
    case F_STOP:
        put_ul(buf, (unsigned long)c->stop);
        return buf;
    case F_FLOW:
        return flow_shown[c->flow];
    case F_TERM:
        return c->term == TERM_UTF8 ? "UTF-8 (ANSI, UTF-8 to CP437)"
                                    : "ANSI (CP437)";
    case F_DIR:
        return c->dldir;
    }
    return "";
}

/* Move a field to its next (dir > 0) or previous value, wrapping. */
void cfg_step(struct config *c, int field, int dir)
{
    int i;

    switch (field) {
    case F_PORT:
        c->port = (c->port - 1 + 4 + dir) % 4 + 1;
        break;
    case F_BAUD:
        for (i = 0; i < cfg_nbauds && cfg_bauds[i] != c->baud; i++)
            ;
        i = (i + cfg_nbauds + dir) % cfg_nbauds;
        c->baud = cfg_bauds[i];
        break;
    case F_DATA:
        c->data = (c->data - 5 + 4 + dir) % 4 + 5;
        break;
    case F_PARITY:
        c->parity = (c->parity + 5 + dir) % 5;
        break;
    case F_STOP:
        c->stop = c->stop == 1 ? 2 : 1;
        break;
    case F_FLOW:
        c->flow = (c->flow + 3 + dir) % 3;
        break;
    case F_TERM:
        c->term = !c->term;
        break;
    }
}

/* Whether two configurations set the serial line the same way. */
int cfg_same_line(const struct config *a, const struct config *b)
{
    return a->port == b->port && a->baud == b->baud && a->data == b->data &&
           a->parity == b->parity && a->stop == b->stop &&
           a->flow == b->flow;
}

static char *put_str(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    return p;
}

/* The status bar: exactly SCR_COLS characters and a terminator.  It
 * shows what reproduces the session, never the download directory. */
void cfg_status(const struct config *c, int state, char *line)
{
    static const char *const states[] = { "CLOSED", "OPEN", "ERROR" };
    static const char hints[] = "Alt-M Menu  Alt-X Exit ";
    char *p = line;
    int n;

    *p++ = ' ';
    p = put_str(p, "COM");
    *p++ = (char)('0' + c->port);
    p = put_str(p, "  ");
    p = put_ul(p, c->baud);
    p = put_str(p, "  ");
    *p++ = (char)('0' + c->data);
    *p++ = "NOEMS"[c->parity];
    *p++ = (char)('0' + c->stop);
    p = put_str(p, "  Flow:");
    p = put_str(p, flow_shown[c->flow]);
    p = put_str(p, "  ");
    p = put_str(p, term_shown[c->term]);
    p = put_str(p, "  ");
    p = put_str(p, states[state]);
    n = (int)(p - line);
    while (n < SCR_COLS - (int)(sizeof hints - 1)) {
        line[n++] = ' ';
    }
    if (n == SCR_COLS - (int)(sizeof hints - 1))
        strcpy(line + n, hints);
    else {
        while (n < SCR_COLS)
            line[n++] = ' ';
        line[n] = '\0';
    }
}
