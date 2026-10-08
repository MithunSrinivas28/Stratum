#include "engine/executor.h"

#include <cctype>
#include <climits>
#include <string_view>

namespace stratum::engine {

namespace {

std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

bool parse_int64(const std::string& str, int64_t& out) {
    if (str.empty()) {
        return false;
    }
    size_t i = 0;
    bool negative = false;
    if (str[0] == '-') {
        negative = true;
        i = 1;
    } else if (str[0] == '+') {
        i = 1;
    }
    if (i >= str.size()) {
        return false;
    }

    uint64_t val = 0;
    const uint64_t max_limit = negative
        ? (static_cast<uint64_t>(INT64_MAX) + 1ULL)
        : static_cast<uint64_t>(INT64_MAX);

    for (; i < str.size(); ++i) {
        char c = str[i];
        if (c < '0' || c > '9') {
            return false;
        }
        uint64_t digit = static_cast<uint64_t>(c - '0');
        if (val > (max_limit - digit) / 10ULL) {
            return false;
        }
        val = val * 10ULL + digit;
    }

    if (negative) {
        if (val == static_cast<uint64_t>(INT64_MAX) + 1ULL) {
            out = INT64_MIN;
        } else {
            out = -static_cast<int64_t>(val);
        }
    } else {
        out = static_cast<int64_t>(val);
    }
    return true;
}

} // namespace

Result Executor::execute(const resp::Command& cmd, int64_t now_ms) {
    if (cmd.name == "PING") {
        return handle_ping(cmd);
    }
    if (cmd.name == "SET") {
        return handle_set(cmd);
    }
    if (cmd.name == "GET") {
        return handle_get(cmd, now_ms);
    }
    if (cmd.name == "DEL") {
        return handle_del(cmd, now_ms);
    }
    if (cmd.name == "EXISTS") {
        return handle_exists(cmd, now_ms);
    }
    if (cmd.name == "EXPIRE") {
        return handle_expire(cmd, now_ms);
    }
    if (cmd.name == "TTL") {
        return handle_ttl(cmd, now_ms);
    }

    return Error{"ERR unknown command '" + cmd.name + "'"};
}

Result Executor::handle_ping(const resp::Command& cmd) {
    if (cmd.args.empty()) {
        return SimpleString{"PONG"};
    }
    if (cmd.args.size() == 1) {
        return BulkString{cmd.args[0]};
    }
    return Error{"ERR wrong number of arguments for 'ping' command"};
}

Result Executor::handle_set(const resp::Command& cmd) {
    if (cmd.args.size() < 2) {
        return Error{"ERR wrong number of arguments for 'set' command"};
    }
    if (cmd.args.size() > 2) {
        return Error{"ERR syntax error"};
    }
    storage_.set(cmd.args[0], cmd.args[1]);
    return SimpleString{"OK"};
}

Result Executor::handle_get(const resp::Command& cmd, int64_t now_ms) {
    if (cmd.args.size() != 1) {
        return Error{"ERR wrong number of arguments for 'get' command"};
    }
    auto val = storage_.get(cmd.args[0], now_ms);
    if (!val.has_value()) {
        return NullBulk{};
    }
    return BulkString{std::move(*val)};
}

Result Executor::handle_del(const resp::Command& cmd, int64_t now_ms) {
    if (cmd.args.empty()) {
        return Error{"ERR wrong number of arguments for 'del' command"};
    }
    int64_t count = 0;
    for (const auto& key : cmd.args) {
        if (storage_.remove(key, now_ms)) {
            ++count;
        }
    }
    return Integer{count};
}

Result Executor::handle_exists(const resp::Command& cmd, int64_t now_ms) {
    if (cmd.args.empty()) {
        return Error{"ERR wrong number of arguments for 'exists' command"};
    }
    int64_t count = 0;
    for (const auto& key : cmd.args) {
        if (storage_.exists(key, now_ms)) {
            ++count;
        }
    }
    return Integer{count};
}

Result Executor::handle_expire(const resp::Command& cmd, int64_t now_ms) {
    if (cmd.args.size() != 2) {
        return Error{"ERR wrong number of arguments for 'expire' command"};
    }

    int64_t seconds = 0;
    if (!parse_int64(cmd.args[1], seconds)) {
        return Error{"ERR value is not an integer or out of range"};
    }

    // Check multiplication overflow: seconds * 1000
    if (seconds > INT64_MAX / 1000 || seconds < INT64_MIN / 1000) {
        return Error{"ERR value is not an integer or out of range"};
    }
    int64_t delta_ms = seconds * 1000;

    // Check addition overflow: now_ms + delta_ms
    if (delta_ms > 0 && now_ms > INT64_MAX - delta_ms) {
        return Error{"ERR value is not an integer or out of range"};
    }
    if (delta_ms < 0 && now_ms < INT64_MIN - delta_ms) {
        return Error{"ERR value is not an integer or out of range"};
    }

    const auto& key = cmd.args[0];

    // Check if key is alive (lazy expiration occurs if expired)
    if (!storage_.exists(key, now_ms)) {
        return Integer{0};
    }

    if (seconds <= 0) {
        storage_.remove(key, now_ms);
        return Integer{1};
    }

    int64_t deadline = now_ms + delta_ms;
    storage_.set_expiry(key, deadline, now_ms);
    return Integer{1};
}

Result Executor::handle_ttl(const resp::Command& cmd, int64_t now_ms) {
    if (cmd.args.size() != 1) {
        return Error{"ERR wrong number of arguments for 'ttl' command"};
    }

    auto ttl_res = storage_.get_ttl(cmd.args[0], now_ms);
    if (ttl_res.status == TtlStatus::Missing) {
        return Integer{-2};
    }
    if (ttl_res.status == TtlStatus::NoExpiry) {
        return Integer{-1};
    }

    // Key is alive with active expiry (ttl_res.remaining_ms > 0).
    // Convert ms -> seconds, rounded to nearest: (remaining_ms + 500) / 1000.
    // A still-alive key must never return -2.
    int64_t seconds = (ttl_res.remaining_ms + 500) / 1000;
    return Integer{seconds};
}

} // namespace stratum::engine
