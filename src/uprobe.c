/*
 * uprobe.c - is there a UART at an address?
 *
 * Portable C, with the port reads and writes passed in, so that
 * tests/unit can check it against simulated registers.
 */

#include "term16.h"

#define LCR 3

/* A UART's LCR reads back what was written; open bus or another device
 * does not.  On success *old_lcr is the LCR found and the LCR is left
 * at 8N1; otherwise the LCR is put back as it was. */
int uart_probe(unsigned base, port_in_fn in, port_out_fn out,
               unsigned char *old_lcr)
{
    *old_lcr = in(base + LCR);
    out(base + LCR, 0x1B);
    if (in(base + LCR) == 0x1B) {
        out(base + LCR, 0x03);
        if (in(base + LCR) == 0x03)
            return 0;
    }
    out(base + LCR, *old_lcr);
    return -1;
}
