#include <gtest/gtest.h>

#include "engine/executor.h"
#include "engine/hash_table.h"
#include "resp/resp_parser.h"
#include "resp/result.h"

#include <climits>
#include <string>

using namespace stratum;
using namespace stratum::engine;
using namespace stratum::resp;

// ═══════════════════════════════════════════════════════════════════════════
//  Storage Core Unit Tests (HashTable isolation)
// ═══════════════════════════════════════════════════════════════════════════

TEST(HashTableTest, BasicSetAndGet) {
    HashTable table;
    int64_t now = 1000;

    table.set("key1", "val1");
    auto res = table.get("key1", now);
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(*res, "val1");

    EXPECT_FALSE(table.get("nonexistent", now).has_value());
}

TEST(HashTableTest, OverwriteAndClearExpiry) {
    HashTable table;
    int64_t now = 1000;

    // Set with expiry
    table.set("key1", "val1", now + 5000);
    EXPECT_TRUE(table.exists("key1", now));

    // Overwrite without expiry
    table.set("key1", "val2");
    EXPECT_EQ(table.get("key1", now + 10000).value_or(""), "val2");
    EXPECT_EQ(table.get_ttl("key1", now + 10000).status, TtlStatus::NoExpiry);
}

TEST(HashTableTest, LazyExpirationOnGet) {
    HashTable table;
    int64_t now = 1000;

    table.set("k1", "v1", now + 100);
    EXPECT_EQ(table.raw_size(), 1u);

    // Access before expiry
    EXPECT_TRUE(table.get("k1", now + 50).has_value());

    // Access exactly at or after expiry -> removed
    EXPECT_FALSE(table.get("k1", now + 100).has_value());
    EXPECT_EQ(table.raw_size(), 0u);
}

TEST(HashTableTest, LazyExpirationOnRemove) {
    HashTable table;
    int64_t now = 1000;

    table.set("k1", "v1", now + 100);
    // Remove after expiry -> returns false (not alive), but entry erased
    EXPECT_FALSE(table.remove("k1", now + 200));
    EXPECT_EQ(table.raw_size(), 0u);
}

TEST(HashTableTest, LazyExpirationOnExists) {
    HashTable table;
    int64_t now = 1000;

    table.set("k1", "v1", now + 100);
    // Exists after expiry -> returns false, entry erased
    EXPECT_FALSE(table.exists("k1", now + 150));
    EXPECT_EQ(table.raw_size(), 0u);
}

TEST(HashTableTest, SetExpiry) {
    HashTable table;
    int64_t now = 1000;

    table.set("k1", "v1");
    EXPECT_TRUE(table.set_expiry("k1", now + 5000, now));
    EXPECT_FALSE(table.set_expiry("missing", now + 5000, now));

    // After expiration, set_expiry returns false and erases
    EXPECT_FALSE(table.set_expiry("k1", now + 10000, now + 6000));
    EXPECT_EQ(table.raw_size(), 0u);
}

TEST(HashTableTest, GetTtl) {
    HashTable table;
    int64_t now = 1000;

    // Missing key
    auto ttl_missing = table.get_ttl("missing", now);
    EXPECT_EQ(ttl_missing.status, TtlStatus::Missing);

    // Persistent key
    table.set("k_perm", "v");
    auto ttl_perm = table.get_ttl("k_perm", now);
    EXPECT_EQ(ttl_perm.status, TtlStatus::NoExpiry);

    // Key with expiry
    table.set("k_exp", "v", now + 2500);
    auto ttl_exp = table.get_ttl("k_exp", now);
    EXPECT_EQ(ttl_exp.status, TtlStatus::Ok);
    EXPECT_EQ(ttl_exp.remaining_ms, 2500);

    // After expiration
    auto ttl_after = table.get_ttl("k_exp", now + 3000);
    EXPECT_EQ(ttl_after.status, TtlStatus::Missing);
    EXPECT_EQ(table.raw_size(), 1u); // k_perm still remains
}

