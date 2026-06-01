#pragma once

#include <string>

namespace binfmt {

constexpr const char* HANDLER_NAME = "riscv64-sched";
constexpr const char* REGISTER_PATH = "/proc/sys/fs/binfmt_misc/register";

bool register_handler(const std::string& interpreter_path);
bool unregister_handler();
bool is_registered();

} // namespace binfmt
