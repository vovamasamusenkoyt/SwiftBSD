#pragma once
#include <stdint.h>

#define MAX_TASKS 16
#define STACK_SIZE 4096

struct task {
    uint64_t *frame;
    int state;
};

void sched_init(void);
int  task_create(void (*func)(void));
void sched_yield(void);
uint64_t *sched_irq_return(uint64_t *frame);
