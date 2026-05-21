// power_monitor.cpp - CPU power + thermal monitor daemon
// Writes to POSIX shared memory read by the binfmt_misc dispatcher.
// Build: g++ -O2 -std=c++20 power_monitor.cpp -o power_monitor -lrt
// Run: sudo ./power_monitor
// sudo ./power_monitor --once (single snapshot, print + exit)

#include "power_state_shm.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono;

// config 

static constexpr float    THERMAL_LIMIT_C  = 85.0f;
static constexpr float    POWER_LIMIT_W    = 65.0f;
static constexpr int      POLL_MS          = 100;

// helpers 

static std::optional<uint64_t> read_uint64(const fs::path& p) {
    std::ifstream f(p);
    if (!f) return std::nullopt;
    uint64_t v; f >> v;
    return f ? std::optional{v} : std::nullopt;
}

static std::optional<float> read_temp_c(const fs::path& p) {
    auto v = read_uint64(p);
    if (!v) return std::nullopt;
    return static_cast<float>(*v) / 1000.0f;
}

// RAPL

struct RaplDomain {
    std::string  name;
    fs::path     energy_path;
    uint64_t     max_energy_uj = 0;
    uint64_t     prev_uj       = 0;
    bool         primed        = false;
};

static std::vector<RaplDomain> find_rapl_domains() {
    std::vector<RaplDomain> out;
    const fs::path base = "/sys/class/powercap/intel-rapl";
    if (!fs::exists(base)) return out;

    for (auto& e : fs::directory_iterator(base)) {
        auto dirname = e.path().filename().string();
        // skip sub-domains (intel-rapl:0:0, etc.)
        if (std::count(dirname.begin(), dirname.end(), ':') != 1) continue;

        fs::path ep = e.path() / "energy_uj";
        if (!fs::exists(ep)) continue;

        RaplDomain d;
        d.energy_path = ep;
        std::ifstream nf(e.path() / "name");
        std::getline(nf, d.name);

        if (auto m = read_uint64(e.path() / "max_energy_range_uj"))
            d.max_energy_uj = *m;

        out.push_back(std::move(d));
    }
    return out;
}

static float sample_rapl_watts(std::vector<RaplDomain>& domains, float elapsed_s) {
    float total = 0.0f;
    for (auto& d : domains) {
        auto cur = read_uint64(d.energy_path);
        if (!cur) continue;
        if (!d.primed) { d.prev_uj = *cur; d.primed = true; continue; }

        int64_t delta = static_cast<int64_t>(*cur) - static_cast<int64_t>(d.prev_uj);
        if (delta < 0 && d.max_energy_uj > 0)
            delta += static_cast<int64_t>(d.max_energy_uj);  // wraparound
        d.prev_uj = *cur;

        if (elapsed_s > 0.0f)
            total += static_cast<float>(delta) / 1e6f / elapsed_s;
    }
    return total;
}

// thermal 

struct ThermalZone { std::string label; fs::path temp_path; };

static std::vector<ThermalZone> find_thermal_zones() {
    std::vector<ThermalZone> out;
    const fs::path base = "/sys/class/thermal";
    if (!fs::exists(base)) return out;

    for (auto& e : fs::directory_iterator(base)) {
        if (e.path().filename().string().find("thermal_zone") == std::string::npos)
            continue;
        fs::path tp = e.path() / "temp";
        if (!fs::exists(tp)) continue;

        std::string label;
        std::ifstream lf(e.path() / "type");
        std::getline(lf, label);

        // prefer CPU-related zones
        bool cpu = label.find("x86") != std::string::npos ||
                   label.find("cpu") != std::string::npos ||
                   label.find("pkg") != std::string::npos ||
                   label.find("core") != std::string::npos ||
                   label.find("acpitz") != std::string::npos;
        if (cpu) out.insert(out.begin(), {label, tp});
        else     out.push_back({label, tp});
    }
    return out;
}

static float max_temp_c(const std::vector<ThermalZone>& zones) {
    float max = -1.0f;
    for (auto& z : zones) {
        auto t = read_temp_c(z.temp_path);
        if (t && *t > max) max = *t;
    }
    return max;
}

