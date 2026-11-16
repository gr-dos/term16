/*
 * zmcrc.c - ZMODEM CRCs and hex headers
 *
 * CRC-16 is CCITT (polynomial 1021h, initial 0, as XMODEM); CRC-32 is
 * the reflected EDB88320h with initial FFFFFFFFh, whose residue over
 * data and its (complemented, little-endian) CRC is DEBB20E3h.  The
 * tables are built at startup in BSS to keep the executable small.
 *
 * Portable C: also built on the host for tests/unit.
 */

#include "term16.h"

static unsigned short crc16_tab[256];
static unsigned long crc32_tab[256];

void zm_crc_init(void)
{
    unsigned i, k;

    for (i = 0; i < 256; i++) {
        unsigned short c16 = (unsigned short)(i << 8);
        unsigned long c32 = i;

        for (k = 0; k < 8; k++) {
            c16 = (unsigned short)(c16 & 0x8000 ? (c16 << 1) ^ 0x1021
                                                : c16 << 1);
            c32 = c32 & 1 ? (c32 >> 1) ^ 0xEDB88320UL : c32 >> 1;
        }
        crc16_tab[i] = c16;
        crc32_tab[i] = c32;
    }
}

unsigned zm_crc16(unsigned crc, unsigned char c)
{
    return ((crc << 8) ^ crc16_tab[((crc >> 8) ^ c) & 0xFF]) & 0xFFFF;
}

unsigned long zm_crc32(unsigned long crc, unsigned char c)
{
    return crc32_tab[(unsigned char)(crc ^ c)] ^ (crc >> 8);
}

static char *hex2(char *p, unsigned char b)
{
    static const char digits[] = "0123456789abcdef";

    *p++ = digits[b >> 4];
    *p++ = digits[b & 15];
    return p;
}

/* A hex header of type with the 32-bit argument arg (ZP0 is its low
 * byte, ZF0 its high byte), into out (at least 22 bytes); returns its
 * length. */
int zm_hexheader(char *out, int type, unsigned long arg)
{
    char *p = out;
    unsigned crc;
    unsigned char b[5];
    int i;

    b[0] = (unsigned char)type;
    for (i = 0; i < 4; i++)
        b[1 + i] = (unsigned char)(arg >> (8 * i));
    crc = 0;
    for (i = 0; i < 5; i++)
        crc = zm_crc16(crc, b[i]);
    *p++ = ZPAD;
    *p++ = ZPAD;
    *p++ = ZDLE;
    *p++ = ZHEX;
    for (i = 0; i < 5; i++)
        p = hex2(p, b[i]);
    p = hex2(p, (unsigned char)(crc >> 8));
    p = hex2(p, (unsigned char)crc);
    *p++ = '\r';
    *p++ = (char)0x8A;                  /* LF with the high bit, as rz */
    if (type != ZACK && type != ZFIN)
        *p++ = 0x11;                    /* XON */
    return (int)(p - out);
}
