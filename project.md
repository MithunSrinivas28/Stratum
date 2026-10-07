# Stratum — The Full Picture

## Why this document exists

This is not a sprint ticket or a "v1 scope" restriction list. It's the complete context for Stratum — what it is, why it exists, how every piece fits together, and how it gets built. Read this once and hold the whole picture. Actual implementation work will still come as narrow, scoped prompts for one piece at a time — this document is the "why," those prompts are the "what, right now." If a prompt is ever ambiguous, check it against this document and ask a clarifying question rather than guessing or inventing scope.

## What Stratum is, in one paragraph

Stratum is an in-memory key-value store that speaks the Redis wire protocol (RESP), so real Redis clients work against it unmodified. Internally it's multi-threaded, one thread per CPU core, each owning a slice of the keyspace. What makes it different from Redis or Dragonfly: the durable copy of every piece of data lives in S3-compatible object storage, not on the node's local disk. Local disk, where used at all, is a write-behind cache — never the only copy of anything durable. The result: compute nodes are fully disposable. Kill one, bring up a fresh one pointed at the same bucket, and the data is still there.

## The mental model

Think of a cache as a fast but forgetful assistant at a small desk, answering from sticky notes in front of them — that's Redis or Dragonfly, fast because everything lives in reach. Dragonfly's answer to "the desk is too small" is a filing cabinet next to the same desk (local SSD) — more room, but if the office burns down, the cabinet burns with it.

Stratum's answer: keep the permanent copy of everything in an indestructible warehouse across town (S3). The desk still holds whatever's being used right now, so recent lookups are just as fast. Anything idle gets quietly mailed to the warehouse, with an index card nearby noting exactly where. If the desk burns down, bring in a brand-new assistant at a brand-new desk — they personally remember nothing, but they read the index card, and within seconds know what exists and where to fetch it as it's asked for. Nothing was ever lost, because nothing durable ever lived only on that desk.

Several assistants (shard-per-core) split the keyspace by hash so nobody waits in line behind anybody else.

## Why this exists — the landscape, honestly

Dragonfly took SSD Data Tiering to general availability on June 1, 2026 — RAM extended by local NVMe, roughly 8x more data per node, benchmarked against AWS ElastiCache's own tiering. That space is claimed: funded team, already in production. What it doesn't do is separate compute from durable storage — the cold tier lives on the same box as the process, so losing the node risks losing that node's disk too, absent separate replication.

Smaller prior art exists too: a Python client library (RediS3) that skips a server entirely and writes each key straight to S3, and a weekend-scale experiment benchmarking redis-like primitives against AWS's S3 Express tier for cost comparison. Both are real, both are small — a client-side library and a proof-of-concept, not a RESP-wire-protocol server with shard-per-core threading, a local write-behind cache, and a proven crash-recovery story. That gap — a real server, done properly, with correctness proven rather than claimed — is what Stratum is building into.

## Architecture — the whole system

Single process, N worker threads (N = core count):

```
Client (redis-cli, any Redis client library)
        |  RESP over TCP
        v
 +-------------------------------+
 |   Connection layer (epoll)    |
 +---------------+---------------+
                 | route by hash(key) % N
   +-------------+-------------+
   v             v             v
 Shard 0       Shard 1  ...  Shard N-1
 (own thread, own in-memory hash table,
  own local write-behind log)
   |             |             |
   +-------------+-------------+
                 | background flush of cold keys
                 v
         S3-compatible bucket
    (durable values + a manifest/index)
```

- **Connection layer:** epoll event loop accepts connections, parses RESP, routes each command to the owning shard thread by `hash(key) % N`.
- **Shard:** in-memory hash table plus a local append-only log (write-behind — accelerates reads/writes, is not the source of truth).
- **Tiering thread(s):** periodically flush cold (not-recently-accessed) key data to S3, and keep a manifest in S3 mapping keys to their object location.
- **Recovery:** on startup, a node reads the manifest from S3 to rebuild its in-memory index. Actual values are pulled back lazily, on first access — no full rehydration needed before the node serves traffic.

## The full build arc — one system, not four separate projects

Everything below is the same system growing in stages. Later phases are not separate efforts and nothing earlier gets thrown away.

- **Stage 1 — protocol + core:** RESP parser (inline and multibulk), single-threaded hash table, PING/SET/GET/DEL/EXISTS/EXPIRE/TTL, working against an unmodified `redis-cli`.
- **Stage 2 — concurrency:** shard-per-core threading, local write-behind log, crash-safe restart from the local log (no S3 involved yet), published single-thread vs. multi-thread benchmark numbers.
- **Stage 3 — the actual pitch:** S3/MinIO integration, background flush of cold segments, manifest in S3, cold-start recovery proven by deleting local disk entirely and restarting against the same bucket.
- **Stage 4 — proof and launch:** fault-injection suite (kill mid-write, simulate S3 errors/latency, verify no silent data loss), head-to-head benchmarks vs. real Redis and Dragonfly, README with the architecture diagram and the crash-recovery demo, LICENSE, CONTRIBUTING, CI, launch.

## Non-goals — deliberately out of scope

- No AI/ML/LLM component anywhere in the system
- No full SQL query engine
- No multi-node clustering or distributed consensus — the disposable-node story works on a single node; clustering is future work, not this build
- Not trying to out-throughput Dragonfly on pure in-RAM operations — the differentiator is the storage architecture, not raw RAM-path speed
- No Lua scripting, pub/sub, or transactions (MULTI/EXEC)

## How this gets built — the working agreement

- Every piece is designed before any code is written. Design reasoning happens with Claude (architecture tradeoffs) and ChatGPT (system design and CS fundamentals). Antigravity is not asked to invent architecture on its own — it implements against decisions that have already been made.
- Prompts to Antigravity are narrow and scoped to one piece at a time, with the interface and approach already decided.
- After a piece is implemented, a note gets written before moving on — see below. If asked to help with this, Antigravity's job is to draft a first-pass plain-language explanation of what it just built, which then gets rewritten in Mithun's own words — not to produce the final note itself.
- If a request is ambiguous against this document, ask rather than guess.

## The notes practice

One note per concept, same four fields every time:

- **What:** one line, what this piece does
- **Why:** the alternative that wasn't picked, and the real reason
- **How:** the actual mechanism, 2-3 sentences
- **Jargon:** any new term, one plain-sentence definition

Kept as a running file in the repo, next to the code — not written from memory after the fact.

## Suggested repo structure

```
stratum/
  src/
    resp/        -- protocol parser
    engine/      -- in-memory shard storage
    tiering/     -- S3 flush/fetch, manifest
    net/         -- connection layer, epoll
  tests/
  benchmarks/
  docs/
    notes/       -- one file per concept: what / why / how / jargon
  README.md
  LICENSE
  CMakeLists.txt
```

## What success looks like

- Passes standard `redis-cli` command tests unmodified
- Sustains 10,000 concurrent connections in a load test, with real measured numbers published, not estimates
- A recorded demo: kill -9 the process, bring up a fresh node against the same bucket, GET still returns the value
- A fault-injection suite that passes, and a written consistency model that actually matches what the tests show