// ═══════════════════════════════════════════════════════════════════════════
//  Executor Command Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(ExecutorTest, Ping) {
    HashTable table;
    Executor exec(table);
    int64_t now = 1000;

    // 0 args -> SimpleString PONG
    Command cmd_zero{"PING", {}};
    auto res_zero = exec.execute(cmd_zero, now);
    ASSERT_TRUE(std::holds_alternative<SimpleString>(res_zero));
    EXPECT_EQ(std::get<SimpleString>(res_zero).value, "PONG");

    // 1 arg -> BulkString echoing argument
    Command cmd_one{"PING", {"hello world"}};
    auto res_one = exec.execute(cmd_one, now);
    ASSERT_TRUE(std::holds_alternative<BulkString>(res_one));
    EXPECT_EQ(std::get<BulkString>(res_one).value, "hello world");

    // >1 args -> arity error
    Command cmd_two{"PING", {"arg1", "arg2"}};
    auto res_two = exec.execute(cmd_two, now);
    ASSERT_TRUE(std::holds_alternative<Error>(res_two));
    EXPECT_EQ(std::get<Error>(res_two).message, "ERR wrong number of arguments for 'ping' command");
}

TEST(ExecutorTest, BasicSetAndGet) {
    HashTable table;
    Executor exec(table);
    int64_t now = 1000;

    Command set_cmd{"SET", {"mykey", "myval"}};
    auto set_res = exec.execute(set_cmd, now);
    ASSERT_TRUE(std::holds_alternative<SimpleString>(set_res));
    EXPECT_EQ(std::get<SimpleString>(set_res).value, "OK");

    Command get_cmd{"GET", {"mykey"}};
    auto get_res = exec.execute(get_cmd, now);
    ASSERT_TRUE(std::holds_alternative<BulkString>(get_res));
    EXPECT_EQ(std::get<BulkString>(get_res).value, "myval");

    Command get_missing{"GET", {"other"}};
    auto miss_res = exec.execute(get_missing, now);
    EXPECT_TRUE(std::holds_alternative<NullBulk>(miss_res));
}

TEST(ExecutorTest, Overwrite) {
    HashTable table;
    Executor exec(table);
    int64_t now = 1000;

    exec.execute(Command{"SET", {"k", "v1"}}, now);
    exec.execute(Command{"SET", {"k", "v2"}}, now);

    auto res = exec.execute(Command{"GET", {"k"}}, now);
    ASSERT_TRUE(std::holds_alternative<BulkString>(res));
    EXPECT_EQ(std::get<BulkString>(res).value, "v2");
}

TEST(ExecutorTest, DelExistingMissingAndMultipleKeys) {
    HashTable table;
    Executor exec(table);
    int64_t now = 1000;

    exec.execute(Command{"SET", {"k1", "v1"}}, now);
    exec.execute(Command{"SET", {"k2", "v2"}}, now);

    // DEL with mixture of existing and missing keys
    Command del_cmd{"DEL", {"k1", "nonexistent", "k2"}};
    auto del_res = exec.execute(del_cmd, now);
    ASSERT_TRUE(std::holds_alternative<Integer>(del_res));
    EXPECT_EQ(std::get<Integer>(del_res).value, 2);

    // DEL on already deleted keys
    auto del_again = exec.execute(del_cmd, now);
    ASSERT_TRUE(std::holds_alternative<Integer>(del_again));
    EXPECT_EQ(std::get<Integer>(del_again).value, 0);
}

TEST(ExecutorTest, ExistsMultipleAndDuplicates) {
    HashTable table;
    Executor exec(table);
    int64_t now = 1000;

    exec.execute(Command{"SET", {"k1", "v1"}}, now);
    exec.execute(Command{"SET", {"k2", "v2"}}, now);

    // Listing k1 twice counts twice as in Redis
    Command exists_cmd{"EXISTS", {"k1", "k2", "missing", "k1"}};
    auto res = exec.execute(exists_cmd, now);
    ASSERT_TRUE(std::holds_alternative<Integer>(res));
    EXPECT_EQ(std::get<Integer>(res).value, 3);
}

