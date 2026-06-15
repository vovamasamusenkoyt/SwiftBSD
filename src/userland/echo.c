#include <stdint.h>
#include "syscall.h"

void _start(void) {
    print("SwiftBSD\n");
    exit();
}
