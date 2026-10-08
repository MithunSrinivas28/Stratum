#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace stratum {

/// Protocol-independent semantic result variants produced by command execution.

struct SimpleString {
    std::string value;

    bool operator==(const SimpleString& other) const {
        return value == other.value;
    }
};

struct Error {
    std::string message;

    bool operator==(const Error& other) const {
        return message == other.message;
    }
};

struct Integer {
    int64_t value = 0;

    bool operator==(const Integer& other) const {
        return value == other.value;
    }
};

struct BulkString {
    std::string value;

    bool operator==(const BulkString& other) const {
        return value == other.value;
    }
};

struct NullBulk {
    bool operator==(const NullBulk&) const {
        return true;
    }
};

using Result = std::variant<SimpleString, Error, Integer, BulkString, NullBulk>;

} // namespace stratum