TEST(ExecutorTest, PersistentKeys) {
    HashTable table;
    Executor exec(table);
    int64_t now = 1000;

    exec.execute(Command{"SET", {"perm", "val"}}, now);

    // TTL on persistent key returns -1
    auto ttl_res = exec.execute(Command{"TTL", {"perm"}}, now + 1000000);
    ASSERT_TRUE(std::holds_alternative<Integer>(ttl_res));
    EXPECT_EQ(std::get<Integer>(ttl_res).value, -1);
}

TEST(ExecutorTest, ExpireThenTtl) {
    HashTable table;
    Executor exec(table);
    int64_t now = 1000;

    exec.execute(Command{"SET", {"k", "v"}}, now);

    // EXPIRE key 10 -> sets deadline = 1000 + 10000 = 11000
    auto exp_res = exec.execute(Command{"EXPIRE", {"k", "10"}}, now);
    ASSERT_TRUE(std::holds_alternative<Integer>(exp_res));
    EXPECT_EQ(std::get<Integer>(exp_res).value, 1);

    // At now + 4000 (remaining: 6000 ms -> 6 seconds)
    auto ttl_res = exec.execute(Command{"TTL", {"k"}}, now + 4000);
    ASSERT_TRUE(std::holds_alternative<Integer>(ttl_res));
    EXPECT_EQ(std::get<Integer>(ttl_res).value, 6);
}

TEST(ExecutorTest, ExpiredGetRemovesKey) {
    HashTable table;
    Executor exec(table);
    int64_t now = 1000;

    exec.execute(Command{"SET", {"k", "v"}}, now);
    exec.execute(Command{"EXPIRE", {"k", "2"}}, now); // expires at 3000

    // Before expiry
    auto get_before = exec.execute(Command{"GET", {"k"}}, 2999);
    ASSERT_TRUE(std::holds_alternative<BulkString>(get_before));
    EXPECT_EQ(std::get<BulkString>(get_before).value, "v");

    // At or after expiry -> NullBulk and key is removed from storage
    auto get_after = exec.execute(Command{"GET", {"k"}}, 3000);
    EXPECT_TRUE(std::holds_alternative<NullBulk>(get_after));
    EXPECT_EQ(table.raw_size(), 0u);
}

TEST(ExecutorTest, ExpiredExistsRemovesKey) {
    HashTable table;
    Executor exec(table);
    int64_t now = 1000;

    exec.execute(Command{"SET", {"k", "v"}}, now);
    exec.execute(Command{"EXPIRE", {"k", "1"}}, now); // expires at 2000

    auto exists_res = exec.execute(Command{"EXISTS", {"k"}}, 2000);
    ASSERT_TRUE(std::holds_alternative<Integer>(exists_res));
    EXPECT_EQ(std::get<Integer>(exists_res).value, 0);
    EXPECT_EQ(table.raw_size(), 0u);
}

TEST(ExecutorTest, ExpiredTtlRemovesKey) {
    HashTable table;
    Executor exec(table);
    int64_t now = 1000;

    exec.execute(Command{"SET", {"k", "v"}}, now);
    exec.execute(Command{"EXPIRE", {"k", "1"}}, now); // expires at 2000

    auto ttl_res = exec.execute(Command{"TTL", {"k"}}, 2001);
    ASSERT_TRUE(std::holds_alternative<Integer>(ttl_res));
    EXPECT_EQ(std::get<Integer>(ttl_res).value, -2);
    EXPECT_EQ(table.raw_size(), 0u);
}

