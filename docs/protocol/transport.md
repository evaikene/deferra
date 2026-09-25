# Local transport and JSON-RPC envelopes

`jobud` uses a persistent Unix-domain byte stream. Each message has headers,
one blank CRLF line, and one UTF-8 JSON body. This is JobU's framing profile;
the [JSON-RPC 2.0 specification](https://www.jsonrpc.org/specification) defines
the JSON envelope, and the [LSP base protocol](https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/#baseProtocol)
provides background for the framing. JobU does not implement LSP methods.

## Framing

The required `Content-Length` is the **number of UTF-8 bytes in the body**,
not a character count. End header lines with `\r\n`, then end the header with
another `\r\n`. Header names are matched without regard to ASCII case. One
optional `Content-Type` header is accepted when it is syntactically valid and
has a `charset=utf-8` or `charset=utf8` parameter; its media type is not
otherwise used. Unknown well-formed headers are ignored. Duplicate `Content-Length` or
`Content-Type`, malformed lengths, unsupported charsets, and exceeded
limits fail the connection.

This complete ASCII request has a 59-byte body:

```text
Content-Length: 59\r\n
\r\n
{"jsonrpc":"2.0","id":1,"method":"system.info","params":{}}
```

The displayed `\r\n` sequences stand for CRLF bytes. A sender writes those
bytes, then writes exactly the stated number of body bytes. The next frame may
start immediately afterward. For a body containing non-ASCII characters,
encode it as UTF-8 first and count the encoded bytes.

For example, this request body contains 76 Unicode characters but **77 UTF-8
bytes** because `õ` occupies two bytes, so its header is `Content-Length: 77`:

```json
{"jsonrpc":"2.0","id":2,"method":"queue.get","params":{"queue_name":"Tõnu"}}
```

| Limit | Current default | Meaning |
| --- | ---: | --- |
| Header | 16 KiB | Includes the final `\r\n\r\n` |
| Body | 1 MiB | Request or response JSON bytes |
| Queued framed output | 2 MiB per connection | Unacknowledged outgoing bytes |
| JSON nesting | 64 containers | Parser limit per message |
| Batch | 64 entries | Maximum entries in one JSON array |
| Raw client pending calls | 128 | Maximum simultaneous requests in the library client |

These are the current library defaults used by the daemon/client; library
callers can configure their own options. Result pages may stop before their
item limit to remain within the daemon's response budget. See the relevant
method for its per-field limits.

## Request and response envelopes

For a request that needs a reply, send `jsonrpc`, `id`, `method`, and an object
`params`. `id` is a JSON string or integer; the raw server also accepts null,
but a non-null ID keeps responses distinct from invalid-request errors. A
success echoes that ID
and has `result`; a failure echoes it and has `error`. A response has exactly
one of `result` and `error`. For example:

```json
{"jsonrpc":"2.0","id":1,"method":"system.info","params":{}}
```

```json
{"jsonrpc":"2.0","id":1,"error":{"code":-32601,"message":"Method not found"}}
```

| Envelope member | Type | Required | Since | Meaning |
| --- | --- | --- | --- | --- |
| `jsonrpc` | string | Yes | 1.0 | Fixed value `"2.0"` |
| `id` | string, integer, or null | Request: for a reply; response: yes | 1.0 | Correlates one request and response. Omit it for a notification. Prefer a non-null ID; invalid-request errors may also use `null`. |
| `method` | string | Request: yes | 1.0 | Exact capability name, such as `queue.list` |
| `params` | object | JobU operation: yes | 1.0 | Method-specific fields; `system.info` has no fields and ignores params |
| `result` | any JSON value | Successful response: yes | 1.0 | Method-specific value, including JSON `null` for void operations |
| `error` | object | Failed response: yes | 1.0 | Numeric `code`, safe `message`, and optional `data` |

JobU operations use object params and reject extra request members within
`params`. `system.info` ignores params. A missing `id` makes the message a notification: **no response is
sent**, even for a mutation. Use requests when the result matters.

The stream can carry several frames. Requests may be in flight together;
responses need not arrive in request order. A JSON array is a batch of up to
64 requests or notifications, not a database transaction. Earlier mutations
in a batch may commit before a later entry fails. A connection or deadline
failure after sending a mutation can leave its outcome unknown. Consult the
method's idempotency or reconciliation guidance before retrying.

Malformed framing or an invalid response can close the connection. A normal
represented JobU operation error uses JSON-RPC code `-32000` with safe
`data.category` and `data.code`; see [Errors](errors.md).
