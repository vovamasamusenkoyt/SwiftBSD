#include "kernel.h"

static volatile uint64_t ticks;

void timer_irq(void) {
    ticks++;
}

uint64_t timer_get_ticks(void) {
    return ticks;
}
