/*
 * COM1 (3F8h, IRQ 4): an 8250 UART with a Microsoft-compatible two-button serial mouse
 * plugged in. The stock DOS mouse drivers (e.g. FreeDOS CTMOUSE) find it by themselves and
 * do all cursor handling, so no INT 33h code is needed in the BIOS.
 */
#include <string.h>

#include "pc.h"

int mouse_present;

static struct {
    uint8_t ier, lcr, mcr, scr, dll, dlm;
    uint8_t rbr, dr;      /* receive register and "data ready" */
    uint8_t thre_pending; /* transmitter-empty interrupt not yet acknowledged */
    uint8_t fifo[64];
    unsigned head, tail;
} u;

static struct {
    int dx, dy;
    uint8_t buttons, sent_buttons;
} m;

static void rx_push(uint8_t b)
{
    unsigned n = (u.head + 1) % sizeof(u.fifo);
    if (n != u.tail) {
        u.fifo[u.head] = b;
        u.head = n;
    }
}

static unsigned rx_free(void)
{
    return (u.tail + sizeof(u.fifo) - u.head - 1) % sizeof(u.fifo);
}

static void irq_update(void)
{
    if ((u.mcr & 0x08) && (((u.ier & 1) && u.dr) || ((u.ier & 2) && u.thre_pending))) {
        pic_raise(4);
    }
}

void uart_init(void)
{
    memset(&u, 0, sizeof(u));
    u.dll = 0x60; /* 1200 baud */
    m.dx = m.dy = 0;
    m.sent_buttons = m.buttons;
}

void mouse_event(int dx, int dy, int buttons)
{
    hal_input_lock();
    m.dx += dx;
    m.dy += dy;
    m.buttons = (uint8_t)(buttons & 3);
    hal_input_unlock();
}

void uart_poll(void)
{
    if (mouse_present && rx_free() >= 3) {
        int dx, dy;
        uint8_t b;
        hal_input_lock();
        dx = m.dx < -127 ? -127 : m.dx > 127 ? 127 : m.dx;
        dy = m.dy < -127 ? -127 : m.dy > 127 ? 127 : m.dy;
        b = m.buttons;
        if (dx || dy || b != m.sent_buttons) {
            m.dx -= dx;
            m.dy -= dy;
            m.sent_buttons = b;
            hal_input_unlock();
            rx_push((uint8_t)(0x40 | ((b & 1) << 5) | ((b & 2) << 3) | (((dy >> 6) & 3) << 2) |
                              ((dx >> 6) & 3)));
            rx_push((uint8_t)(dx & 0x3F));
            rx_push((uint8_t)(dy & 0x3F));
        } else {
            hal_input_unlock();
        }
    }
    if (!u.dr && u.tail != u.head) {
        u.rbr = u.fifo[u.tail];
        u.tail = (u.tail + 1) % sizeof(u.fifo);
        u.dr = 1;
        irq_update();
    }
}

uint8_t uart_inb(uint16_t port)
{
    switch (port & 7) {
    case 0:
        if (u.lcr & 0x80) {
            return u.dll;
        }
        u.dr = 0;
        return u.rbr;
    case 1: return (u.lcr & 0x80) ? u.dlm : u.ier;
    case 2:
        if ((u.ier & 1) && u.dr) {
            return 0x04;
        }
        if ((u.ier & 2) && u.thre_pending) {
            u.thre_pending = 0;
            return 0x02;
        }
        return 0x01;
    case 3: return u.lcr;
    case 4: return u.mcr;
    case 5: return (uint8_t)(0x60 | u.dr);
    case 6: return 0x00;
    default: return u.scr;
    }
}

void uart_outb(uint16_t port, uint8_t v)
{
    switch (port & 7) {
    case 0:
        if (u.lcr & 0x80) {
            u.dll = v;
        } else {
            u.thre_pending = 1; /* the byte is "sent" at once */
            irq_update();
        }
        break;
    case 1:
        if (u.lcr & 0x80) {
            u.dlm = v;
        } else {
            if ((v & 2) && !(u.ier & 2)) {
                u.thre_pending = 1;
            }
            u.ier = v & 0x0F;
            irq_update();
        }
        break;
    case 3: u.lcr = v; break;
    case 4:
        /* Raising DTR/RTS powers the mouse up; it answers with its ID byte 'M'. */
        if (mouse_present && (v & 3) == 3 && (u.mcr & 3) != 3) {
            u.head = u.tail = 0;
            u.dr = 0;
            rx_push('M');
        }
        u.mcr = v & 0x1F;
        irq_update();
        break;
    case 7: u.scr = v; break;
    default: break;
    }
}
