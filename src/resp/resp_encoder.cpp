#include "resp/resp_encoder.h"

namespace stratum::resp {

std::string encode(const SimpleString& simple) {
    std::string out;
    out.reserve(simple.value.size() + 3);
    out.push_back('+');
    out.append(simple.value);
    out.append("\r\n");
    return out;
}

std::string encode(const Error& error) {
    std::string out;
    out.reserve(error.message.size() + 3);
    out.push_back('-');
    out.append(error.message);
    out.append("\r\n");
    return out;
}

std::string encode(const Integer& integer) {
    std::string val_str = std::to_string(integer.value);
    std::string out;
    out.reserve(val_str.size() + 3);
    out.push_back(':');
    out.append(val_str);
    out.append("\r\n");
    return out;
}

std::string encode(const BulkString& bulk) {
    std::string len_str = std::to_string(bulk.value.size());
    std::string out;
    out.reserve(1 + len_str.size() + 2 + bulk.value.size() + 2);
    out.push_back('$');
    out.append(len_str);
    out.append("\r\n");
    out.append(bulk.value);
    out.append("\r\n");
    return out;
}

std::string encode(const NullBulk& /*null_bulk*/) {
    return "$-1\r\n";
}

std::string encode(const Result& result) {
    return std::visit([](const auto& res) { return encode(res); }, result);
}

} // namespace stratum::resp
