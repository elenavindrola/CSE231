#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

volatile int keep_going = 1;

int main(int argc, char** argv) {
    size_t mb = 100;
    if (argc > 1) mb = (size_t)atol(argv[1]);

    size_t bytes = mb * 1024 * 1024;
    uint8_t* buf = (uint8_t*)malloc(bytes);
    if (!buf) return 1;

    // Touch every page so it's resident
    memset(buf, 0xAB, bytes);

    while (keep_going)
        usleep(10000);

    // Prevent the compiler from optimizing out the allocation
    return buf[0];
}
