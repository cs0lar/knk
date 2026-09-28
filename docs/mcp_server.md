# MCP Server

`mcp_server` is a local [MCP](https://modelcontextprotocol.io/) server exposing every public
`KnowledgeKernel` operation as an individually-schema'd tool, over stdio. It is the kernel's first
caller-facing boundary beyond linking `libkernel.a` directly from C++: an agent, a CRM backend, or any
other MCP-speaking process can now commit and query the kernel without being a C++ process itself.

This is deliberately **not** a network API. There is no listening socket, no request routing, no
auth/TLS surface — `mcp_server` is a subprocess speaking newline-delimited JSON-RPC 2.0 on its own
stdin/stdout, the same shape MCP defines for local tool servers. AGENTS.md's "Do Not Do Yet" list still
forbids an HTTP/network-facing API; see that section for the exact distinction. The implementation
lives in `include/kernel/mcp_tools.hpp` / `src/mcp_tools.cpp` (the tool registry and dispatcher, unit
tested in `tests/mcp_tools_tests.cpp`) and `mcp/main.cpp` (the thin stdio loop on top of it).

## Building and running

```bash
cmake --build build --target mcp_server
./build/mcp_server <storage-root>
```

`<storage-root>` is the same kind of path you'd pass to `StorageConfig` anywhere else in the
kernel — a directory holding `assertions.log`, the index/catalog/provenance logs, etc. It is created
on first use and reopened (with full replay/recovery) on every subsequent run, exactly like any other
`KnowledgeKernel` instance.

The server reads one JSON-RPC 2.0 message per line from stdin and writes one per line to stdout.
**stdout is reserved entirely for protocol messages** — anything diagnostic goes to stderr, since a
stray stdout write would corrupt the stream for whatever process has this as a subprocess. This means
you generally don't run it interactively by hand; the examples below pipe requests in via a shell
one-liner, which works for testing but isn't how a real MCP client talks to it.

## Protocol support

* `initialize` — returns `protocolVersion`, `capabilities.tools`, and `serverInfo`.
* `notifications/initialized`, `notifications/cancelled` — accepted, no response (both are JSON-RPC
  notifications; per spec, notifications never get a response).
* `tools/list` — returns every tool below with its JSON Schema `inputSchema`.
* `tools/call` — `params: {"name": ..., "arguments": {...}}`; dispatches to `KnowledgeKernel::execute`
  and returns `{"content": [{"type": "text", "text": "<json>"}], "isError": bool}`. A tool-level
  failure (unknown tool name, a missing/malformed argument, or an exception from `execute()` itself —
  e.g. `commit_retraction` against an unknown id) comes back as a **successful** JSON-RPC response with
  `isError: true` and the error message in `content[0].text`, per MCP convention — not a JSON-RPC-level
  error. Malformed requests, unknown methods, and JSON parse failures do use JSON-RPC error codes
  (`-32700` parse error, `-32600` invalid request, `-32601` method not found, `-32602` invalid params).

## Tools

One tool per `KnowledgeKernel` method, 48 total — the exact set `KernelCommand` reifies (see
`include/kernel/kernel_command.hpp`). Argument and return types follow the method signatures directly:
`EntityId`/`PredicateId`/`AssertionId` are JSON integers, `Timestamp` is a JSON integer (Unix seconds),
`confidence` is a JSON number, raw bytes (`intern_document`'s `content`, `document_content`'s return
value) are base64-encoded strings, and a `Value` (`intern_value`/`find_value`) is
`{"kind": "text"|"int64"|"double"|"bool"|"timestamp", "value": <matching JSON type>}`. Call `tools/list`
against a running server for the full JSON Schema of each.

### Mutating

| Tool | Description |
|---|---|
| `commit` | Commits a new active assertion. |
| `commit_by_name` | Commits a new active assertion from names/literals instead of ids, interning subject, predicate, and object as needed (idempotent). |
| `commit_batch` | Commits many new active assertions in one call under a single durability boundary. At most 10,000 entries, each with its own valid time; returns the new AssertionIds in input order. Not atomic — a crash mid-batch commits a prefix, never a gap. |
| `commit_batch_by_name` | Commits many new active assertions in one call from names/literals instead of ids, interning each entry's subject, predicate, and object as needed (idempotent). Same batch semantics as `commit_batch`. |
| `commit_retraction` | Commits a retraction record for an existing assertion. |
| `commit_superseding` | Commits a replacement assertion, marking the superseded one as such. |
| `write_snapshot` | Persists a full snapshot of current in-memory assertions. |
| `intern_entity` | Interns a named entity, returning its EntityId (idempotent). |
| `intern_value` | Interns a typed literal value, returning its EntityId (idempotent). |
| `intern_predicate` | Interns a named predicate, returning its PredicateId (idempotent). |
| `intern_document` | Interns raw document bytes, returning an EntityId. |
| `record_provenance` | Records which source produced a given assertion, and by what method. |
| `record_provenance_batch` | Records provenance for many assertions in one call under a single durability boundary. At most 10,000 records; every target is validated first, so one unknown id rejects the whole call without writing anything. |
| `commit_hypothesis` | Commits a labeled, machine-suggested (Hypothesis-status) assertion. |
| `merge_entities` | Merges absorb into keep: a one-way, append-only redirect. |
| `archive_segments_before` | Archives (compacts, does not delete) log segments entirely before an AssertionId. |

### Query

| Tool | Description |
|---|---|
| `get` | Fetches a single assertion by id. |
| `assertions_for_subject` | Returns every recorded assertion (any status) for a subject. Optional `limit` caps the result count (0/omitted = no cap). |
| `current` | Returns every currently active, open-ended assertion for a subject. |
| `current_by_name` | Returns every currently active, open-ended assertion for a subject looked up by name; empty (not an error) if the name was never interned. |
| `current_by_object` | Reverse-direction lookup: who currently has the given entity as object. |
| `current_by_predicate` | Kernel-wide lookup: every currently active assertion for a predicate, any subject. |
| `valid_at` | Returns assertions valid at a given point in valid time. |
| `known_at` | Returns assertions observed by a given point in observed time and still currently Active. |
| `valid_at_known_at` | Combines valid_at and known_at cutoffs. |
| `valid_time_timeline` | Returns active assertions for a subject/predicate sorted by valid_from. |
| `observed_time_timeline` | Returns active assertions for a subject/predicate sorted by observed_at. |
| `commit_history` | Returns every recorded assertion (any status) for a subject/predicate, in commit order. Optional `limit` caps the result count (0/omitted = no cap). |
| `changes_since` | Kernel-wide, status-agnostic: every assertion observed at or after a cutoff, any subject or predicate. Optional `limit` caps the result count and optional `newest_first` reverses the order (most-recently-observed first) — combine both to fetch just the latest change(s) without reading the whole log. |
| `explain` | Walks the supersession/retraction chain from an assertion back to its root. |
| `find_conflicts` | Finds overlapping active assertions for a subject/predicate with different objects. |
| `find_entity` | Looks up a previously interned entity's id by name. |
| `find_value` | Looks up a previously interned literal value's id. |
| `find_predicate` | Looks up a previously interned predicate's id by name. |
| `entity_name` | Resolves a previously interned entity's name. |
| `entity_value` | Resolves a previously interned entity's literal value. |
| `predicate_name` | Resolves a previously interned predicate's name. |
| `document_content` | Fetches previously interned document bytes. |
| `provenance_for` | Resolves recorded provenance for an assertion. |
| `entity_name_batch` | Resolves many entities' names in one call (`ids`). Answers in input order, one slot per id, each exactly what `entity_name` answers — `null` included. At most 10,000 ids. |
| `entity_value_batch` | Resolves many entities' literal values in one call (`ids`), with the same one-slot-per-id contract as `entity_name_batch`. |
| `predicate_name_batch` | Resolves many predicates' names in one call (`ids`), with the same one-slot-per-id contract. |
| `provenance_for_batch` | Resolves recorded provenance for many assertions in one call (`assertion_ids`), with the same one-slot-per-id contract — `null` where nothing was recorded. |
| `hypotheses_for` | Returns open (Hypothesis-status) predictions for a subject. |
| `neighbors` | Bounded breadth-first traversal of current-edge neighbors, both directions. |
| `co_occurring_predicates` | Currently active predicates for a subject. |
| `resolve_entity` | Resolves an id through recorded merge redirects to its canonical id. |
| `query` | Runs a shaped read against the query IR: subject/predicate/object, a valid-time point, an observed-time window, open-endedness, an explicit status set, a `filter` tree (ordered comparisons, comparisons against the object's value, and and/or/not up to 8 deep), ordering and paging — any combination, all arguments optional. `resolve_names` adds a parallel `names` array so rendering needs no second round trip. Returns `{assertions, truncated}` plus `names` when asked. Capped at 10,000 rows; an unknown `ir_version` or a malformed filter is rejected. |

## Example session

```bash
{
  echo '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"example-client","version":"0.1.0"}}}'
  echo '{"jsonrpc":"2.0","method":"notifications/initialized"}'
  echo '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"intern_entity","arguments":{"name":"Alice"}}}'
  echo '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"intern_entity","arguments":{"name":"Acme"}}}'
  echo '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"intern_predicate","arguments":{"name":"works_at"}}}'
  echo '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"commit","arguments":{"subject":1,"predicate":1,"object":2,"valid_from":1704067200,"valid_to":0,"observed_at":1719792000,"confidence":0.9}}}'
  echo '{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"current_by_object","arguments":{"object":2}}}'
} | ./build/mcp_server /tmp/knk_example
```

`initialize`:

```json
{"id":1,"jsonrpc":"2.0","result":{"capabilities":{"tools":{}},"protocolVersion":"2025-06-18","serverInfo":{"name":"knk-mcp-server","version":"0.1.0"}}}
```

`intern_entity("Alice")`, `intern_entity("Acme")`, `intern_predicate("works_at")` — each returns the
next id (1, 2, 1 — entities and predicates are separate id spaces):

```json
{"id":2,"jsonrpc":"2.0","result":{"content":[{"text":"1","type":"text"}],"isError":false}}
{"id":3,"jsonrpc":"2.0","result":{"content":[{"text":"2","type":"text"}],"isError":false}}
{"id":4,"jsonrpc":"2.0","result":{"content":[{"text":"1","type":"text"}],"isError":false}}
```

`commit(Alice, works_at, Acme, ...)` returns the new AssertionId:

```json
{"id":5,"jsonrpc":"2.0","result":{"content":[{"text":"1","type":"text"}],"isError":false}}
```

`current_by_object(Acme)` — "who currently works at Acme" — returns the committed assertion:

```json
{"id":6,"jsonrpc":"2.0","result":{"content":[{"text":"[{\"confidence\":0.9,\"id\":1,\"object\":2,\"observed_at\":1719792000,\"predicate\":1,\"retracts_id\":0,\"status\":\"Active\",\"subject\":1,\"supersedes_id\":0,\"valid_from\":1704067200,\"valid_to\":0}]","type":"text"}],"isError":false}}
```

A tool-level error looks the same shape, with `isError: true`:

```json
{"id":7,"jsonrpc":"2.0","result":{"content":[{"text":"error: invalid retraction target","type":"text"}],"isError":true}}
```

## Connecting a real MCP client

Point any stdio-based MCP client at the built binary with the storage root as its one argument. For a
client that reads a JSON server config (the shape Claude Desktop and several other MCP clients use):

```json
{
  "mcpServers": {
    "knowledge-kernel": {
      "command": "/absolute/path/to/knk/build/mcp_server",
      "args": ["/absolute/path/to/a/storage/directory"]
    }
  }
}
```

Use an absolute path for both the binary and the storage root — the server has no notion of a working
directory beyond what it's given, and a relative path would resolve against wherever the client happens
to launch it from.
