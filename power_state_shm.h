// power_state_shm.h — shared between monitor daemon and dispatcher
// Both sides include this header. The struct layout must never change
// between recompiles of the two binaries.

#pragma once
#include <cstdint>
#include <atomic>

#define SHM_NAME  "/cpu_power_state"
#define SHM_SIZE  sizeof(PowerState)

struct PowerState {
    // Written by monitor, read by dispatcher.
    // seq is odd while a write is in progress (seqlock pattern).
    std::atomic<uint32_t> seq;

    float    temp_max_c;
    float    cpu_total_watts;
    float    fpga_watts;         // -1 = not available
    bool     throttle;           // true: route to FPGA
    uint64_t timestamp_us;       // unix time of last update

    float    thermal_limit_c;    // config snapshot, informational
    float    power_limit_w;
};
