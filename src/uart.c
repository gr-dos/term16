/*
 * uart.c - interrupt-driven receive on an 8250/16450/16550 UART
 *
 * The IRQ handler moves received bytes into a ring buffer and does
 * receive flow control: above RX_HIGH bytes it drops RTS (RTS/CTS) or
 * sends XOFF (XON/XOFF), and uart_getc undoes that below RX_LOW.
 * Sending is polled.  Everything the open changes (line settings,
 * divisor, IER, MCR, FIFO, PIC mask, vector) is put back by
 * uart_close, which main.c calls on every exit path.
 */

#include <dos.h>
#include <stddef.h>
#include "term16.h"

#define RX_MASK (RX_SIZE - 1)
#define RX_HIGH (RX_SIZE * 3 / 4)
#define RX_LOW  (RX_SIZE / 4)

#define RBR 0
#define THR 0
#define DLL 0
#define IER 1
#define DLM 1
#define IIR 2
#define FCR 2
#define LCR 3
#define MCR 4
#define LSR 5
#define MSR 6

#define PIC_CMD  0x20
#define PIC_MASK 0x21
#define XON  0x11
#define XOFF 0x13

unsigned char inb(unsigned port);
#pragma aux inb = "in al,dx" parm [dx] value [al] modify exact [al];
void outb(unsigned port, unsigned char v);
#pragma aux outb = "out dx,al" parm [dx] [al] modify exact [];
void cli(void);
#pragma aux cli = "cli" modify exact [];
void sti(void);
#pragma aux sti = "sti" modify exact [];

/* For uart_probe, which takes the port access as functions */
static unsigned char port_in(unsigned port)
{
    return inb(port);
}

static void port_out(unsigned port, unsigned char v)
{
    outb(port, v);
}

static const unsigned default_base[4] = { 0x3F8, 0x2F8, 0x3E8, 0x2E8 };

static unsigned char rxbuf[RX_SIZE];
static volatile unsigned rx_head;       /* written by the handler */
static unsigned rx_tail;

static unsigned base;
static unsigned char irq;
static unsigned char is_open;
static unsigned char flow;
static volatile unsigned char throttled; /* we asked the sender to stop */
static volatile unsigned char xoff_due;  /* XOFF to send when THR frees */
static volatile unsigned char tx_held;   /* the remote sent XOFF */

unsigned long uart_overruns;
volatile unsigned uart_received;         /* bytes, wrapping */
const char *uart_error;

/* State to restore */
static unsigned char old_lcr, old_mcr, old_ier, old_dll, old_dlm;
static unsigned char old_pic, old_fifo;
static void (__interrupt __far *old_vect)();

static void __interrupt __far uart_isr(void)
{
    unsigned char iir, lsr, c;
    unsigned h, n;

    while (!((iir = inb(base + IIR)) & 1)) {
        switch (iir & 6) {
        case 4:                         /* data (or FIFO timeout) */
            while ((lsr = inb(base + LSR)) & 1) {
                c = inb(base + RBR);
                if (lsr & 2)
                    uart_overruns++;
                if (flow == FLOW_XONXOFF && (c & 0x7F) == XOFF) {
                    tx_held = 1;
                    continue;
                }
                if (flow == FLOW_XONXOFF && (c & 0x7F) == XON) {
                    tx_held = 0;
                    continue;
                }
                h = rx_head;
                n = (h + 1) & RX_MASK;
                if (n == rx_tail) {
                    uart_overruns++;
                    continue;
                }
                rxbuf[h] = c;
                rx_head = n;
                uart_received++;
            }
            break;
        case 6:
            inb(base + LSR);
            break;
        case 0:
            inb(base + MSR);
            break;
        default:                        /* THR empty: not enabled */
            break;
        }
    }

    if (!throttled && flow != FLOW_NONE &&
        ((rx_head - rx_tail) & RX_MASK) >= RX_HIGH) {
        throttled = 1;
        if (flow == FLOW_RTSCTS)
            outb(base + MCR, 0x09);     /* DTR, OUT2; RTS off */
        else if (inb(base + LSR) & 0x20)
            outb(base + THR, XOFF);
        else
            xoff_due = 1;
    }
    outb(PIC_CMD, 0x20);
}

#define BIOS_WORD(off) (*(unsigned short __far *)MK_FP(0x40, (off)))

