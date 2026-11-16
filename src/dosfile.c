/*
 * dosfile.c - files and directories for downloads
 *
 * In the test build (-DTERM16_TEST) df_rename fails when %T16NOREN% is
 * set, so tests/run.py can check what a failed final rename does.
 */

#include <dos.h>
#include <stdlib.h>
#include <string.h>
#include "term16.h"

int df_exists(const char *path)
{
    unsigned attr;

    return _dos_getfileattr(path, &attr) == 0;
}

/* Whether path (a full path from df_fullpath) is a directory.  DOS
 * gives no attributes for a root directory, so "X:\" is looked for as
 * a drive instead. */
int df_isdir(const char *path)
{
    unsigned attr;
    struct find_t f;
    char pat[8];

    if (path[0] != '\0' && path[1] == ':' && path[2] == '\\' &&
        path[3] == '\0') {
        pat[0] = path[0];
        strcpy(pat + 1, ":\\*.*");
        /* 0 found, 18 no files: either way the drive is there */
        attr = _dos_findfirst(pat, _A_SUBDIR | _A_HIDDEN | _A_SYSTEM, &f);
        return attr == 0 || attr == 18;
    }
    return _dos_getfileattr(path, &attr) == 0 && (attr & _A_SUBDIR);
}

int df_rename(const char *from, const char *to)
{
    union REGS r;
    struct SREGS s;

#ifdef TERM16_TEST
    if (getenv("T16NOREN") != NULL)
        return -1;
#endif
    segread(&s);
    s.es = s.ds;
    r.h.ah = 0x56;
    r.x.dx = (unsigned)from;
    r.x.di = (unsigned)to;
    intdosx(&r, &r, &s);
    return r.x.cflag ? -1 : 0;
}

int df_unlink(const char *path)
{
    union REGS r;

    r.h.ah = 0x41;
    r.x.dx = (unsigned)path;
    intdos(&r, &r);
    return r.x.cflag ? -1 : 0;
}

/* The full path of a directory, without a trailing backslash unless it
 * is a root; 0, or -1 if too long or invalid. */
int df_fullpath(char *out, const char *path)
{
    char buf[_MAX_PATH];
    size_t n;

    if (_fullpath(buf, path, sizeof buf) == NULL)
        return -1;
    n = strlen(buf);
    if (n > 3 && buf[n - 1] == '\\')
        buf[--n] = '\0';
    if (n >= DIR_MAX)
        return -1;
    strcpy(out, buf);
    return 0;
}
