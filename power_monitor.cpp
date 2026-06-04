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

// config (defaults; override with --temp-limit / --power-limit)

static float              g_thermal_limit_c = 85.0f;
static float              g_power_limit_w   = 80.0f;
static constexpr int      POLL_MS           = 100;

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
// Calculate power from energy readings
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

        // only consider CPU-related zones (no GPU, battery, SSD or other thermal readings)
        bool cpu = label.find("x86") != std::string::npos ||
                   label.find("cpu") != std::string::npos ||
                   label.find("pkg") != std::string::npos ||
                   label.find("core") != std::string::npos ||
                   label.find("acpitz") != std::string::npos;
        if (cpu) out.push_back({label, tp});
    }

    // AMD desktop chips (e.g. Ryzen 5700G) expose CPU temperature via hwmon
    // (k10temp Tctl/Tdie), not /sys/class/thermal — Intel coretemp likewise
    // when ACPI zones are absent. Collect those sensors too.
    const fs::path hwmon = "/sys/class/hwmon";
    if (fs::exists(hwmon)) {
        for (auto& e : fs::directory_iterator(hwmon)) {
            std::string chip;
            std::ifstream nf(e.path() / "name");
            std::getline(nf, chip);
            if (chip != "k10temp" && chip != "zenpower" && chip != "coretemp")
                continue;
            for (auto& s : fs::directory_iterator(e.path())) {
                const std::string fn = s.path().filename().string();
                if (fn.rfind("temp", 0) == 0 &&
                    fn.size() > 6 && fn.compare(fn.size() - 6, 6, "_input") == 0)
                    out.push_back({chip + "/" + fn, s.path()});
            }
        }
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
    ps->thermal_limit_c = g_thermal_limit_c;
    ps->power_limit_w   = g_power_limit_w;
    ps->fpga_watts      = -1.0f;
    return ps;
}

// --watch: follow the daemon's shared memory (no root needed) — handy while
// driving a load scenario to see exactly when the dispatcher would flip to FPGA.
static int watch_shm() {
    int fd = shm_open(SHM_NAME, O_RDONLY, 0);
    if (fd < 0) {
        fprintf(stderr, "no shared memory '%s' — is the monitor running?\n", SHM_NAME);
        return 1;
    }
    auto* ps = static_cast<const PowerState*>(
        mmap(nullptr, SHM_SIZE, PROT_READ, MAP_SHARED, fd, 0));
    close(fd);
    if (ps == MAP_FAILED) { perror("mmap"); return 1; }

    for (;;) {
        // seqlock read (same protocol as the dispatcher)
        float temp, cpu_w; bool throttle;
        for (;;) {
            uint32_t s1 = ps->seq.load(std::memory_order_acquire);
            if (s1 & 1) continue;
            temp     = ps->temp_max_c;
            cpu_w    = ps->cpu_total_watts;
            throttle = ps->throttle;
            uint32_t s2 = ps->seq.load(std::memory_order_acquire);
            if (s1 == s2) break;
        }
        printf("temp=%5.1fC (limit %.0f)  cpu=%6.2fW (limit %.0f)  -> %s\n",
               temp, ps->thermal_limit_c, cpu_w, ps->power_limit_w,
               throttle ? "FPGA" : "QEMU");
        fflush(stdout);
        std::this_thread::sleep_for(milliseconds(1000));
    }
}

// Seqlock write: increment seq to odd before write, even after.
// Dispatcher spins if it catches an odd value.
static void shm_write(PowerState* ps,
                      float temp_c, float cpu_w, float fpga_w, bool throttle) {
    auto seq = ps->seq.load(std::memory_order_relaxed);
    ps->seq.store(seq + 1, std::memory_order_release);   // odd -> write in progress

    ps->temp_max_c      = temp_c;
    ps->cpu_total_watts = cpu_w;
    ps->fpga_watts      = fpga_w;
    ps->throttle        = throttle;
    ps->timestamp_us    = duration_cast<microseconds>(
                              system_clock::now().time_since_epoch()).count();

    ps->seq.store(seq + 2, std::memory_order_release);   // even -> write done
}

// main 

int main(int argc, char* argv[]) {
    bool once = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--once")  once = true;
        else if (a == "--watch") return watch_shm();
        else if (a == "--temp-limit"  && i + 1 < argc) g_thermal_limit_c = std::atof(argv[++i]);
        else if (a == "--power-limit" && i + 1 < argc) g_power_limit_w   = std::atof(argv[++i]);
        else {
            fprintf(stderr,
                    "usage: power_monitor [--once] [--watch] "
                    "[--temp-limit C] [--power-limit W]\n");
            return 1;
        }
    }

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
        fprintf(stderr, "Monitor running -> shm '%s'  (ctrl-C to stop)\n", SHM_NAME);
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

        bool throttle = (temp_c > g_thermal_limit_c) || (cpu_w > g_power_limit_w);

        if (once) {
            printf("temp_max_c:      %.1f Celsius  (limit %.0f Celsius)\n", temp_c, g_thermal_limit_c);
            printf("cpu_total_watts: %.2f W  (limit %.0f W)\n", cpu_w,  g_power_limit_w);
            printf("Exceed Limit:        %s\n", throttle ? "YES" : "no");
            return 0;
        }

        shm_write(shm, temp_c, cpu_w, fpga_w, throttle);
        std::this_thread::sleep_for(milliseconds(POLL_MS));
    }
}