int uart_open(const struct config *c)
{
    unsigned div;
    unsigned char lcr;

    uart_error = NULL;
    if (is_open)
        uart_close();
    base = BIOS_WORD((c->port - 1) * 2);
    if (base == 0)
        base = default_base[c->port - 1];
    irq = (c->port & 1) ? 4 : 3;

    if (uart_probe(base, port_in, port_out, &old_lcr) < 0) {
        uart_error = "no serial port at that address";
        return -1;
    }

    old_mcr = inb(base + MCR);
    old_ier = inb(base + IER);
    outb(base + LCR, 0x80);
    old_dll = inb(base + DLL);
    old_dlm = inb(base + DLM);
    old_fifo = inb(base + IIR) & 0xC0;
    old_pic = inb(PIC_MASK);
    old_vect = _dos_getvect(8 + irq);

    outb(base + IER, 0);
    div = (unsigned)(115200UL / c->baud);
    outb(base + LCR, 0x80);
    outb(base + DLL, (unsigned char)div);
    outb(base + DLM, (unsigned char)(div >> 8));
    lcr = (unsigned char)(c->data - 5);
    if (c->stop == 2)
        lcr |= 0x04;
    switch (c->parity) {
    case PAR_ODD:   lcr |= 0x08; break;
    case PAR_EVEN:  lcr |= 0x18; break;
    case PAR_MARK:  lcr |= 0x28; break;
    case PAR_SPACE: lcr |= 0x38; break;
    }
    outb(base + LCR, lcr);

    /* A 16550A FIFO, interrupting at 8 bytes; not on the buggy 16550 */
    outb(base + FCR, 0x87);
    if ((inb(base + IIR) & 0xC0) != 0xC0)
        outb(base + FCR, 0);

    flow = (unsigned char)c->flow;
    rx_head = rx_tail = 0;
    throttled = xoff_due = tx_held = 0;
    uart_overruns = 0;

    inb(base + LSR);
    inb(base + RBR);
    inb(base + IIR);
    inb(base + MSR);

    _dos_setvect(8 + irq, uart_isr);
    outb(base + MCR, 0x0B);             /* DTR, RTS, OUT2 */
    outb(base + IER, 0x01);             /* received data */
    cli();
    outb(PIC_MASK, inb(PIC_MASK) & ~(1 << irq));
    sti();
    is_open = 1;

    /* An LSR of FFh is a missing or disabled port. */
    if (inb(base + LSR) == 0xFF) {
        uart_close();
        uart_error = "serial port not responding";
        return -1;
    }
    return 0;
}

void uart_close(void)
{
    if (!is_open)
        return;
    is_open = 0;
    outb(base + IER, 0);
    cli();
    outb(PIC_MASK, (inb(PIC_MASK) & ~(1 << irq)) | (old_pic & (1 << irq)));
    sti();
    _dos_setvect(8 + irq, old_vect);
    outb(base + FCR, old_fifo ? 0x07 : 0);
    outb(base + LCR, 0x80);
    outb(base + DLL, old_dll);
    outb(base + DLM, old_dlm);
    outb(base + LCR, old_lcr);
    outb(base + MCR, old_mcr);
    outb(base + IER, old_ier);
}

unsigned uart_count(void)
{
    return (rx_head - rx_tail) & RX_MASK;
}

int uart_getc(void)
{
    int c;

    if (rx_tail == rx_head)
        return -1;
    c = rxbuf[rx_tail];
    rx_tail = (rx_tail + 1) & RX_MASK;
    if (throttled && ((rx_head - rx_tail) & RX_MASK) < RX_LOW) {
        cli();
        throttled = 0;
        xoff_due = 0;
        sti();
        if (flow == FLOW_RTSCTS)
            outb(base + MCR, 0x0B);
        else
            uart_putc(XON);
    }
    return c;
}

/* Send a pending XOFF once the transmitter is free. */
void uart_poll(void)
{
    if (xoff_due && (inb(base + LSR) & 0x20)) {
        xoff_due = 0;
        outb(base + THR, XOFF);
    }
}

/* Send one byte, waiting up to about 2 seconds for the transmitter,
 * CTS (RTS/CTS) or XON (XON/XOFF). */
int uart_putc(int c)
{
    unsigned start;

    if (!is_open)
        return -1;
    start = ticks();
    for (;;) {
        unsigned char lsr = inb(base + LSR);

        uart_poll();
        if ((lsr & 0x20) &&
            (flow != FLOW_RTSCTS || (inb(base + MSR) & 0x10)) &&
            (flow != FLOW_XONXOFF || !tx_held))
            break;
        if ((unsigned)(ticks() - start) > 2 * TICKS_PER_SEC)
            return -1;
    }
    outb(base + THR, (unsigned char)c);
    return 0;
}

void uart_write(const char *s, unsigned n)
{
    while (n-- > 0)
        if (uart_putc((unsigned char)*s++) < 0)
            break;
}

/* Wait (up to about a second) until the last byte has left the UART. */
void uart_drain(void)
{
    unsigned start = ticks();

    while (is_open && (inb(base + LSR) & 0x40) == 0 &&
           (unsigned)(ticks() - start) < TICKS_PER_SEC)
        ;
}

