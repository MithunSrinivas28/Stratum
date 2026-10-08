#pragma once

#include "resp/result.h"
#include <string>

namespace stratum::resp {

/// Pure encoder functions that serialize protocol-independent semantic results
/// into Redis Serialization Protocol (RESP) wire format.
///
/// RESP format mapping:
///   SimpleString -> +<value>\r\n
///   Error        -> -<message>\r\n
///   Integer      -> :<value>\r\n
///   BulkString   -> $<length>\r\n<bytes>\r\n (binary-safe)
///   NullBulk     -> $-1\r\n

std::string encode(const SimpleString& simple);
std::string encode(const Error& error);
std::string encode(const Integer& integer);
std::string encode(const BulkString& bulk);
std::string encode(const NullBulk& null_bulk);

/// Encodes any Result variant into RESP wire format bytes.
std::string encode(const Result& result);

} // namespace stratum::resp
