# Note 01 — RESP Wire Protocol Parser

**What:** A streaming, zero-allocation-path RESP parser that transforms raw TCP chunk feeds into strongly-typed, uppercase-normalized Redis commands with binary-safe arguments.

**Why:** Using a full parser-combinator library or relying on naive string splitting fails under TCP fragmentation and introduces needless dynamic allocation overhead. We chose an explicit two-phase finite state machine (FSM) over an internal rolling buffer so we can pause mid-payload on partial TCP reads without dropping connection state or synchronizing across threads.

**How:** `RespParser` buffers incoming bytes and inspects the stream prefix: `*` triggers multibulk array parsing (reading explicit bulk lengths and raw bytes), while any other prefix triggers inline parsing up to a newline. If a TCP read fragments midway through a bulk string or array header, the parser preserves the partial suffix in `buffer_` and yields control until the next `feed()`; any protocol corruption or buffer boundary breach permanently poisons the instance to prevent split-brain command execution.

**Jargon:** 
- *RESP (REdis Serialization Protocol):* A simple, type-prefixed binary-safe request-response protocol used by Redis clients.
- *Multibulk / Array framing:* The standard Redis command framing format prefixed by `*<count>\r\n`, containing explicit length-prefixed bulk strings (`$<len>\r\n<data>\r\n`).
- *Poisoned Parser State:* A terminal failure mode where the parser refuses all subsequent data feeds once corrupt wire bytes or size violations are detected, forcing the network layer to terminate the underlying socket.
