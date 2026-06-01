#include <unistd.h>

volatile int keep_going = 1;

int main(void) {
    while (keep_going)
        usleep(10000);
    return 0;
}
