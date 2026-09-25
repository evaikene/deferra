# Secret administration methods

Secrets are named byte sequences that can be set, rotated, listed, and deleted
through the daemon. See [Named secrets](../../secrets.md) for the details.

The daemon advertises these methods:

| Method | Description | Since | CLI | C++ client |
| --- | --- | --- | --- | --- |
| `secret.set` | Set or rotate a secret | 1.2 | `secret set` | `set_secret()` |
| `secret.list` | List secret metadata | 1.2 | `secret list` | `list_secrets()` |
| `secret.delete` | Delete a secret | 1.2 | `secret delete` | `delete_secret()` |

All methods require object params. Only names and timestamps are returned; there
is no secret-value read method. Secret names use lowercase dot-separated segments,
are at most 128 bytes, and are not normalized. Each segment begins with `a`–`z`
and continues with lowercase letters, digits, or `_`.

## Set or rotate (`secret.set`)

Set or rotate a secret. The request envelope follows
[the transport guide](../transport.md#request-and-response-envelopes):

| Member | Type | Required | Since | Description |
| --- | --- | --- | --- | --- |
| `method` | string | Yes | 1.2 | Fixed string `secret.set` |
| `params` | object | Yes | 1.2 | Fields for the secret to set or rotate |

Params object MUST contain the following members:

| Member | Type | Required | Since | Description |
| --- | --- | --- | --- | --- |
| `name` | string | Yes | 1.2 | Canonical name of the secret |
| `value` | object | Yes | 1.2 | Encoded bytes to store |

The `value` object MUST contain the following members:

| Member | Type | Required | Since | Description |
| --- | --- | --- | --- | --- |
| `encoding` | string | Yes | 1.2 | `utf8` or `base64` |
| `data` | string | Yes | 1.2 | Encoded secret value |

The `base64` form uses strict RFC 4648 padded base64, with no whitespace or
alternate encodings of unused trailing bits.

Decoded values may contain 0–65,536 bytes. A zero-length value is valid.

The result of a successful set is metadata only, containing the following members:

| Member | Type | Since | Description |
| --- | --- | --- | --- |
| `name` | string | 1.2 | Canonical name of the secret |
| `created_at` | UTC time | 1.2 | Original creation time |
| `updated_at` | UTC time | 1.2 | Last set time |

Setting an existing name atomically replaces its bytes, preserves `created_at`,
and updates `updated_at`. Each accepted set commits before its reply and requests
a later scheduler rescan. It does not change already prepared attempts; later
attempts resolve the current value. A lost reply can leave the outcome unknown.
A repeated set is permitted, but may advance `updated_at`; inspect metadata
through `secret.list` before deciding to retry.

For example, this complete JSON-RPC request sets a UTF-8 secret named
`reports.token` to the fictitious value `example-token`:

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "secret.set",
  "params": {
    "name": "reports.token",
    "value": {
      "encoding": "utf8",
      "data": "example-token"
    }
  }
}
```

The result is metadata only:

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "name": "reports.token",
    "created_at": "2026-01-01T00:00:00.000000Z",
    "updated_at": "2026-01-01T00:00:00.000000Z"
  }
}
```

Use base64 for binary bytes. For example, `AP8=` is the two bytes `00 ff`:

```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "method": "secret.set",
  "params": {
    "name": "reports.binary",
    "value": {
      "encoding": "base64",
      "data": "AP8="
    }
  }
}
```

## List metadata (`secret.list`)

| Params member | Type | Required | Default | Since | Description |
| --- | --- | --- | --- | --- | --- |
| `limit` | integer | No | 100 | 1.2 | 1–200 metadata entries |
| `after_name` | canonical name | No | Beginning | 1.2 | Exclusive name boundary |

| Result member | Type | Since | Description |
| --- | --- | --- | --- |
| `items` | array of metadata objects | 1.2 | At most `limit` names and timestamps |
| `next_after_name` | string or null | 1.2 | Boundary for another page, or null at the end |

Params may contain `limit` and `after_name` only. `limit` defaults to 100 and
must be an integer from 1 through 200. `after_name` is an optional canonical
name and is an exclusive boundary; it need not still exist. A JSON null is not
an omitted boundary. Results use ascending name order and contain only
`items` and nullable `next_after_name`:

```json
{"limit":1}
```

```json
{"items":[{"name":"reports.token","created_at":"2026-01-01T00:00:00.000000Z","updated_at":"2026-01-01T00:00:00.000000Z"}],"next_after_name":"reports.token"}
```

Continue with `{"limit":1,"after_name":"reports.token"}`. An empty or final
page has `"next_after_name":null`. Pagination is a live view; concurrent sets
and deletes can change later pages. Neither the query nor the response selects
value bytes, length, digest, or preview.

## Delete (`secret.delete`)

| Params member | Type | Required | Since | Description |
| --- | --- | --- | --- | --- |
| `name` | canonical name | Yes | 1.2 | Secret to delete |

Params contain exactly `name`; success returns JSON null:

```json
{"name":"reports.token"}
```

```json
null
```

Deletion commits before the reply. A missing name returns
`jobu.secret.not_found`. A current job definition or a nonterminal run
snapshot referencing the name returns `jobu.secret.in_use`; there is no force
flag. Terminal historical snapshots alone do not block deletion. A lost reply
can leave the outcome unknown; list metadata before retrying.

## Errors and exposure

Malformed shapes, types, encodings, extra request members, and base64 that
passes the size checks return JSON-RPC invalid params (`-32602`) without
application data. An apparently oversized base64 value returns application
error `-32000` with
`data:{"category":"resource_exhausted","code":"jobu.secret.too_large"}`.
The decoder checks encoded length and padding-implied decoded length before
validating base64 characters and padding. An input that is both apparently
oversized and malformed therefore returns `jobu.secret.too_large`. Valid
UTF-8 input above 65,536 bytes returns the same error.
For example, with request ID 2:

```json
{"jsonrpc":"2.0","id":2,"error":{"code":-32000,"message":"Secret exceeds its raw byte limit","data":{"category":"resource_exhausted","code":"jobu.secret.too_large"}}}
```

Invalid names, limits, conflicts, and stopped mutations return the stable
operation codes in [the error reference](../errors.md). `secret.list` remains
available after mutation admission stops, subject to the service's read
failure policy. A response exceeding the configured RPC limit returns
`jobu.response.too_large`; a preceding mutation may already be durable.
Backend diagnostics and submitted bytes are never included in generated
errors, metadata, or capabilities. Storage is plaintext SQLite, and execution
or captured output can expose values as described in [Named secrets](../../secrets.md).

See [`jobuctl secret` commands](../../jobuctl.md#named-secrets) for safe
file/standard-input administration. The typed client member names are listed
in the method table above.
