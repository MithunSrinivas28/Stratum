# Note 02 — Storage Core, Executor, and RESP Encoder

> **DRAFT — to be rewritten by Mithun in his own words**

**What:** A single-threaded in-memory key-value storage engine (`HashTable`) supporting lazy TTL expiration, an `Executor` executing core Redis commands (`PING`, `SET`, `GET`, `DEL`, `EXISTS`, `EXPIRE`, `TTL`) into protocol-independent semantic results (`Result`), and a RESP wire encoder converting semantic results to RESP protocol bytes.

**Why:** 
- *Embedded expiry vs. separate expiry table:* A separate expiry index (e.g., min-heap or secondary hash table) adds write amplification, extra pointer chasing, and dual-structure synchronization on every SET/DEL. Embedding `expires_at` directly in `Entry` keeps lookups $O(1)$ and cache-local, perfectly aligned with future per-shard memory layouts.
- *Lazy vs. active expiry:* Background timer threads or active sampling loops introduce locking overhead, context switches, and non-deterministic behavior during testing. Lazy expiration evicts dead keys on read access with zero extra threads and zero locks. *Known limitations:* Lazy-only expiry never frees expired keys that are never read again, which will require active randomized sampling in later stages to bound memory growth.
- *Wall-clock vs. monotonic clock:* Monotonic clocks (`CLOCK_MONOTONIC`) are ideal for measuring intervals, but expire deadlines in Redis clients and persistence engines (snapshots, S3 manifests, write-ahead logs) require absolute Unix epoch timestamps (`CLOCK_REALTIME`). Every storage and executor method accepts an explicit `now_ms` parameter so time is fully deterministic and testable without sleeping.
- *`std::unordered_map` vs. custom hash table:* Stage 1 focuses on protocol compliance, semantic separation, and correct expiration behavior. Using standard library associative containers isolates logic bugs from table-rehash bugs; custom cache-conscious bucket arrays are deferred to shard performance optimization stages.

**How:** 
- `HashTable` holds `std::unordered_map<std::string, Entry>` where each `Entry` stores `{value, expires_at}` with a `NO_EXPIRY = -1` sentinel. Accessors (`get`, `remove`, `exists`, `set_expiry`, `get_ttl`) receive `now_ms` from the caller and prune entries when `now_ms >= expires_at`.
- `Executor` dispatches uppercase `Command` structs from the parser, validates arity and numeric boundaries (including 64-bit integer overflow protection for `EXPIRE`), and returns a variant `Result` (`SimpleString`, `Error`, `Integer`, `BulkString`, `NullBulk`).
- `resp::encode` is a set of pure serialization functions that transform `Result` variants into valid RESP bytes (`+OK\r\n`, `-ERR ...\r\n`, `:1\r\n`, `$len\r\nbytes\r\n`, `$-1\r\n`).

**Jargon:** 
- *Lazy Expiration:* Deferring the eviction of an expired key until that key is actively accessed by a read, write, or existence check.
- *Absolute Expiration Timestamp:* A specific Unix epoch wall-clock deadline in milliseconds after which an entry is considered logically dead.
- *Wall-clock vs. Monotonic Clock:* Wall-clock time maps to real-world calendar time (affected by NTP shifts, system clock changes), whereas monotonic time only increases steadily and never jumps backwards (used for elapsed durations).
- *Sentinel Value:* A dedicated in-band marker (here, `NO_EXPIRY = -1`) used to denote special states like infinite persistence without allocating optional wrapper types.
- *Semantic Result:* A strongly-typed, protocol-agnostic intermediate representation of command execution outcomes, decoupling engine execution from wire encoding.
- *TTL (Time To Live):* The remaining lifespan of a key before expiration, reported in seconds rounded to the nearest integer.
- *Shard Ownership:* An architectural model where a single dedicated thread exclusively owns and mutates a slice of the keyspace, eliminating thread synchronization and mutex overhead.
