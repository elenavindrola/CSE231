#include <stdint.h>
#include <unistd.h>

struct {
    uint64_t marker;
    uint64_t scratch[4];
} volatile testdata = {
    .marker  = 0xDEADBEEFCAFEBABE,
    .scratch = {0x1111, 0x2222, 0x3333, 0x4444},
};

int main(void) {
    while (testdata.marker == 0xDEADBEEFCAFEBABE)
        usleep(10000);
    return 0;
}
