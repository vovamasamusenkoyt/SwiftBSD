#pragma once
#include <stdint.h>

void tss_init(void);
void tss_set_kernel_stack(uint64_t rsp0);
