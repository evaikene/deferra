# Attempt history and retained output

API 1.3 exposes `attempt.get`, `attempt.list`, and `attempt.output`. All require
object params. Reads never mutate an attempt or return resolved secret inputs.

## Get and list attempts

`attempt.get` takes a canonical `run_id` and positive `attempt_number`:

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
Continue with only `{"cursor":"<opaque token>"}`. The five-minute,
eviction, restart, retry, and live-view rules match [run.list](run.md).

## Read one output chunk (`attempt.output`)

Params contain `run_id`, positive `attempt_number`, and `channel`; optional
`offset` defaults to zero and `limit` to 16,384 raw retained bytes. Accepted
limits are 1–65,536. CLI attempts accept `stdout`/`stderr`; HTTP attempts
accept `body`/`headers`. An offset equal to the retained length requests EOF;
a larger or unrepresentable offset is invalid.

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

Malformed params return JSON-RPC `-32602`. Ordinary missing, invalid-request,
and cursor errors return application `-32000` with safe `data:{category,code}`.
Fatal persisted-data or storage failures close read admission. The CLI and typed
client routes are added in later Phase 8 stages.
