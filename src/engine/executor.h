#pragma once

#include "engine/hash_table.h"
#include "resp/resp_parser.h"
#include "resp/result.h"

#include <chrono>
#include <cstdint>

namespace stratum::engine {

/// Returns current Unix wall-clock time in milliseconds for production use.
inline int64_t now_ms() {
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
}

/// Executes parsed Commands against HashTable storage, returning protocol-independent Results.
///
/// Layering invariant: Executor knows about Command, HashTable, and Result.
/// It never constructs raw RESP wire protocol bytes.
class Executor {
public:
    explicit Executor(HashTable& storage) : storage_(storage) {}

    /// Execute a command at the specified wall-clock timestamp now_ms.
    Result execute(const resp::Command& cmd, int64_t now_ms);

private:
    HashTable& storage_;

    Result handle_ping(const resp::Command& cmd);
    Result handle_set(const resp::Command& cmd);
    Result handle_get(const resp::Command& cmd, int64_t now_ms);
    Result handle_del(const resp::Command& cmd, int64_t now_ms);
    Result handle_exists(const resp::Command& cmd, int64_t now_ms);
    Result handle_expire(const resp::Command& cmd, int64_t now_ms);
    Result handle_ttl(const resp::Command& cmd, int64_t now_ms);
};

} // namespace stratum::engine
