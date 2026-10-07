#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace stratum::resp {

/// A parsed command. name is uppercase-normalized by the parser.
/// args stores bulk-string data as raw bytes (binary-safe via std::string
/// with explicit size tracking — may contain \0, \r\n, etc.).
struct Command {
    std::string name;                // Uppercase-normalized command name
    std::vector<std::string> args;   // Binary-safe arguments (excludes name)
};

/// Outcome of a feed() call.
enum class ParseStatus {
    Complete,    // All input consumed into commands; buffer empty, no in-flight parse
    Incomplete,  // Partial input retained; zero or more commands may have been extracted
    Error        // Malformed input or limit violation — parser is permanently poisoned
};

/// Result returned by feed().
struct ParseResult {
    std::vector<Command> commands;   // Commands extracted during this feed
    ParseStatus status;
    std::string error;               // Human-readable message; populated only on Error
};

/// Persistent RESP parser — one instance per TCP connection.
///
/// Supports inline commands (space-delimited, no quoting) and multibulk
/// (RESP array) commands. Not thread-safe; only the owning connection
/// thread may call feed().
///
/// Two-tier buffer limits:
///   max_inline_length — caps an unterminated inline line (default 64 KB,
///                       matches Redis PROTO_INLINE_MAX_SIZE)
///   max_buffer_size   — caps total internal buffer (default 64 MB,
///                       accommodates legitimate large bulk strings)
///
/// Once feed() returns Error the parser is poisoned: every subsequent
/// feed() also returns Error without parsing. The connection layer must
/// tear down the connection.
class RespParser {
public:
    explicit RespParser(
        size_t max_inline_length = 64 * 1024,          // 64 KB
        size_t max_buffer_size   = 64 * 1024 * 1024    // 64 MB
    );

    /// Append raw bytes from a TCP read and extract all complete commands.
    ///
    /// Returns:
    ///   Complete   — all input consumed, commands (if any) in result.commands
    ///   Incomplete — partial data retained internally; commands may still
    ///                have been extracted from the portion that was complete
    ///   Error      — parser is poisoned; result.error describes the cause;
    ///                result.commands contains any commands that were fully
    ///                parsed before the error was encountered
    ParseResult feed(std::string_view data);

private:
    std::string buffer_;
    size_t max_inline_length_;
    size_t max_buffer_size_;
    bool poisoned_ = false;
    std::string poison_message_;

    // Multibulk in-progress state (persisted across feed() calls)
    enum class State { Initial, MultibulkArgs };
    State state_ = State::Initial;
    int64_t multibulk_argc_ = 0;
    std::vector<std::string> multibulk_parts_;

    ParseResult make_error(std::vector<Command> commands, const std::string& msg);
};

} // namespace stratum::resp
