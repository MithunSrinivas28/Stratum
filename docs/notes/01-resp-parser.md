# Note 01 — RESP Wire Protocol Parser

**What:** A streaming, zero-allocation-path RESP parser that transforms raw TCP chunk feeds into strongly-typed, uppercase-normalized Redis commands with binary-safe arguments.

**Why:** Using a full parser-combinator library or relying on naive string splitting fails under TCP fragmentation and introduces needless dynamic allocation overhead. We chose an explicit two-phase finite state machine (FSM) over an internal rolling buffer so we can pause mid-payload on partial TCP reads without dropping connection state or synchronizing across threads.

**How:** `RespParser` buffers incoming bytes and inspects the stream prefix: `*` triggers multibulk array parsing (reading explicit bulk lengths and raw bytes), while any other prefix triggers inline parsing up to a newline. If a TCP read fragments midway through a bulk string or array header, the parser preserves the partial suffix in `buffer_` and yields control until the next `feed()`; any protocol corruption or buffer boundary breach permanently poisons the instance to prevent split-brain command execution.

**Jargon:** 
- *RESP (REdis Serialization Protocol):* A simple, type-prefixed binary-safe request-response protocol used by Redis clients.
- *Multibulk / Array framing:* The standard Redis command framing format prefixed by `*<count>\r\n`, containing explicit length-prefixed bulk strings (`$<len>\r\n<data>\r\n`).
- *Poisoned Parser State:* A terminal failure mode where the parser refuses all subsequent data feeds once corrupt wire bytes or size violations are detected, forcing the network layer to terminate the underlying socket.


# RESP Parser

## What

The RESP parser is the component that converts raw bytes received from a TCP connection into structured Redis commands that Stratum can execute.

Stratum needs to support the Redis wire protocol so that standard clients such as `redis-cli` can communicate with it. For Stage 1, the parser supports the two command formats relevant to the initial implementation:

- **Inline commands** — commands represented as text separated by line endings.
- **Multibulk commands** — the structured RESP format where a command contains a number of bulk-string arguments.

The parser does **not** execute commands or decide what commands mean. Its responsibility ends at recognizing valid protocol input and turning it into a structured representation:

```text
Command {
    name
    arguments[]
}
```

This separation is useful because the parser answers **"What did the client send?"**, while the execution layer answers **"What should Stratum do about it?"**

---

## Why

The main reason the parser needs to maintain state is that **TCP is a byte stream, not a message protocol**.

A call to `recv()` does not necessarily correspond to one Redis command. A single command may be split across several TCP reads:

```text
Read 1:  "SET foo"
Read 2:  " bar\r"
Read 3:  "\n"
```

The parser therefore cannot assume that the bytes provided to one `feed()` call contain a complete command.

The opposite can also happen. One TCP read may contain several commands:

```text
"PING\r\nSET foo bar\r\nGET foo\r\n"
```

The parser must be able to extract all complete commands available in the current input.

This leads to the central design principle:

> **The parser owns incomplete protocol state, while the caller owns completed commands.**

The parser keeps the bytes it still needs to finish parsing. Once a command is complete, ownership of that command moves to the caller.

This makes the parser persistent for the lifetime of a connection rather than creating a new parser for every `recv()` call.

---

## How

There is one persistent `RespParser` associated with each TCP connection.

Conceptually, the data flow is:

```text
TCP connection
      │
      ▼
 raw bytes
      │
      ▼
 RespParser
      │
      ▼
 Command[]
      │
      ▼
 execution layer
```

The parser exposes a conceptual operation:

```text
feed(bytes) → ParseResult
```

A call to `feed()` adds newly received bytes to the parser's internal input state and attempts to extract as many complete commands as possible.

The result contains:

```text
ParseResult {
    commands[]
    status
    error?
}
```

where `status` is one of:

```text
Complete
Incomplete
Error
```

### Complete

The parser successfully extracted every complete command currently available.

There may be one command:

```text
PING
```

or many:

```text
PING
SET foo bar
GET foo
```

All completed commands are returned to the caller.

### Incomplete

The parser successfully extracted any complete commands at the front of the input, but the next command is incomplete.

For example:

```text
SET foo bar\r\n
GET fo
```

The first command is returned immediately, while the incomplete `GET fo` remains inside the parser for the next `feed()` call.

This means `Incomplete` does **not** necessarily mean that zero commands were produced. It means that parsing reached the end of currently available bytes while a valid command was still unfinished.

### Error

The parser encountered input that cannot form a valid RESP command.

Examples include malformed multibulk framing, invalid lengths, or other protocol violations.

The parser does **not** attempt to guess where the next valid command begins.

This is important because after malformed framing, the parser may no longer know where command boundaries are. Trying to "skip bad bytes and continue" could cause later bytes to be interpreted incorrectly.

Once an `Error` occurs, the parser becomes **poisoned**. Future calls to `feed()` also return `Error` rather than attempting to recover.

