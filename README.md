# Stratum

> A Redis-compatible in-memory cache where compute is disposable and object storage (S3) is the permanent source of truth.

---

### The Problem with Local Memory Tiering

Traditional caches (Redis, Dragonfly) treat the local machine as the center of the universe. When dataset size exceeds physical RAM, Dragonfly's modern answer is SSD Tiering (extending RAM via local NVMe). 

That works if your node never dies. But if the physical host catches fire, your local NVMe dies with it. Absent heavy cluster replication topologies, that data is gone.

**Stratum takes a different stance:**
* RAM is just an ephemeral scratchpad.
* Local NVMe/SSD (when used) is strictly a write-behind acceleration layer.
* The indestructible source of truth lives in S3-compatible object storage (MinIO / AWS S3).
* Compute nodes are completely disposable: kill a node with `kill -9`, bring up an empty container pointed at the same bucket, and keys are indexed in seconds via object manifests and lazily paged in on demand.

```
Client (redis-cli / redis-py / ioredis)
        │
        │ RESP wire protocol over TCP
        ▼
┌──────────────────────────────────────────────┐
│        Connection Layer (Linux epoll)        │
└──────────────────────┬───────────────────────┘
                       │ hash(key) % N shards
         ┌─────────────┼─────────────┐
         ▼             ▼             ▼
   ┌───────────┐ ┌───────────┐ ┌───────────┐
   │  Shard 0  │ │  Shard 1  │ │ Shard N-1 │  (1 thread per CPU core,
   │ (Hashmap) │ │ (Hashmap) │ │ (Hashmap) │   zero cross-shard lock contention)
   └─────┬─────┘ └─────┬─────┘ └─────┬─────┘
         │             │             │
         └─────────────┼─────────────┘
                       │ async background flush / compaction
                       ▼
         ┌───────────────────────────┐
         │ S3-Compatible Object Store│  (Durable cold segments + manifests)
         └───────────────────────────┘
```

---

## Technical Deep-Dive: Stage 1 — The RESP Parser

Before we do multi-threading or tiering, we need an uncompromising wire protocol parser that can speak to unmodified `redis-cli` instances and client drivers.

Redis clients communicate using **RESP (REdis Serialization Protocol)**. Stratum implements a persistent, per-connection finite state machine (`RespParser`) in C++17 with bounded buffers and poison semantics.

### Wire Format Breakdown

The parser handles two entry modes on raw TCP streams:

#### 1. Multibulk (Unified Request Format)
Every standard Redis driver sends commands as arrays of length-prefixed bulk strings. Because lengths are explicit, payloads are 100% binary-safe and can contain embedded `\r\n`, null bytes (`\0`), or arbitrary serialization blobs (Protobuf, MessagePack).

```
*3\r\n           --> Array of 3 elements
$3\r\nSET\r\n    --> Element 1: bulk string of len 3 ("SET")
$4\r\nuser\r\n   --> Element 2: bulk string of len 4 ("user")
$5\r\nhello\r\n  --> Element 3: bulk string of len 5 ("hello")
```

#### 2. Inline Commands
Sent by human operators via `telnet` or `nc` (e.g. `PING\r\n` or `GET mykey\r\n`). Arguments are split on whitespace and bounded tightly to prevent memory exhaustion attacks.

---

### Parser Internals & State Machine

```
              ┌──────────────┐
              │  feed(data)  │
              └──────┬───────┘
                     │
             [ Is Poisoned? ] ──(Yes)──> Return Error (No-op)
                     │ (No)
          [ Append to buffer_ ]
                     │
          [ Exceeds 64MB cap? ] ──(Yes)──> Poison Parser -> Return Error
                     │ (No)
                     ▼
          ┌─────────────────────┐
          │     Parse Loop      │<────────────────────────┐
          └──────────┬──────────┘                         │
                     │                                    │
           [ Current State? ]                             │
             /            \                               │
      (Initial)       (MultibulkArgs)                     │
         /                    \                           │
   [ Prefix == '*' ? ]    [ Parse '$<len>\r\n<data>' ]    │
      /         \              │                          │
    (Yes)       (No)     [ Incomplete data? ] ──(Yes)──> Yield
     /            \            │ (No)                     │
[ Multibulk ]   [ Inline ]     ▼                          │
  Header        Line       All args collected?            │
    │             │            │                          │
    └──────┬──────┘            ├──(No)──> Loop next arg   │
           │                   │                          │
           ▼                   └──(Yes)─> Emit Command ───┘
   [ Incomplete? ] ──(Yes)──> Yield
```

#### Key Invariants

