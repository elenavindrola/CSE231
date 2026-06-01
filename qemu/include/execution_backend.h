#pragma once

#include "execution_context.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

enum class ExecutionState {
    Idle,
    Running,
    Stopped,
    Exited,
    Error,
};

class IExecutionBackend {
public:
    virtual ~IExecutionBackend() = default;

    virtual bool launch(const std::string& binary,
                        const std::vector<std::string>& args) = 0;

    virtual ExecutionState state() = 0;

    virtual int wait() = 0;

    virtual bool stop() = 0;

    virtual std::optional<ExecutionCheckpoint> checkpoint() = 0;

    virtual bool restore(const ExecutionCheckpoint& ckpt,
                         const std::string& binary,
                         const std::vector<std::string>& args) = 0;
};
