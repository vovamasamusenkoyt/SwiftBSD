#include "kernel.h"
#include <arch/x86_64/portio.h>

#define COM1 0x3F8

static int serial_transmit_empty(void) {
    return inb(COM1 + 5) & 0x20;
}

void serial_init(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

void serial_putc(char c) {
    while (!serial_transmit_empty());
    outb(COM1, c);
}

void serial_puts(const char *s) {
    while (*s) {
        if (*s == '\n')
            serial_putc('\r');
        serial_putc(*s++);
    }
}

void serial_vprintf(const char *fmt, __builtin_va_list args) {
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            serial_putc(*p);
            continue;
        }
        switch (*++p) {
        case 's': {
            const char *s = __builtin_va_arg(args, const char *);
            serial_puts(s);
            break;
        }
        case 'x':
        case 'X': {
            unsigned int n = __builtin_va_arg(args, unsigned int);
            char buf[9];
            for (int i = 7; i >= 0; i--) {
                unsigned int d = n & 0xF;
                buf[i] = d < 10 ? '0' + d : 'A' + d - 10;
                n >>= 4;
            }
            buf[8] = 0;
            serial_puts(buf);
            break;
        }
        case 'p':
        case 'P': {
            uint64_t n = __builtin_va_arg(args, uint64_t);
            char buf[17];
            for (int i = 15; i >= 0; i--) {
                unsigned int d = n & 0xF;
                buf[i] = d < 10 ? '0' + d : 'A' + d - 10;
                n >>= 4;
            }
            buf[16] = 0;
            serial_puts(buf);
            break;
        }
        case 'd':
        case 'i': {
            int n = __builtin_va_arg(args, int);
            char buf[12];
            int pos = 11;
            buf[11] = 0;
            int neg = 0;
            if (n < 0) { neg = 1; n = -n; }
            if (n == 0) buf[--pos] = '0';
            while (n > 0) { buf[--pos] = '0' + (n % 10); n /= 10; }
            if (neg) buf[--pos] = '-';
            serial_puts(&buf[pos]);
            break;
        }
        case 'u': {
            unsigned int n = __builtin_va_arg(args, unsigned int);
            char buf[12];
            int pos = 11;
            buf[11] = 0;
            if (n == 0) buf[--pos] = '0';
            while (n > 0) { buf[--pos] = '0' + (n % 10); n /= 10; }
            serial_puts(&buf[pos]);
            break;
        }
        case 'c': {
            int c = __builtin_va_arg(args, int);
            serial_putc(c);
            break;
        }
        default:
            serial_putc('%');
            serial_putc(*p);
            break;
        }
    }
}

void serial_printf(const char *fmt, ...) {
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    serial_vprintf(fmt, args);
    __builtin_va_end(args);
}

int serial_haschar(void) {
    return inb(COM1 + 5) & 1;
}

char serial_getc(void) {
    while (!serial_haschar());
    return inb(COM1);
}