// FPGA power (best-effort via xbutil) 

static float read_fpga_watts() {
    FILE* p = popen("xbutil examine --report electrical --format JSON 2>/dev/null", "r");
    if (!p) return -1.0f;

    std::string buf; char chunk[512];
    while (fgets(chunk, sizeof(chunk), p)) buf += chunk;
    pclose(p);

    // crude extraction: find "power_consumption_watts": <number>
    auto pos = buf.find("power_consumption_watts");
    if (pos == std::string::npos) pos = buf.find("power_watts");
    if (pos == std::string::npos) return -1.0f;

    pos = buf.find(':', pos);
    if (pos == std::string::npos) return -1.0f;
    return std::stof(buf.substr(pos + 1));
}

// shared memory 

static PowerState* open_shm_write() {
    // create (or reopen) the shared memory segment
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0644);
    if (fd < 0) { perror("shm_open"); return nullptr; }
    if (ftruncate(fd, SHM_SIZE) < 0) { perror("ftruncate"); close(fd); return nullptr; }

    auto* ps = static_cast<PowerState*>(
        mmap(nullptr, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)
    );
    close(fd);
    if (ps == MAP_FAILED) { perror("mmap"); return nullptr; }

    // zero-initialise (including the atomic seq counter)
    memset(ps, 0, SHM_SIZE);
    ps->thermal_limit_c = THERMAL_LIMIT_C;
    ps->power_limit_w   = POWER_LIMIT_W;
    ps->fpga_watts      = -1.0f;
    return ps;
}

// Seqlock write: increment seq to odd before write, even after.
// Dispatcher spins if it catches an odd value.
static void shm_write(PowerState* ps,
                      float temp_c, float cpu_w, float fpga_w, bool throttle) {
    auto seq = ps->seq.load(std::memory_order_relaxed);
    ps->seq.store(seq + 1, std::memory_order_release);   // odd → write in progress

    ps->temp_max_c      = temp_c;
    ps->cpu_total_watts = cpu_w;
    ps->fpga_watts      = fpga_w;
    ps->throttle        = throttle;
    ps->timestamp_us    = duration_cast<microseconds>(
                              system_clock::now().time_since_epoch()).count();

    ps->seq.store(seq + 2, std::memory_order_release);   // even → write done
}

// main 

int main(int argc, char* argv[]) {
    bool once = argc > 1 && std::string(argv[1]) == "--once";

    auto domains = find_rapl_domains();
    auto zones   = find_thermal_zones();

    if (domains.empty())
        fprintf(stderr, "WARNING: no RAPL domains found (need root + Intel/AMD powercap)\n");
    if (zones.empty())
        fprintf(stderr, "WARNING: no thermal zones found\n");

    PowerState* shm = nullptr;
    if (!once) {
        shm = open_shm_write();
        if (!shm) return 1;
        fprintf(stderr, "Monitor running → shm '%s'  (ctrl-C to stop)\n", SHM_NAME);
    }

    auto prev_ts = steady_clock::now();

    // prime RAPL counters
    sample_rapl_watts(domains, 0.0f);
    std::this_thread::sleep_for(milliseconds(POLL_MS));

    for (;;) {
        auto now = steady_clock::now();
        float elapsed = duration_cast<microseconds>(now - prev_ts).count() / 1e6f;
        prev_ts = now;

        float cpu_w  = sample_rapl_watts(domains, elapsed);
        float temp_c = max_temp_c(zones);
        float fpga_w = -1.0f;   // poll FPGA less often (expensive popen)

        bool throttle = (temp_c > THERMAL_LIMIT_C) || (cpu_w > POWER_LIMIT_W);

        if (once) {
            printf("temp_max_c:      %.1f°C  (limit %.0f°C)\n", temp_c, THERMAL_LIMIT_C);
            printf("cpu_total_watts: %.2f W  (limit %.0f W)\n", cpu_w,  POWER_LIMIT_W);
            printf("throttle:        %s\n", throttle ? "YES" : "no");
            return 0;
        }

        shm_write(shm, temp_c, cpu_w, fpga_w, throttle);
        std::this_thread::sleep_for(milliseconds(POLL_MS));
    }
}