TEST(ExecutorTest, ExpiredDelRemovesKeyAndNotCounted) {
    HashTable table;
    Executor exec(table);
    int64_t now = 1000;

    exec.execute(Command{"SET", {"k", "v"}}, now);
    exec.execute(Command{"EXPIRE", {"k", "1"}}, now); // expires at 2000

    auto del_res = exec.execute(Command{"DEL", {"k"}}, 2500);
    ASSERT_TRUE(std::holds_alternative<Integer>(del_res));
    EXPECT_EQ(std::get<Integer>(del_res).value, 0);
    EXPECT_EQ(table.raw_size(), 0u);
}

TEST(ExecutorTest, ExpireOnMissingKey) {
    HashTable table;
    Executor exec(table);
    int64_t now = 1000;

    auto exp_res = exec.execute(Command{"EXPIRE", {"nonexistent", "10"}}, now);
    ASSERT_TRUE(std::holds_alternative<Integer>(exp_res));
    EXPECT_EQ(std::get<Integer>(exp_res).value, 0);
}

TEST(ExecutorTest, ExpireWithSecondsZeroOrNegative) {
    HashTable table;
    Executor exec(table);
    int64_t now = 1000;

    // Seconds = 0 deletes key, returns 1
    exec.execute(Command{"SET", {"k1", "v1"}}, now);
    auto exp_zero = exec.execute(Command{"EXPIRE", {"k1", "0"}}, now);
    ASSERT_TRUE(std::holds_alternative<Integer>(exp_zero));
    EXPECT_EQ(std::get<Integer>(exp_zero).value, 1);
    EXPECT_FALSE(table.exists("k1", now));

    // Seconds < 0 deletes key, returns 1
    exec.execute(Command{"SET", {"k2", "v2"}}, now);
    auto exp_neg = exec.execute(Command{"EXPIRE", {"k2", "-5"}}, now);
    ASSERT_TRUE(std::holds_alternative<Integer>(exp_neg));
    EXPECT_EQ(std::get<Integer>(exp_neg).value, 1);
    EXPECT_FALSE(table.exists("k2", now));

    // Seconds <= 0 on missing key returns 0
    auto exp_miss = exec.execute(Command{"EXPIRE", {"k3", "-5"}}, now);
    ASSERT_TRUE(std::holds_alternative<Integer>(exp_miss));
    EXPECT_EQ(std::get<Integer>(exp_miss).value, 0);
}

TEST(ExecutorTest, ExpireWithNonInteger) {
    HashTable table;
    Executor exec(table);
    int64_t now = 1000;

    exec.execute(Command{"SET", {"k", "v"}}, now);

    auto res_alpha = exec.execute(Command{"EXPIRE", {"k", "abc"}}, now);
    ASSERT_TRUE(std::holds_alternative<Error>(res_alpha));
    EXPECT_EQ(std::get<Error>(res_alpha).message, "ERR value is not an integer or out of range");

    auto res_empty = exec.execute(Command{"EXPIRE", {"k", ""}}, now);
    ASSERT_TRUE(std::holds_alternative<Error>(res_empty));
    EXPECT_EQ(std::get<Error>(res_empty).message, "ERR value is not an integer or out of range");

    auto res_float = exec.execute(Command{"EXPIRE", {"k", "3.14"}}, now);
    ASSERT_TRUE(std::holds_alternative<Error>(res_float));
    EXPECT_EQ(std::get<Error>(res_float).message, "ERR value is not an integer or out of range");
}

TEST(ExecutorTest, ExpireOverflowGuard) {
    HashTable table;
    Executor exec(table);
    int64_t now = 1000;

    exec.execute(Command{"SET", {"k", "v"}}, now);

    // Number too large to multiply by 1000 or fit in int64_t
    auto res_overflow = exec.execute(Command{"EXPIRE", {"k", "9223372036854775807"}}, now);
    ASSERT_TRUE(std::holds_alternative<Error>(res_overflow));
    EXPECT_EQ(std::get<Error>(res_overflow).message, "ERR value is not an integer or out of range");
}

