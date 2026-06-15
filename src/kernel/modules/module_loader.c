#include "kernel.h"
#include "log.h"

#if USE_RUST_MODULE
extern int rust_module_init(const struct kernel_api *api);
#endif

void module_subsystem_init(void) {
    log_info("modules: Initializing subsystem");
}

void module_load_all(const struct kernel_api *api) {
    log_info("modules: Loading built-in modules");
#if USE_RUST_MODULE
    int ret = rust_module_init(api);
    log_ok("rust_module_init returned 0x%x", (unsigned)ret);
#else
    log_ok("(no rust module)");
#endif
}
