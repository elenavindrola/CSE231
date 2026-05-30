# Power and Temperature Aware Dispatcher for Load Balancing QEMU and FPGA
 
Routes RISC-V binaries to QEMU or FPGA based on CPU power and thermal state.
 
## Build
 
```bash
g++ -O2 -std=c++20 power_monitor.cpp -o power_monitor -lrt
g++ -O2 -std=c++20 dispatcher.cpp -o dispatcher -lrt
```
 
## Run
 
```bash
# 1. start the power monitor (requires sudo for RAPL access)
sudo rm -f /dev/shm/cpu_power_state
sudo ./power_monitor &
#sleep to give sampling time
sleep 3
 
# 2. run a RISC-V (gcc-riscv64-linux-gnu) binary through the dispatcher
./dispatcher <riscv-binary>
# example with a hello world binary
./dispatcher ./hello
```
 
The dispatcher prints which backend was chosen, for example:
```
[dispatcher] temp=63.0°C cpu=2.2W -> QEMU
```
 
## Routing logic
 
| Condition | Backend |
|-|-|
| T < 85°C and cpu < 80W | QEMU |
| T > 85°C or cpu > 80W | FPGA |
| monitor not running | QEMU (safe default) |
 
## Dependencies
 
```bash
sudo apt-get install -y g++ qemu-user-static gcc-riscv64-linux-gnu
```
 
## Notes
 
- The power monitor reads RAPL via `/sys/class/powercap/` (root required)
- Power state is shared via POSIX shared memory at `/dev/shm/cpu_power_state`
- Thermal readings come from `/sys/class/thermal/` (no root required)
