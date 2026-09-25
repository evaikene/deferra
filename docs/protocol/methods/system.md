# System methods

| Method | Purpose | Since | CLI | C++ client |
| --- | --- | --- | --- | --- |
| `system.info` | Discover the daemon and its methods | 1.0 | `system info` | `get_system_info()` |
| `system.stats` | Summarize retained runs and attempts | 1.3 | `system stats` | `system_statistics()` |

`system.stats` requires object params; `system.info` ignores params. See the [transport guide](../transport.md)
for the surrounding JSON-RPC envelope and [shared types](../types.md) for UTC
times, IDs, and statistics objects.

## Daemon information (`system.info`)

Send an empty params object for a simple request. The handler has no
method-specific fields and ignores supplied params. The result has these members:

| Member | Type | Since | Meaning |
| --- | --- | --- | --- |
| `daemon_version` | string | 1.0 | Executable version |
| `api_version` | object | 1.0 | Supported API version |
| `api_version.major` | integer | 1.0 | Protocol compatibility family; currently 1 |
| `api_version.minor` | integer | 1.0 | Additive API version; currently 3 |
| `capabilities` | array of strings | 1.0 | Sorted unique names of registered methods |

For example, send `{"jsonrpc":"2.0","id":1,"method":"system.info","params":{}}`.
The returned `capabilities` array contains exactly the methods the daemon
registered. A 1.x client checks the major version and the named capability
before calling a method; exact minor-version equality is unnecessary.

## Retained statistics (`system.stats`)

An initial query may contain these params. All filters combine. A
continuation contains **only** `cursor`.

| Member | Type | Required | Default | Since | Meaning |
| --- | --- | --- | --- | --- | --- |
| `queue_id` | ID | No | All queues | 1.3 | Queue captured on a run |
| `job_id` | ID | No | All jobs | 1.3 | Definition captured on a run |
| `type` | `cli` or `http` | No | Both | 1.3 | Runner family |
| `origin` | `scheduled` or `manual` | No | Both | 1.3 | Run origin |
| `planned` | time range | No | Last 24 hours | 1.3 | Half-open planned-run window |
| `group_by` | string | No | `none` | 1.3 | `none`, `queue`, `job`, `type`, `origin`, or `state` |
| `limit` | integer | No | 100 | 1.3 | 1–200 groups |
| `cursor` | string | Continuation only | — | 1.3 | Opaque token, alone in params |

The daemon resolves an omitted `planned.to` to its current UTC time and an
omitted `planned.from` to 24 hours before the resolved upper bound. The
resolved window must increase and span at most 31 days. It stays fixed for
the cursor's lifetime. Runs enter the cohort by their immutable planned
time; all retained attempts of those runs count, including retries outside
the window.

```json
{"planned":{"from":"2026-01-01T00:00:00Z","to":"2026-01-02T00:00:00Z"},"group_by":"queue","limit":2}
```

The result is a [statistics page](../types.md#statistics-page). An empty
cohort grouped by `none` still has one zero-count group with `key:null`.
See the [checked empty-cohort result](../examples/system-stats.result.json).
Lateness and execution duration come from wall-clock timestamps; missing,
negative, or unrepresentable samples are excluded. The page reports
`runnable_wait_ms:null` and `measurement.runnable_wait:"unavailable"`.
Capture counts use persisted output flags; a missing output row is not
evidence of successful capture.

Groups sort by canonical UUID bytes for identities and wire spelling for
enums. A page can be shorter than `limit`. Continue with
`{"cursor":"<token from next_cursor>"}`. The cursor binds the query but
pages read live retained data. It expires five minutes after the initial
page, and eviction or daemon restart may invalidate it earlier.

Malformed params return JSON-RPC `-32602`. Invalid service-level windows or
limits return `jobu.statistics.invalid_request`; invalid/expired cursors
return `jobu.statistics.invalid_cursor`. Represented operation errors use
`-32000` with safe `data.category` and `data.code`. Fatal persisted-data or
storage failures close read admission. See [Errors](../errors.md).

For example, an invalid statistics window can return:

```json
{"jsonrpc":"2.0","id":2,"error":{"code":-32000,"message":"Statistics request is invalid","data":{"category":"invalid_argument","code":"jobu.statistics.invalid_request"}}}
```