1. **Partial TCP Frame Resilience:** TCP does not guarantee frame boundaries. If a payload of 10 MB arrives in 4 KB packets, the parser pauses gracefully when bytes run out, yields `ParseStatus::Incomplete`, and resumes on subsequent feeds without losing its position.
2. **Uppercase Normalization:** `Command.name` is always canonicalized to uppercase (`get` $\rightarrow$ `GET`). Command routing lookups can use direct hashing without case-folding branches in hot paths.
3. **Two-Tier Buffer Limits:**
   - **Inline Limit (`64 KB`):** Unterminated inline lines are hard-capped at 64 KB (matching Redis's `PROTO_INLINE_MAX_SIZE`), preventing malicious clients from buffering unbounded memory.
   - **Total Buffer Limit (`64 MB`):** Accommodates legitimate large bulk strings while capping runaway allocations per connection.
4. **Poisoning on Malformed Data:** If a frame contains corrupt lengths (`*abc\r\n` or negative array counts), the parser transitions to an irrecoverable `Error` state. Any subsequent `feed()` call on that instance returns `Error` immediately, signaling the connection manager to close the socket.

---

## Project Structure

```
Stratum/
├── CMakeLists.txt              # Top-level build configuration (C++17, GTest)
├── project.md                  # System design bible & technical specification
├── .gitignore
├── src/
│   ├── CMakeLists.txt
│   └── resp/
│       ├── CMakeLists.txt      # stratum_resp static library target
│       ├── resp_parser.h       # Public parser API & Command data structures
│       └── resp_parser.cpp     # FSM implementation
├── tests/
│   ├── CMakeLists.txt          # stratum_resp_tests runner
│   └── resp_parser_test.cpp    # 26 unit tests covering all edge cases
└── docs/
    └── notes/
        └── 01-resp-parser.md   # Concept note
```

---

## Building & Running Tests

Stratum targets modern Linux kernels (`epoll`, `io_uring` for later stages). We recommend building inside **WSL2 (Ubuntu 22.04+)** or a native Linux environment.

### Prerequisites

```bash
sudo apt update
sudo apt install -y build-essential cmake git
```

### Build & Run Test Suite

GoogleTest is automatically fetched and built via CMake `FetchContent`:

```bash
# Clone the repository
git clone https://github.com/MithunSrinivas28/Stratum.git
cd Stratum

# Configure & build
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build . -j$(nproc)

# Run tests
ctest --output-on-failure
```

Or execute the test binary directly with full verbose output:

```bash
./tests/stratum_resp_tests
```

---

## Test Matrix (26 Scenarios)

| Category | Tests | Scenarios Covered |
| :--- | :--- | :--- |
| **Inline Protocol** | `InlineSimpleCommand`<br>`InlineWithArgs`<br>`InlineEmpty`<br>`InlineNameUppercased`<br>`InlineBareNewline` | Basic single line, space-delimited arguments, empty `\r\n` no-op, lowercase canonicalization, bare `\n` tolerance. |
| **Multibulk Protocol** | `MultibulkSimple`<br>`MultibulkWithArgs`<br>`MultibulkNameUppercased`<br>`MultibulkBinarySafe`<br>`MultibulkEmptyBulkString` | Array header framing, multiple bulk elements, binary-safe embedded nulls/newlines (`\0`, `\r\n`), empty `$0\r\n\r\n` strings. |
| **TCP Stream Fragmentation** | `PartialInlineRead`<br>`PartialMultibulkHeader`<br>`PartialMultibulkBulkData`<br>`PartialMultibulkAcrossThreeFeeds` | Feeds sliced mid-header, mid-length, and mid-payload across 2 to 3 separate chunk deliveries. |
| **Pipelining / Multi-Command** | `MultipleInlineOneFeed`<br>`MultipleMultibulkOneFeed`<br>`MixedInlineAndMultibulk`<br>`CompleteFollowedByIncomplete` | Multiple requests in one feed, mixed formats in same connection stream, complete command emitted while retaining incomplete tail. |
| **Protocol Validation & Errors** | `MalformedMultibulkBadCount`<br>`MalformedMultibulkNegativeCount`<br>`MalformedMultibulkBadBulkLen`<br>`MalformedMultibulkMissingDollar` | Non-integer counts, negative arrays, non-integer lengths, unexpected token markers. |
| **Poison State Verification** | `FeedAfterError`<br>`FeedAfterErrorPreservesPoison` | Ensures any feed following an error refuses execution and stays poisoned. |
| **Memory Bound Protections** | `InlineMaxLengthExceeded`<br>`OverallMaxBufferExceeded` | Verification of 64 KB inline limit and 64 MB total buffer threshold enforcement. |

---

## Roadmap

- [x] **Stage 1A: RESP Protocol Parser** — Streaming FSM, multibulk, inline, binary-safe bulk strings, poisoned state.
- [ ] **Stage 1B: Core Storage Engine** — In-memory hashmap with TTL expiration, `PING`, `SET`, `GET`, `DEL`, `EXISTS`, `EXPIRE`, `TTL`.
- [ ] **Stage 2: Shard-per-Core Concurrency** — Non-blocking `epoll` connection layer, hash partitioning (`hash(key) % N`), local write-behind WAL.
- [ ] **Stage 3: Object Storage Tiering** — S3/MinIO background segment flusher, distributed manifest tracking, cold-start recovery demo.
- [ ] **Stage 4: Verification & Benchmarks** — Crash-recovery fault injection, head-to-head performance benchmarks against Redis and Dragonfly.
