#include "keyboard.h"
#include <arch/x86_64/portio.h>
#include "kernel.h"

#define KEYB_DATA 0x60
#define KEYB_STAT 0x64

#define RING_SIZE 256

static volatile char ring[RING_SIZE];
static volatile int ring_r, ring_w;
static int shift;

static const char scancode_asc[128] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' ', 0,
};

static const char scancode_shift[128] = {
    0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' ', 0,
};

static inline int ring_full(void) {
    return ((ring_w + 1) % RING_SIZE) == ring_r;
}

static inline int ring_empty(void) {
    return ring_r == ring_w;
}

char keyboard_getc(void) {
    while (ring_empty())
        __asm__("pause");
    int i = ring_r;
    ring_r = (ring_r + 1) % RING_SIZE;
    return ring[i];
}

void keyboard_irq(void) {
    uint8_t sc = inb(KEYB_DATA);

    if (sc == 0x2A || sc == 0x36) {
        shift = 1;
        return;
    }
    if (sc == 0xAA || sc == 0xB6) {
        shift = 0;
        return;
    }

    if (sc & 0x80)
        return;

    if (sc >= 128)
        return;

    char c = shift ? scancode_shift[sc] : scancode_asc[sc];
    if (!c)
        return;

    if (c == '\b') {
        serial_puts("\b \b");
    } else {
        serial_putc(c);
    }

    if (!ring_full()) {
        ring[ring_w] = c;
        ring_w = (ring_w + 1) % RING_SIZE;
    }
}

void keyboard_init(void) {
    ring_r = ring_w = 0;
    shift = 0;
}
