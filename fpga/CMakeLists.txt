# FPGA execution backend.
#
# libfpgaexec lives in the cva6_fk33 submodule (host/) with a plain Makefile;
# build its sources directly here so one CMake invocation covers everything.
set(FPGAEXEC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../cva6_fk33/host)

add_library(fpgaexec STATIC
    ${FPGAEXEC_DIR}/src/elf_loader.cpp
    ${FPGAEXEC_DIR}/src/xdma.cpp
    ${FPGAEXEC_DIR}/src/fpga_backend.cpp
    ${FPGAEXEC_DIR}/src/qemu_backend.cpp
)
target_include_directories(fpgaexec PUBLIC ${FPGAEXEC_DIR}/include)

# FpgaExecutor / QemuVirtExecutor: IExecutionBackend (from sched_core's
# headers) over libfpgaexec.
add_library(fpga_core STATIC
    fpgaexec_adapter.cpp
    fpga_executor.cpp
    qemu_virt_executor.cpp
)
target_include_directories(fpga_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fpga_core
    PUBLIC fpgaexec sched_core
    PRIVATE Threads::Threads
)
