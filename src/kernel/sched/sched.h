#pragma once
#include "process.h"

void sched_init(void);
void sched_yield(void);
void sched_run(void);
uint64_t *sched_irq_return(uint64_t *frame);
