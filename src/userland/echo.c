#include <stdint.h>
#include "syscall.h"

void _start(void) {
    const char *msg = ARGS_PAGE;
    if (msg[0] == 0) {
        print("\n");
    } else {
        print(msg);
        print("\n");
    }
    exit();
}