TEST(ExecutorTest, SetClearingOldExpiry) {
    HashTable table;
    Executor exec(table);
    int64_t now = 1000;

    exec.execute(Command{"SET", {"k", "v1"}}, now);
    exec.execute(Command{"EXPIRE", {"k", "5"}}, now); // expires at 6000

    // Overwrite with SET clears expiry
    exec.execute(Command{"SET", {"k", "v2"}}, now + 1000);

    // Key survives past original 6000 expiry
    auto ttl_res = exec.execute(Command{"TTL", {"k"}}, now + 8000);
    ASSERT_TRUE(std::holds_alternative<Integer>(ttl_res));
    EXPECT_EQ(std::get<Integer>(ttl_res).value, -1);

    auto get_res = exec.execute(Command{"GET", {"k"}}, now + 8000);
    ASSERT_TRUE(std::holds_alternative<BulkString>(get_res));
    EXPECT_EQ(std::get<BulkString>(get_res).value, "v2");
}

TEST(ExecutorTest, ReplacementOfExpiredKey) {
    HashTable table;
    Executor exec(table);
    int64_t now = 1000;

    exec.execute(Command{"SET", {"k", "v1"}}, now);
    exec.execute(Command{"EXPIRE", {"k", "2"}}, now); // expires at 3000

    // After key is expired, SET creates a fresh persistent entry
    exec.execute(Command{"SET", {"k", "v2"}}, 3500);

    auto get_res = exec.execute(Command{"GET", {"k"}}, 3500);
    ASSERT_TRUE(std::holds_alternative<BulkString>(get_res));
    EXPECT_EQ(std::get<BulkString>(get_res).value, "v2");

    auto ttl_res = exec.execute(Command{"TTL", {"k"}}, 3500);
    ASSERT_TRUE(std::holds_alternative<Integer>(ttl_res));
    EXPECT_EQ(std::get<Integer>(ttl_res).value, -1);
}

TEST(ExecutorTest, TtlRoundingAndNeverMinusTwoWhenAlive) {
    HashTable table;
    Executor exec(table);
    int64_t now = 10000;

    // Test standard rounding:
    // 2400 ms remaining -> (2400 + 500) / 1000 = 2 seconds
    table.set("k1", "v", now + 2400);
    auto r1 = exec.execute(Command{"TTL", {"k1"}}, now);
    ASSERT_TRUE(std::holds_alternative<Integer>(r1));
    EXPECT_EQ(std::get<Integer>(r1).value, 2);

    // 2600 ms remaining -> (2600 + 500) / 1000 = 3 seconds
    table.set("k2", "v", now + 2600);
    auto r2 = exec.execute(Command{"TTL", {"k2"}}, now);
    ASSERT_TRUE(std::holds_alternative<Integer>(r2));
    EXPECT_EQ(std::get<Integer>(r2).value, 3);

    // 1500 ms remaining -> (1500 + 500) / 1000 = 2 seconds
    table.set("k3", "v", now + 1500);
    auto r3 = exec.execute(Command{"TTL", {"k3"}}, now);
    ASSERT_TRUE(std::holds_alternative<Integer>(r3));
    EXPECT_EQ(std::get<Integer>(r3).value, 2);

    // 1499 ms remaining -> (1499 + 500) / 1000 = 1 second
    table.set("k4", "v", now + 1499);
    auto r4 = exec.execute(Command{"TTL", {"k4"}}, now);
    ASSERT_TRUE(std::holds_alternative<Integer>(r4));
    EXPECT_EQ(std::get<Integer>(r4).value, 1);

    // Sub-second remaining time: 200 ms remaining -> 0 seconds.
    // Critical contract requirement: A still-alive key must never return -2.
    table.set("k5", "v", now + 200);
    auto r5 = exec.execute(Command{"TTL", {"k5"}}, now);
    ASSERT_TRUE(std::holds_alternative<Integer>(r5));
    EXPECT_EQ(std::get<Integer>(r5).value, 0);
    EXPECT_NE(std::get<Integer>(r5).value, -2);
}

