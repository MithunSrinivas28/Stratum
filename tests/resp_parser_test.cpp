#include <gtest/gtest.h>

#include "resp/resp_parser.h"

#include <string>

using namespace stratum::resp;

// ═══════════════════════════════════════════════════════════════════════════
//  Inline commands
// ═══════════════════════════════════════════════════════════════════════════

TEST(RespParserTest, InlineSimpleCommand) {
    RespParser parser;
    auto result = parser.feed("PING\r\n");

    ASSERT_EQ(result.status, ParseStatus::Complete);
    ASSERT_EQ(result.commands.size(), 1u);
    EXPECT_EQ(result.commands[0].name, "PING");
    EXPECT_TRUE(result.commands[0].args.empty());
}

TEST(RespParserTest, InlineWithArgs) {
    RespParser parser;
    auto result = parser.feed("SET key value\r\n");

    ASSERT_EQ(result.status, ParseStatus::Complete);
    ASSERT_EQ(result.commands.size(), 1u);
    EXPECT_EQ(result.commands[0].name, "SET");
    ASSERT_EQ(result.commands[0].args.size(), 2u);
    EXPECT_EQ(result.commands[0].args[0], "key");
    EXPECT_EQ(result.commands[0].args[1], "value");
}

TEST(RespParserTest, InlineEmpty) {
    // Contract point 15: empty inline line → zero commands, Complete, not error
    RespParser parser;
    auto result = parser.feed("\r\n");

    ASSERT_EQ(result.status, ParseStatus::Complete);
    EXPECT_TRUE(result.commands.empty());
}

TEST(RespParserTest, InlineNameUppercased) {
    // Contract point 14: Command.name normalized to uppercase
    RespParser parser;
    auto result = parser.feed("get mykey\r\n");

    ASSERT_EQ(result.status, ParseStatus::Complete);
    ASSERT_EQ(result.commands.size(), 1u);
    EXPECT_EQ(result.commands[0].name, "GET");
    ASSERT_EQ(result.commands[0].args.size(), 1u);
    EXPECT_EQ(result.commands[0].args[0], "mykey");
}

TEST(RespParserTest, InlineBareNewline) {
    // Bare \n accepted (matches Redis behavior)
    RespParser parser;
    auto result = parser.feed("PING\n");

    ASSERT_EQ(result.status, ParseStatus::Complete);
    ASSERT_EQ(result.commands.size(), 1u);
    EXPECT_EQ(result.commands[0].name, "PING");
    EXPECT_TRUE(result.commands[0].args.empty());
}

// ═══════════════════════════════════════════════════════════════════════════
//  Multibulk commands
// ═══════════════════════════════════════════════════════════════════════════

TEST(RespParserTest, MultibulkSimple) {
    RespParser parser;
    auto result = parser.feed("*1\r\n$4\r\nPING\r\n");

    ASSERT_EQ(result.status, ParseStatus::Complete);
    ASSERT_EQ(result.commands.size(), 1u);
    EXPECT_EQ(result.commands[0].name, "PING");
    EXPECT_TRUE(result.commands[0].args.empty());
}

TEST(RespParserTest, MultibulkWithArgs) {
    RespParser parser;
    auto result = parser.feed(
        "*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n");

    ASSERT_EQ(result.status, ParseStatus::Complete);
    ASSERT_EQ(result.commands.size(), 1u);
    EXPECT_EQ(result.commands[0].name, "SET");
    ASSERT_EQ(result.commands[0].args.size(), 2u);
    EXPECT_EQ(result.commands[0].args[0], "key");
    EXPECT_EQ(result.commands[0].args[1], "value");
}

TEST(RespParserTest, MultibulkNameUppercased) {
    RespParser parser;
    auto result = parser.feed("*1\r\n$4\r\nping\r\n");

    ASSERT_EQ(result.status, ParseStatus::Complete);
    ASSERT_EQ(result.commands.size(), 1u);
    EXPECT_EQ(result.commands[0].name, "PING");
}

TEST(RespParserTest, MultibulkBinarySafe) {
    // Bulk-string argument containing \r\n, \0, and space — would break
    // inline parsing but works in multibulk because of explicit lengths.
    RespParser parser;

    std::string binary_value;
    binary_value += "he";       // 2 bytes
    binary_value += "\r\n";     // 2 bytes
    binary_value += '\0';       // 1 byte
    binary_value += " world";   // 6 bytes  → total 11 bytes

    std::string input = "*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$";
    input += std::to_string(binary_value.size());
    input += "\r\n";
    input += binary_value;
    input += "\r\n";

    auto result = parser.feed(input);

    ASSERT_EQ(result.status, ParseStatus::Complete);
    ASSERT_EQ(result.commands.size(), 1u);
    EXPECT_EQ(result.commands[0].name, "SET");
    ASSERT_EQ(result.commands[0].args.size(), 2u);
    EXPECT_EQ(result.commands[0].args[0], "key");
    ASSERT_EQ(result.commands[0].args[1].size(), binary_value.size());
    EXPECT_EQ(result.commands[0].args[1], binary_value);
}

