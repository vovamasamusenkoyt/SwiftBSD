#include "kernel.h"

static volatile uint64_t ticks;

void timer_irq(void) {
    ticks++;
}
