cmake_minimum_required(VERSION 3.16)
project(hybrid_riscv_sched LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

enable_testing()  # aggregate qemu/'s tests so `ctest` works from the root build dir

# QEMU execution backend (sched_core, riscv-handler, tests).
# Exposes the sched_core static lib with PUBLIC include dir qemu/include
# and the USE_PROC_MEM compile definition.
add_subdirectory(qemu)

# Dispatcher — the binfmt_misc entry point and single routing point.
# Reads power state from shared memory and routes the guest binary to a
# concrete IExecutionBackend (QemuExecutor today, FpgaExecutor later).
add_executable(dispatcher dispatcher.cpp)
target_link_libraries(dispatcher PRIVATE sched_core rt)

# power_monitor — writes power state into shared memory for the dispatcher.
add_executable(power_monitor power_monitor.cpp)
target_link_libraries(power_monitor PRIVATE rt Threads::Threads)
