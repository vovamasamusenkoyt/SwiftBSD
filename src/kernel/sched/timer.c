#include "kernel.h"

static volatile uint64_t ticks;

void timer_irq(void) {
    ticks++;

    if (ticks % 100 == 1)
        serial_printf("\r[%d sec] ", (unsigned int)(ticks / 100 + 1));
}