TEST(RespParserTest, MultibulkEmptyBulkString) {
    // $0\r\n\r\n — zero-length bulk string as argument
    RespParser parser;
    auto result = parser.feed("*2\r\n$3\r\nSET\r\n$0\r\n\r\n");

    ASSERT_EQ(result.status, ParseStatus::Complete);
    ASSERT_EQ(result.commands.size(), 1u);
    EXPECT_EQ(result.commands[0].name, "SET");
    ASSERT_EQ(result.commands[0].args.size(), 1u);
    EXPECT_EQ(result.commands[0].args[0], "");
}

// ═══════════════════════════════════════════════════════════════════════════
//  Partial reads (commands split across feeds)
// ═══════════════════════════════════════════════════════════════════════════

TEST(RespParserTest, PartialInlineRead) {
    RespParser parser;

    auto r1 = parser.feed("PI");
    EXPECT_EQ(r1.status, ParseStatus::Incomplete);
    EXPECT_TRUE(r1.commands.empty());

    auto r2 = parser.feed("NG\r\n");
    ASSERT_EQ(r2.status, ParseStatus::Complete);
    ASSERT_EQ(r2.commands.size(), 1u);
    EXPECT_EQ(r2.commands[0].name, "PING");
}

TEST(RespParserTest, PartialMultibulkHeader) {
    RespParser parser;

    // Just the header — no bulk strings yet
    auto r1 = parser.feed("*2\r\n");
    EXPECT_EQ(r1.status, ParseStatus::Incomplete);
    EXPECT_TRUE(r1.commands.empty());

    auto r2 = parser.feed("$3\r\nGET\r\n$3\r\nkey\r\n");
    ASSERT_EQ(r2.status, ParseStatus::Complete);
    ASSERT_EQ(r2.commands.size(), 1u);
    EXPECT_EQ(r2.commands[0].name, "GET");
    ASSERT_EQ(r2.commands[0].args.size(), 1u);
    EXPECT_EQ(r2.commands[0].args[0], "key");
}

TEST(RespParserTest, PartialMultibulkBulkData) {
    // Bulk-string payload split mid-data
    RespParser parser;

    auto r1 = parser.feed("*1\r\n$5\r\nhel");
    EXPECT_EQ(r1.status, ParseStatus::Incomplete);
    EXPECT_TRUE(r1.commands.empty());

    auto r2 = parser.feed("lo\r\n");
    ASSERT_EQ(r2.status, ParseStatus::Complete);
    ASSERT_EQ(r2.commands.size(), 1u);
    EXPECT_EQ(r2.commands[0].name, "HELLO");
}

TEST(RespParserTest, PartialMultibulkAcrossThreeFeeds) {
    RespParser parser;

    // Feed 1: header only
    auto r1 = parser.feed("*2\r\n");
    EXPECT_EQ(r1.status, ParseStatus::Incomplete);
    EXPECT_TRUE(r1.commands.empty());

    // Feed 2: first bulk string only
    auto r2 = parser.feed("$3\r\nGET\r\n");
    EXPECT_EQ(r2.status, ParseStatus::Incomplete);
    EXPECT_TRUE(r2.commands.empty());

    // Feed 3: second bulk string — completes the command
    auto r3 = parser.feed("$3\r\nkey\r\n");
    ASSERT_EQ(r3.status, ParseStatus::Complete);
    ASSERT_EQ(r3.commands.size(), 1u);
    EXPECT_EQ(r3.commands[0].name, "GET");
    ASSERT_EQ(r3.commands[0].args.size(), 1u);
    EXPECT_EQ(r3.commands[0].args[0], "key");
}

// ═══════════════════════════════════════════════════════════════════════════
//  Multiple commands in one feed
// ═══════════════════════════════════════════════════════════════════════════

TEST(RespParserTest, MultipleInlineOneFeed) {
    RespParser parser;
    auto result = parser.feed("PING\r\nGET key\r\n");

    ASSERT_EQ(result.status, ParseStatus::Complete);
    ASSERT_EQ(result.commands.size(), 2u);
    EXPECT_EQ(result.commands[0].name, "PING");
    EXPECT_EQ(result.commands[1].name, "GET");
    ASSERT_EQ(result.commands[1].args.size(), 1u);
    EXPECT_EQ(result.commands[1].args[0], "key");
}

TEST(RespParserTest, MultipleMultibulkOneFeed) {
    RespParser parser;
    auto result = parser.feed(
        "*1\r\n$4\r\nPING\r\n"
        "*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n");

    ASSERT_EQ(result.status, ParseStatus::Complete);
    ASSERT_EQ(result.commands.size(), 2u);
    EXPECT_EQ(result.commands[0].name, "PING");
    EXPECT_EQ(result.commands[1].name, "GET");
    ASSERT_EQ(result.commands[1].args.size(), 1u);
    EXPECT_EQ(result.commands[1].args[0], "key");
}

