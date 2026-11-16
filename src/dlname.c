/*
 * dlname.c - DOS names for received files
 *
 * The sender's name is reduced to its last path component (so
 * "../../AUTOEXEC.BAT", "/etc/passwd" and "C:\X\Y" cannot leave the
 * download directory), then to a valid upper-case 8.3 name; DOS device
 * names get a leading underscore.  dl_unique never picks a name that
 * exists, so nothing is overwritten.
 *
 * Portable C: also built on the host for tests/unit.
 */

#include <string.h>
#include "term16.h"

static int valid(unsigned char c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c >= 0x80)
        return 1;
    return c != 0 && strchr("!#$%&'()-@^_`{}~", c) != NULL;
}

static unsigned char upper(unsigned char c)
{
    return c >= 'a' && c <= 'z' ? (unsigned char)(c - 'a' + 'A') : c;
}

/* Copy at most max valid characters of s[0..n) to out, upper-cased. */
static void clean(const char *s, int n, char *out, int max)
{
    int k = 0;

    while (n-- > 0 && k < max) {
        unsigned char c = upper((unsigned char)*s++);

        if (c == ' ')
            continue;
        out[k++] = valid(c) ? (char)c : '_';
    }
    out[k] = '\0';
}

void dl_safename(const char *sender, char *base, char *ext)
{
    static const char *const devices[] = {
        "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4",
        "LPT1", "LPT2", "LPT3", "CLOCK$", "CONFIG$"
    };
    const char *name = sender, *p, *dot;
    int i, n;

    for (p = sender; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':')
            name = p + 1;
    while (*name == '.')
        name++;                         /* ".", "..", ".profile" */
    dot = strrchr(name, '.');
    n = dot != NULL ? (int)(dot - name) : (int)strlen(name);
    clean(name, n, base, 8);
    if (dot != NULL)
        clean(dot + 1, (int)strlen(dot + 1), ext, 3);
    else
        ext[0] = '\0';
    if (base[0] == '\0')
        strcpy(base, "FILE");
    for (i = 0; i < (int)(sizeof devices / sizeof devices[0]); i++)
        if (strcmp(base, devices[i]) == 0) {
            memmove(base + 1, base, strlen(base) + 1);
            base[0] = '_';
            base[8] = '\0';
            break;
        }
}

void dl_join(char *path, const char *dir, const char *name)
{
    size_t n = strlen(dir);

    strcpy(path, dir);
    if (n > 0 && dir[n - 1] != '\\' && dir[n - 1] != ':')
        path[n++] = '\\';
    strcpy(path + n, name);
}

static void make(char *out, const char *base, int keep, unsigned k,
                 const char *ext)
{
    char num[6];
    int i = 0, j;

    memcpy(out, base, (size_t)keep);
    j = keep;
    if (k != 0) {
        do {
            num[i++] = (char)('0' + k % 10);
            k /= 10;
        } while (k);
        out[j++] = '~';
        while (i)
            out[j++] = num[--i];
    }
    if (ext[0] != '\0') {
        out[j++] = '.';
        strcpy(out + j, ext);
    } else
        out[j] = '\0';
}

int dl_unique(const char *dir, const char *base, const char *ext,
              int (*exists)(const char *path), char *out)
{
    char path[DIR_MAX + 14];
    unsigned k;
    int len = (int)strlen(base);

    for (k = 0; k < 1000; k++) {
        int digits = k == 0 ? 0 : k < 10 ? 1 : k < 100 ? 2 : 3;
        int keep = k == 0 ? len : 8 - 1 - digits;

        if (keep > len)
            keep = len;
        make(out, base, keep, k, ext);
        dl_join(path, dir, out);
        if (!exists(path))
            return 0;
    }
    return -1;
}
