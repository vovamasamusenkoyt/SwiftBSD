#include "kernel.h"

extern int rust_module_init(const struct kernel_api *api);

void module_subsystem_init(void) {
    serial_puts("[mod] Initializing module subsystem...\n");
}

void module_load_all(const struct kernel_api *api) {
    serial_puts("[mod] Loading built-in modules...\n");
    int ret = rust_module_init(api);
    serial_printf("[mod] rust_module_init returned: %x\n", ret);
}