TEST(ExecutorTest, UnknownCommand) {
    HashTable table;
    Executor exec(table);
    int64_t now = 1000;

    auto res = exec.execute(Command{"FOOBAR", {"a", "b"}}, now);
    ASSERT_TRUE(std::holds_alternative<Error>(res));
    EXPECT_EQ(std::get<Error>(res).message, "ERR unknown command 'FOOBAR'");
}

TEST(ExecutorTest, WrongArity) {
    HashTable table;
    Executor exec(table);
    int64_t now = 1000;

    // GET requires 1 arg
    auto r_get0 = exec.execute(Command{"GET", {}}, now);
    ASSERT_TRUE(std::holds_alternative<Error>(r_get0));
    EXPECT_EQ(std::get<Error>(r_get0).message, "ERR wrong number of arguments for 'get' command");

    auto r_get2 = exec.execute(Command{"GET", {"a", "b"}}, now);
    ASSERT_TRUE(std::holds_alternative<Error>(r_get2));
    EXPECT_EQ(std::get<Error>(r_get2).message, "ERR wrong number of arguments for 'get' command");

    // SET requires at least 2 args
    auto r_set1 = exec.execute(Command{"SET", {"k"}}, now);
    ASSERT_TRUE(std::holds_alternative<Error>(r_set1));
    EXPECT_EQ(std::get<Error>(r_set1).message, "ERR wrong number of arguments for 'set' command");

    // DEL requires at least 1 arg
    auto r_del0 = exec.execute(Command{"DEL", {}}, now);
    ASSERT_TRUE(std::holds_alternative<Error>(r_del0));
    EXPECT_EQ(std::get<Error>(r_del0).message, "ERR wrong number of arguments for 'del' command");

    // EXISTS requires at least 1 arg
    auto r_exists0 = exec.execute(Command{"EXISTS", {}}, now);
    ASSERT_TRUE(std::holds_alternative<Error>(r_exists0));
    EXPECT_EQ(std::get<Error>(r_exists0).message, "ERR wrong number of arguments for 'exists' command");

    // EXPIRE requires 2 args
    auto r_exp1 = exec.execute(Command{"EXPIRE", {"k"}}, now);
    ASSERT_TRUE(std::holds_alternative<Error>(r_exp1));
    EXPECT_EQ(std::get<Error>(r_exp1).message, "ERR wrong number of arguments for 'expire' command");

    // TTL requires 1 arg
    auto r_ttl0 = exec.execute(Command{"TTL", {}}, now);
    ASSERT_TRUE(std::holds_alternative<Error>(r_ttl0));
    EXPECT_EQ(std::get<Error>(r_ttl0).message, "ERR wrong number of arguments for 'ttl' command");
}

TEST(ExecutorTest, SetWithExtraArgsSyntaxError) {
    HashTable table;
    Executor exec(table);
    int64_t now = 1000;

    // SET with >2 args returns ERR syntax error (options not implemented in Stage 1)
    auto res_ex = exec.execute(Command{"SET", {"k", "v", "EX", "10"}}, now);
    ASSERT_TRUE(std::holds_alternative<Error>(res_ex));
    EXPECT_EQ(std::get<Error>(res_ex).message, "ERR syntax error");

    auto res_nx = exec.execute(Command{"SET", {"k", "v", "NX"}}, now);
    ASSERT_TRUE(std::holds_alternative<Error>(res_nx));
    EXPECT_EQ(std::get<Error>(res_nx).message, "ERR syntax error");
}

TEST(ExecutorTest, NowMsHelperReturnsPositive) {
    int64_t ms = now_ms();
    EXPECT_GT(ms, 0);
}
