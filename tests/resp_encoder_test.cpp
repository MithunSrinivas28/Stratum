#include <gtest/gtest.h>

#include "resp/resp_encoder.h"
#include "resp/result.h"

using namespace stratum;
using namespace stratum::resp;

TEST(RespEncoderTest, SimpleString) {
    Result res = SimpleString{"OK"};
    EXPECT_EQ(encode(res), "+OK\r\n");

    Result pong = SimpleString{"PONG"};
    EXPECT_EQ(encode(pong), "+PONG\r\n");
}

TEST(RespEncoderTest, Error) {
    Result err = Error{"ERR unknown command 'FOO'"};
    EXPECT_EQ(encode(err), "-ERR unknown command 'FOO'\r\n");
}

TEST(RespEncoderTest, Integer) {
    Result zero = Integer{0};
    EXPECT_EQ(encode(zero), ":0\r\n");

    Result positive = Integer{42};
    EXPECT_EQ(encode(positive), ":42\r\n");

    Result negative = Integer{-2};
    EXPECT_EQ(encode(negative), ":-2\r\n");
}

TEST(RespEncoderTest, BulkString) {
    Result bulk = BulkString{"hello"};
    EXPECT_EQ(encode(bulk), "$5\r\nhello\r\n");

    Result empty_bulk = BulkString{""};
    EXPECT_EQ(encode(empty_bulk), "$0\r\n\r\n");
}

TEST(RespEncoderTest, NullBulk) {
    Result null_b = NullBulk{};
    EXPECT_EQ(encode(null_b), "$-1\r\n");
}

TEST(RespEncoderTest, BulkStringBinarySafe) {
    // Bulk string containing \0, \r\n, and binary bytes
    std::string binary_data;
    binary_data += "abc";
    binary_data += '\0';
    binary_data += "\r\n";
    binary_data += "xyz";

    Result bulk = BulkString{binary_data};
    std::string encoded = encode(bulk);

    std::string expected = "$" + std::to_string(binary_data.size()) + "\r\n" + binary_data + "\r\n";
    EXPECT_EQ(encoded, expected);
    EXPECT_EQ(encoded.size(), 1 + std::to_string(binary_data.size()).size() + 2 + binary_data.size() + 2);
}