TEST(RespParserTest, MixedInlineAndMultibulk) {
    RespParser parser;
    auto result = parser.feed(
        "PING\r\n"
        "*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n");

    ASSERT_EQ(result.status, ParseStatus::Complete);
    ASSERT_EQ(result.commands.size(), 2u);
    EXPECT_EQ(result.commands[0].name, "PING");
    EXPECT_EQ(result.commands[1].name, "GET");
    ASSERT_EQ(result.commands[1].args.size(), 1u);
    EXPECT_EQ(result.commands[1].args[0], "key");
}

TEST(RespParserTest, CompleteFollowedByIncomplete) {
    // Contract point 6: return completed commands, retain incomplete suffix
    RespParser parser;

    auto r1 = parser.feed("PING\r\n*2\r\n$3\r\n");
    EXPECT_EQ(r1.status, ParseStatus::Incomplete);
    ASSERT_EQ(r1.commands.size(), 1u);
    EXPECT_EQ(r1.commands[0].name, "PING");

    // Complete the multibulk command in a second feed
    auto r2 = parser.feed("GET\r\n$3\r\nkey\r\n");
    ASSERT_EQ(r2.status, ParseStatus::Complete);
    ASSERT_EQ(r2.commands.size(), 1u);
    EXPECT_EQ(r2.commands[0].name, "GET");
    ASSERT_EQ(r2.commands[0].args.size(), 1u);
    EXPECT_EQ(r2.commands[0].args[0], "key");
}

// ═══════════════════════════════════════════════════════════════════════════
//  Malformed input → Error
// ═══════════════════════════════════════════════════════════════════════════

TEST(RespParserTest, MalformedMultibulkBadCount) {
    RespParser parser;
    auto result = parser.feed("*abc\r\n");

    EXPECT_EQ(result.status, ParseStatus::Error);
    EXPECT_FALSE(result.error.empty());
}

TEST(RespParserTest, MalformedMultibulkNegativeCount) {
    RespParser parser;
    auto result = parser.feed("*-2\r\n");

    EXPECT_EQ(result.status, ParseStatus::Error);
    EXPECT_FALSE(result.error.empty());
}

TEST(RespParserTest, MalformedMultibulkBadBulkLen) {
    RespParser parser;
    auto result = parser.feed("*1\r\n$xyz\r\n");

    EXPECT_EQ(result.status, ParseStatus::Error);
    EXPECT_FALSE(result.error.empty());
}

TEST(RespParserTest, MalformedMultibulkMissingDollar) {
    // After *1\r\n the parser expects '$' but gets 'X'
    RespParser parser;
    auto result = parser.feed("*1\r\nXhello\r\n");

    EXPECT_EQ(result.status, ParseStatus::Error);
    EXPECT_FALSE(result.error.empty());
}

// ═══════════════════════════════════════════════════════════════════════════
//  Poisoned parser (feed-after-error)
// ═══════════════════════════════════════════════════════════════════════════

TEST(RespParserTest, FeedAfterError) {
    // Contract point 11: once Error, every subsequent feed() also returns Error
    RespParser parser;

    auto r1 = parser.feed("*abc\r\n");
    ASSERT_EQ(r1.status, ParseStatus::Error);

    // Perfectly valid input — must still be rejected
    auto r2 = parser.feed("PING\r\n");
    EXPECT_EQ(r2.status, ParseStatus::Error);
    EXPECT_TRUE(r2.commands.empty());
}

TEST(RespParserTest, FeedAfterErrorPreservesPoison) {
    RespParser parser;

    auto r1 = parser.feed("*-1\r\n");
    ASSERT_EQ(r1.status, ParseStatus::Error);

    // Three subsequent feeds — all must return Error
    for (int i = 0; i < 3; ++i) {
        auto r = parser.feed("PING\r\n");
        EXPECT_EQ(r.status, ParseStatus::Error)
            << "feed #" << (i + 2) << " after error should still be Error";
        EXPECT_TRUE(r.commands.empty());
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Buffer / input-size enforcement
// ═══════════════════════════════════════════════════════════════════════════

TEST(RespParserTest, InlineMaxLengthExceeded) {
    // Small inline limit for testing (16 bytes)
    RespParser parser(16, 64 * 1024 * 1024);

    // 30 bytes of inline data with no newline → exceeds 16-byte inline limit
    auto result = parser.feed("THIS_IS_WAY_TOO_LONG_NO_NEWLN");
    EXPECT_EQ(result.status, ParseStatus::Error);
    EXPECT_FALSE(result.error.empty());
}

TEST(RespParserTest, OverallMaxBufferExceeded) {
    // Small overall buffer limit for testing (32 bytes)
    RespParser parser(64 * 1024, 32);

    // 40 bytes in one feed — exceeds 32-byte overall limit
    auto result = parser.feed(std::string(40, 'x'));
    EXPECT_EQ(result.status, ParseStatus::Error);
    EXPECT_FALSE(result.error.empty());
}
