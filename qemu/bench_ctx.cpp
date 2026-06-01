#include "qemu_executor.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

using clk = std::chrono::steady_clock;

static double ms(clk::time_point a, clk::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

struct Stats {
    double mean;
    double stddev;
};

static Stats compute(const std::vector<double>& v) {
    double sum = 0;
    for (double x : v) sum += x;
    double mean = sum / v.size();

    double sq_sum = 0;
    for (double x : v) sq_sum += (x - mean) * (x - mean);
    double stddev = std::sqrt(sq_sum / v.size());

    return {mean, stddev};
}

static bool run_bench(const std::string& binary, const std::string& label,
                      const std::vector<std::string>& args, int rounds) {
    QemuConfig config;
    config.enable_gdb = true;
    QemuExecutor qemu(config);

    if (!qemu.launch(binary, args)) {
        fprintf(stderr, "launch failed for %s\n", label.c_str());
        return false;
    }

    // Let the guest allocate and touch memory before we start timing
    usleep(500'000);

    std::vector<double> ckpt_times, restore_times, total_times;

    for (int r = 0; r < rounds; r++) {
        auto t0 = clk::now();
        auto ctx = qemu.checkpoint();
        auto t1 = clk::now();

        if (!ctx) {
            fprintf(stderr, "checkpoint failed (%s round %d)\n",
                    label.c_str(), r);
            return false;
        }

        auto t2 = clk::now();
        bool ok = qemu.restore(*ctx, binary, args);
        auto t3 = clk::now();

        if (!ok) {
            fprintf(stderr, "restore failed (%s round %d)\n",
                    label.c_str(), r);
            return false;
        }

        // Let the restored guest settle before next round
        usleep(200'000);

        ckpt_times.push_back(ms(t0, t1));
        restore_times.push_back(ms(t2, t3));
        total_times.push_back(ms(t0, t3));
    }

    auto ckpt_s = compute(ckpt_times);
    auto rest_s = compute(restore_times);
    auto tot_s = compute(total_times);

    printf("  %-14s %10.2f %10.2f\n", "checkpoint", ckpt_s.mean, ckpt_s.stddev);
    printf("  %-14s %10.2f %10.2f\n", "restore", rest_s.mean, rest_s.stddev);
    printf("  %-14s %10.2f %10.2f\n", "total", tot_s.mean, tot_s.stddev);

    return true;
}

int main() {
    namespace fs = std::filesystem;

    if (std::system("which qemu-riscv64 > /dev/null 2>&1") != 0) {
        fprintf(stderr, "qemu-riscv64 not found\n");
        return 1;
    }

    auto base = fs::path(__FILE__).parent_path() / "tests";
    std::string spin = base / "spin_rv";
    std::string memload = base / "memload_rv";

    if (!fs::exists(spin)) {
        fprintf(stderr, "spin_rv not found\n");
        return 1;
    }
    if (!fs::exists(memload)) {
        fprintf(stderr, "memload_rv not found\n");
        return 1;
    }

    constexpr int ROUNDS = 10;

    struct Scenario {
        std::string label;
        std::string binary;
        std::vector<std::string> args;
    };

    std::vector<Scenario> scenarios = {
        {"spin (tiny)",  spin,    {}},
        {"100 MB",       memload, {"100"}},
        {"500 MB",       memload, {"500"}},
        {"1000 MB",      memload, {"1000"}},
    };

    printf("Context switch benchmark (%d rounds each)\n\n", ROUNDS);

    for (auto& s : scenarios) {
        printf("=== %s ===\n", s.label.c_str());
        printf("  %-14s %10s %10s\n", "", "mean(ms)", "stddev(ms)");
        printf("  %-14s %10s %10s\n", "--------------", "----------", "----------");

        if (!run_bench(s.binary, s.label, s.args, ROUNDS)) {
            printf("  FAILED\n");
        }
        printf("\n");
    }

    return 0;
}
