#include "resp/resp_parser.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace stratum::resp {
namespace {

/// Parse a decimal integer from a string.  Rejects empty strings, leading
/// whitespace, trailing non-digit characters, and values that would overflow
/// int64_t.  Accepts an optional leading '-' for negative numbers.
bool parse_integer(const std::string& s, int64_t& out) {
    if (s.empty()) return false;

    size_t start = 0;
    bool negative = false;
    if (s[0] == '-') {
        negative = true;
        start = 1;
    }
    if (start >= s.size()) return false;   // bare "-"

    int64_t val = 0;
    for (size_t i = start; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        int64_t digit = s[i] - '0';
        if (val > (INT64_MAX - digit) / 10) return false;   // overflow
        val = val * 10 + digit;
    }
    out = negative ? -val : val;
    return true;
}

/// Split a string on ASCII whitespace (space and tab).
/// No quoting or escaping — intentional limitation for inline commands.
std::vector<std::string> split_whitespace(const std::string& s) {
    std::vector<std::string> tokens;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        if (i >= s.size()) break;
        size_t start = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t') ++i;
        tokens.emplace_back(s, start, i - start);
    }
    return tokens;
}

/// In-place uppercase for ASCII bytes.  Non-ASCII bytes pass through
/// unchanged — command names are always ASCII.
void to_upper(std::string& s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

RespParser::RespParser(size_t max_inline_length, size_t max_buffer_size)
    : max_inline_length_(max_inline_length)
    , max_buffer_size_(max_buffer_size) {}

// ---------------------------------------------------------------------------
// Error helper
// ---------------------------------------------------------------------------

ParseResult RespParser::make_error(std::vector<Command> commands,
                                   const std::string& msg) {
    poisoned_ = true;
    poison_message_ = msg;

    ParseResult result;
    result.commands = std::move(commands);
    result.status = ParseStatus::Error;
    result.error = msg;
    return result;
}

// ---------------------------------------------------------------------------
// feed()
// ---------------------------------------------------------------------------

ParseResult RespParser::feed(std::string_view data) {
    // ── Poisoned? ──────────────────────────────────────────────────────
    if (poisoned_) {
        ParseResult result;
        result.status = ParseStatus::Error;
        result.error  = poison_message_;
        return result;
    }

    // ── Append input ───────────────────────────────────────────────────
    buffer_.append(data.data(), data.size());

    // ── Overall buffer limit ───────────────────────────────────────────
    if (buffer_.size() > max_buffer_size_) {
        return make_error({}, "maximum buffer size exceeded");
    }

    // ── Extract loop ───────────────────────────────────────────────────
    // Pull out as many complete commands as possible.  Any incomplete
    // suffix stays in buffer_ for the next feed().
    std::vector<Command> commands;

    while (true) {

        // ── State::Initial — decide inline vs. multibulk ──────────────
        if (state_ == State::Initial) {
            if (buffer_.empty()) break;

            if (buffer_[0] == '*') {
                // ── Multibulk header: *<count>\r\n ─────────────────────
                auto nl = buffer_.find('\n');
                if (nl == std::string::npos) break;   // need more data

                size_t line_end = (nl > 0 && buffer_[nl - 1] == '\r')
                                      ? nl - 1
                                      : nl;

                std::string count_str(buffer_, 1, line_end - 1);
                int64_t count = 0;
                if (!parse_integer(count_str, count)) {
                    return make_error(std::move(commands),
                        "invalid multibulk count: '" + count_str + "'");
                }
                if (count < 0) {
                    return make_error(std::move(commands),
                        "negative multibulk count");
                }

                buffer_.erase(0, nl + 1);

                if (count == 0) {
                    // *0\r\n — empty array, no command emitted, not an error
                    continue;
                }

                multibulk_argc_ = count;
                multibulk_parts_.clear();
                state_ = State::MultibulkArgs;
                // fall through to MultibulkArgs below

            } else {
                // ── Inline command ─────────────────────────────────────
                auto nl = buffer_.find('\n');

                if (nl == std::string::npos) {
                    // No terminator yet — check inline length limit
                    if (buffer_.size() > max_inline_length_) {
                        return make_error(std::move(commands),
                            "inline command exceeds maximum length");
                    }
                    break;   // need more data
                }

                size_t line_end = (nl > 0 && buffer_[nl - 1] == '\r')
                                      ? nl - 1
                                      : nl;

                std::string line(buffer_, 0, line_end);
                buffer_.erase(0, nl + 1);

                auto tokens = split_whitespace(line);
                if (tokens.empty()) {
                    // Empty / whitespace-only line → zero commands, not error
                    continue;
                }

                Command cmd;
                cmd.name = std::move(tokens[0]);
                to_upper(cmd.name);
                for (size_t i = 1; i < tokens.size(); ++i) {
                    cmd.args.push_back(std::move(tokens[i]));
                }
                commands.push_back(std::move(cmd));
                continue;
            }
        }

        // ── State::MultibulkArgs — consume $len\r\ndata\r\n chunks ───
        if (state_ == State::MultibulkArgs) {
            bool need_more = false;

            while (static_cast<int64_t>(multibulk_parts_.size()) < multibulk_argc_) {
                if (buffer_.empty()) { need_more = true; break; }

                // Expect '$'
                if (buffer_[0] != '$') {
                    return make_error(std::move(commands),
                        std::string("expected '$' but got '") + buffer_[0] + "'");
                }

                // Find end of $<len> line
                auto nl = buffer_.find('\n');
                if (nl == std::string::npos) { need_more = true; break; }

                size_t line_end = (nl > 0 && buffer_[nl - 1] == '\r')
                                      ? nl - 1
                                      : nl;

                std::string len_str(buffer_, 1, line_end - 1);
                int64_t len = 0;
                if (!parse_integer(len_str, len)) {
                    return make_error(std::move(commands),
                        "invalid bulk string length: '" + len_str + "'");
                }
                if (len < 0) {
                    return make_error(std::move(commands),
                        "negative bulk string length");
                }

                size_t data_start = nl + 1;
                auto bulk_len = static_cast<size_t>(len);

                // Need data_start + bulk_len bytes of payload, then \n (or \r\n)
                if (buffer_.size() < data_start + bulk_len + 1) {
                    need_more = true;
                    break;
                }

                // Verify trailing delimiter after the payload
                size_t after_data = data_start + bulk_len;
                size_t consume_end = 0;

                if (buffer_[after_data] == '\r') {
                    if (buffer_.size() < after_data + 2) {
                        need_more = true;
                        break;
                    }
                    if (buffer_[after_data + 1] != '\n') {
                        return make_error(std::move(commands),
                            "expected \\r\\n after bulk data");
                    }
                    consume_end = after_data + 2;
                } else if (buffer_[after_data] == '\n') {
                    consume_end = after_data + 1;
                } else {
                    return make_error(std::move(commands),
                        "expected \\r\\n after bulk data");
                }

                // Extract the bulk-string payload (binary-safe)
                multibulk_parts_.emplace_back(buffer_, data_start, bulk_len);
                buffer_.erase(0, consume_end);
            }

            if (need_more) break;

            // All parts collected — emit command
            Command cmd;
            cmd.name = std::move(multibulk_parts_[0]);
            to_upper(cmd.name);
            for (size_t i = 1; i < multibulk_parts_.size(); ++i) {
                cmd.args.push_back(std::move(multibulk_parts_[i]));
            }
            commands.push_back(std::move(cmd));

            multibulk_parts_.clear();
            multibulk_argc_ = 0;
            state_ = State::Initial;
            continue;
        }

        break;   // unreachable in normal flow, safety net
    }

    // ── Determine final status ─────────────────────────────────────────
    ParseResult result;
    result.commands = std::move(commands);
    result.status   = (buffer_.empty() && state_ == State::Initial)
                          ? ParseStatus::Complete
                          : ParseStatus::Incomplete;
    return result;
}

} // namespace stratum::resp
