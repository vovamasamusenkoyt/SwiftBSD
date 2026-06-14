#include "pit.h"
#include <arch/x86_64/portio.h>

#define PIT_CH0  0x40
#define PIT_CMD  0x43

#define PIT_SETCH0 0x36

void pit_init(int hz) {
    int divisor = 1193182 / hz;
    outb(PIT_CMD, PIT_SETCH0);
    outb(PIT_CH0, divisor & 0xFF);
    outb(PIT_CH0, (divisor >> 8) & 0xFF);
}
