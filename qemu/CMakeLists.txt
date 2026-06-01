cmake_minimum_required(VERSION 3.16)
project(fpga_sched LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(sched_core STATIC
    src/execution_context.cpp
    src/qemu_executor.cpp
    src/gdb_client.cpp
    src/binfmt.cpp
    src/proc_mem.cpp
)
target_include_directories(sched_core PUBLIC include)

option(USE_PROC_MEM "Use /proc/pid/mem for memory transfer instead of GDB stub" ON)
if(USE_PROC_MEM)
    target_compile_definitions(sched_core PUBLIC USE_PROC_MEM)
endif()

# Restorer-loader: a freestanding RV64 ELF that rebuilds a guest address space
# from a checkpoint blob (Tier-2 restore). Cross-compiled with the RISC-V
# toolchain when available; restore() bakes in its path (overridable at runtime
# via RISCV_RESTORER_BIN).
find_program(RISCV64_GCC riscv64-linux-gnu-gcc)
if(RISCV64_GCC)
    set(RESTORER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/restorer)
    set(RESTORER_BIN ${CMAKE_CURRENT_BINARY_DIR}/restorer_rv)
    add_custom_command(
        OUTPUT ${RESTORER_BIN}
        COMMAND ${RISCV64_GCC} -nostdlib -static -no-pie -mcmodel=medany
                -mno-relax -ffreestanding -fno-stack-protector -fno-pic -O2
                -T ${RESTORER_SRC}/restorer.ld
                ${RESTORER_SRC}/start.S ${RESTORER_SRC}/restorer.c
                -o ${RESTORER_BIN}
        DEPENDS ${RESTORER_SRC}/restorer.c ${RESTORER_SRC}/start.S
                ${RESTORER_SRC}/restorer.ld
        COMMENT "Building RISC-V restorer-loader (restorer_rv)"
        VERBATIM)
    add_custom_target(build_restorer ALL DEPENDS ${RESTORER_BIN})
    add_dependencies(sched_core build_restorer)
    target_compile_definitions(sched_core PRIVATE RESTORER_RV_PATH="${RESTORER_BIN}")
else()
    message(WARNING "riscv64-linux-gnu-gcc not found: restorer_rv will not be "
                    "built; Tier-2 restore needs RISCV_RESTORER_BIN at runtime.")
endif()

add_executable(riscv-handler src/handler.cpp)
target_link_libraries(riscv-handler PRIVATE sched_core)

add_executable(bench_ctx bench_ctx.cpp)
target_link_libraries(bench_ctx PRIVATE sched_core)

option(BUILD_TESTS "Build test suite" ON)
if(BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()

    add_executable(unit_tests
        tests/test_context.cpp
        tests/test_gdb_client.cpp
    )
    target_link_libraries(unit_tests PRIVATE sched_core GTest::gtest_main)
    target_include_directories(unit_tests PRIVATE include)
    add_test(NAME unit_tests COMMAND unit_tests)

    add_executable(integration_tests
        tests/test_qemu_executor.cpp
    )
    target_link_libraries(integration_tests PRIVATE sched_core GTest::gtest_main)
    target_include_directories(integration_tests PRIVATE include)
    add_test(NAME integration_tests COMMAND integration_tests)

    add_executable(proc_mem_tests
        tests/test_proc_mem.cpp
    )
    target_link_libraries(proc_mem_tests PRIVATE sched_core GTest::gtest_main)
    target_include_directories(proc_mem_tests PRIVATE include)
    add_test(NAME proc_mem_tests COMMAND proc_mem_tests)

    # Build the executor integration tests again with USE_PROC_MEM
    # so the read/write_guest_memory path through /proc/pid/mem is exercised.
    add_library(sched_core_procmem STATIC
        src/execution_context.cpp
        src/qemu_executor.cpp
        src/gdb_client.cpp
        src/binfmt.cpp
        src/proc_mem.cpp
    )
    target_include_directories(sched_core_procmem PUBLIC include)
    target_compile_definitions(sched_core_procmem PUBLIC USE_PROC_MEM)
    if(RISCV64_GCC)
        add_dependencies(sched_core_procmem build_restorer)
        target_compile_definitions(sched_core_procmem PRIVATE
            RESTORER_RV_PATH="${RESTORER_BIN}")
    endif()

    add_executable(integration_tests_procmem
        tests/test_qemu_executor.cpp
    )
    target_link_libraries(integration_tests_procmem PRIVATE sched_core_procmem GTest::gtest_main)
    target_include_directories(integration_tests_procmem PRIVATE include)
    add_test(NAME integration_tests_procmem COMMAND integration_tests_procmem)
endif()