The connection layer is responsible for closing the connection and discarding the parser.

---

## Parsing Multiple Commands

A single call to `feed()` should extract **all complete commands currently available**, rather than returning only one command.

For example:

```text
feed("PING\r\nGET foo\r\nSET foo bar\r\n")
```

produces:

```text
[
    Command("PING"),
    Command("GET", ["foo"]),
    Command("SET", ["foo", "bar"])
]
```

The parser then has no completed input left to process.

If the input instead ends in the middle of another command:

```text
PING\r\nGET foo\r\nSET foo
```

the parser returns:

```text
commands = [
    PING,
    GET foo
]

status = Incomplete
```

and retains the incomplete `SET foo` internally.

The key invariant is:

> **The parser consumes every complete command at the front of its buffer. If the next command is incomplete, it stops and retains that incomplete suffix.**

This gives deterministic behavior across arbitrary TCP read boundaries.

---

## Command Representation

The parser produces a structured `Command` rather than exposing raw RESP arrays to the rest of the system:

```text
Command {
    name
    arguments[]
}
```

This keeps the execution layer independent of the wire-format details.

For example, both of these inputs represent the same logical command:

```text
SET foo bar
```

The parser turns them into the same conceptual structure:

```text
name = "SET"
arguments = ["foo", "bar"]
```

The parser is therefore responsible for **protocol parsing**, not command semantics.

One important requirement is that bulk-string arguments must be preserved accurately. The parser should not assume that every argument is ordinary human-readable text; RESP bulk strings can contain arbitrary byte data.

---

## Important Edge Cases

### Partial TCP reads

A command may be split at any point, including in the middle of a line, length field, or bulk-string payload.

The parser retains the incomplete bytes and continues when the next `feed()` call arrives.

### Multiple commands in one read

One read can contain many complete commands. The parser extracts all of them rather than stopping after the first.

### Complete command followed by incomplete command

Completed commands are returned immediately, while the incomplete command remains buffered.

### Malformed input

Malformed RESP results in `Error`.

The parser does not attempt protocol resynchronization.

### Error followed by another feed

Once the parser enters the error state, it is poisoned. Further parsing is not attempted.

The connection layer should terminate the connection and discard the parser.

### Buffer-size limit

The parser has a configured maximum amount of buffered input.

If an incomplete command grows beyond this limit without becoming a valid complete command, the parser returns `Error`.

This is treated as a protocol/parser failure rather than a separate parser outcome.

### Connection closes during an incomplete command

If the TCP connection closes while the parser is holding incomplete input, that partial command is discarded. There is no later read from which the parser could finish it.

---

## Design Boundary

The parser should stay focused on one responsibility:

> **Convert a TCP byte stream containing RESP data into complete structured commands.**

It should not be responsible for:

- executing commands;
- maintaining the key-value store;
- deciding command semantics;
- routing commands to shards;
- managing the connection lifecycle.

Those responsibilities belong to later layers of Stratum.

The parser therefore forms a clean boundary:

```text
          Protocol boundary
                 │
TCP bytes ──► RESP Parser ──► Commands
                              │
                              ▼
                         Execution layer
```

This separation will make the parser easier to test independently from the rest of Stratum.

---

## Jargon

**RESP** — Redis Serialization Protocol, the wire protocol used by Redis clients and servers.

**Inline command** — A simple command represented as text with a line ending.

**Multibulk** — The RESP representation where a command is encoded as multiple bulk-string elements.

**TCP byte stream** — TCP provides an ordered stream of bytes but does not preserve application-level message boundaries.

**Parser state** — Information the parser must retain between calls because the currently available bytes are not enough to finish parsing.

**Framing** — Determining where one protocol message ends and the next begins.

**Protocol resynchronization** — Attempting to recover after malformed input by guessing where the next valid message begins.

**Poisoned parser** — A parser that has entered an unrecoverable error state and must no longer be used.

---

## Final Contract

The Stage 1 RESP parser is therefore defined as follows:

1. A persistent parser exists for each TCP connection.
2. The parser owns its incomplete input buffer and parsing state.
3. `feed()` accepts newly received bytes.
4. Each call extracts all complete commands currently available.
5. Completed commands are returned to the caller.
6. Incomplete input remains owned by the parser for the next `feed()`.
7. The parser supports inline and multibulk RESP commands.
8. Commands are represented as `Command { name, arguments[] }`.
9. Bulk-string arguments must preserve their data accurately.
10. `Complete`, `Incomplete`, and `Error` are the only parser outcomes.
11. Malformed input produces `Error`; the parser does not resynchronize.
12. Once `Error` occurs, the parser is poisoned and must not be reused.
13. Exceeding the configured buffer limit produces `Error`.
14. The connection layer is responsible for terminating a connection after parser failure.

The implementation can now be built against this contract without making additional architectural decisions about parser behavior.
