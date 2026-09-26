# Attempt history and retained output

API 1.3 exposes `attempt.get`, `attempt.list`, and `attempt.output`. All require
object params. Reads never mutate an attempt or return resolved secret inputs.

| Method | Purpose | Since | CLI | C++ client |
| --- | --- | --- | --- | --- |
| `attempt.get` | Read an attempt detail | 1.3 | `attempt get` | `get_attempt()` |
| `attempt.list` | List attempt summaries | 1.3 | `attempt list` | `list_attempts()` |
| `attempt.output` | Read an output chunk | 1.3 | `attempt output` | `read_attempt_output()` |

[Shared types](../types.md#run-and-attempt-history) list result members and
their introduction versions.

## Get and list attempts

`attempt.get` takes a canonical `run_id` and an `attempt_number` from 1 through
9,223,372,036,854,775,807 (`INT64_MAX`), inclusive:

| Params member | Type | Required | Since | Meaning |
| --- | --- | --- | --- | --- |
| `run_id` | ID | Yes | 1.3 | Parent run |
| `attempt_number` | integer | Yes | 1.3 | Attempt within the run, in the range above |

```json
{"run_id":"00112233-4455-6677-8899-aabbccddeeff","attempt_number":1}
```

The result contains `run_id`, `attempt_number`, `due_at`, nullable `started_at`
and `completed_at`, `state` (`pending`, `running`, or `completed`), nullable
`outcome` (`succeeded`, `failed`, `interrupted`, or `cancelled`), and nullable
safe `result`. Captured output is separate. A missing attempt returns
`jobu.attempt.not_found`.

`attempt.list` initially takes `run_id` and optional `limit` (default 100,
range 1–200). It returns `{"items":[...],"next_cursor":null}`; items are
lightweight summaries without `result` or output, ordered by descending
`attempt_number`. A response may stop early at the 512 KiB result budget.
See an [empty result page](../examples/attempt-list.result.json).
Continue with only `{"cursor":"<opaque token>"}`. The five-minute,
eviction, restart, retry, and live-view rules match [run.list](run.md).

| Params member | Type | Required | Default | Since | Meaning |
| --- | --- | --- | --- | --- | --- |
| `run_id` | ID | Initial: yes | — | 1.3 | Parent run |
| `limit` | integer | No | 100 | 1.3 | 1–200 summaries |
| `cursor` | string | Continuation only | — | 1.3 | Sole member on continuation |

## Read one output chunk (`attempt.output`)

Params contain `run_id`, an `attempt_number` in the same range, and `channel`; optional
`offset` defaults to zero and `limit` to 16,384 raw retained bytes. Accepted
limits are 1–65,536. CLI attempts accept `stdout`/`stderr`; HTTP attempts
accept `body`/`headers`. An offset equal to the retained length requests EOF;
a larger or unrepresentable offset is invalid.

| Params member | Type | Required | Default | Since | Meaning |
| --- | --- | --- | --- | --- | --- |
| `run_id` | ID | Yes | — | 1.3 | Parent run |
| `attempt_number` | integer | Yes | — | 1.3 | Attempt within the run, from 1 through `INT64_MAX` |
| `channel` | string | Yes | — | 1.3 | CLI `stdout`/`stderr` or HTTP `body`/`headers` |
| `offset` | integer | No | 0 | 1.3 | Retained-byte offset |
| `limit` | integer | No | 16384 | 1.3 | 1–65536 raw bytes |

```json
{"run_id":"00112233-4455-6677-8899-aabbccddeeff","attempt_number":1,"channel":"stdout","offset":0,"limit":16384}
```

The result identifies the attempt and channel, then gives `status` (`available`,
`pending`, `not_captured`, or `lost`), `offset`, `bytes_returned`, nullable
`next_offset`, `retained_bytes`, nullable `total_bytes`/`omitted_bytes`,
`truncated`, `capture_lost`, `encoding`, and `data`. `encoding:"utf8"` carries
valid UTF-8 text; `encoding:"base64"` carries padded RFC 4648 data. Decode
to raw bytes before concatenating chunks. A UTF-8 character can be split by a
chunk boundary, so an individual chunk may use base64.

Offsets address retained bytes after any omitted middle segment, not original
stream positions. `pending` means the attempt is incomplete; there is no live
stream. A completed absent channel is `not_captured` unless explicit loss
evidence makes it `lost`. Loss can coexist with retained bytes. Unknown totals
remain null, especially after interrupted recovery. Captured application output
can itself contain secret values; it is not a generated diagnostic.

Malformed params, including an attempt number outside 1 through `INT64_MAX`,
return JSON-RPC `-32602`. Ordinary missing, invalid-request, and cursor errors
return application `-32000` with safe `data:{category,code}`.
Fatal persisted-data or storage failures close read admission. The equivalent
CLI and typed-client routes are listed above.

For example, reading a missing attempt can return:

```json
{"jsonrpc":"2.0","id":6,"error":{"code":-32000,"message":"Attempt was not found","data":{"category":"not_found","code":"jobu.attempt.not_found"}}}
```
