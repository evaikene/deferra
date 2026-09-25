# Cron schedule preview methods

| Method | Purpose | Since | CLI | C++ client |
| --- | --- | --- | --- | --- |
| `schedule.validate` | Validate a cron schedule | 1.3 | `schedule validate` | `validate_schedule()` |
| `schedule.next` | Preview occurrences | 1.3 | `schedule next` | `next_schedule_occurrences()` |

`schedule.validate` and `schedule.next` use the daemon's existing cron engine.
Both require a cron-only schedule object with exactly `kind`, `expression`,
and `timezone`; once schedules are rejected as invalid params (`-32602`).
`timezone` is an IANA name or `UTC`. Neither method stores or changes a job.
See [Cron schedules](../../cron.md) for the expression grammar, alias meanings,
and daylight-saving behavior.

The shared `schedule` object has these members:

| Member | Type | Required | Since | Meaning |
| --- | --- | --- | --- | --- |
| `kind` | string | Yes | 1.3 | Fixed `cron` |
| `expression` | string | Yes | 1.3 | Five-field cron expression or alias |
| `timezone` | string | Yes | 1.3 | IANA timezone name or `UTC` |

## Validate (`schedule.validate`)

| Params member | Type | Required | Since | Meaning |
| --- | --- | --- | --- | --- |
| `schedule` | cron schedule object | Yes | 1.3 | Schedule to check |

```json
{"schedule":{"kind":"cron","expression":"@daily","timezone":"Europe/Tallinn"}}
```

A valid schedule returns `{"valid":true}`. Invalid expressions or timezones
return the ordinary structured application error, never `{"valid":false}`.

| Result member | Type | Since | Meaning |
| --- | --- | --- | --- |
| `valid` | boolean | 1.3 | Always `true` on success |

The existing engine accepts named fields and aliases, cyclic weekday ranges,
and applies its established DST rules.
The accepted aliases are `@hourly`, `@daily`, and `@weekly`. A weekday range
can wrap over Sunday (`FRI-MON` means Friday through Monday). Numeric `0`
and `7` each mean Sunday but cannot both appear in one weekday field.
Weekday wildcard steps are rejected; `SUN-SAT/2` is an explicit stepped
range. Day-of-month and day-of-week restrictions both have to match. See
[schedules](../types.md#schedules-and-time-ranges) for the timezone gap and
overlap behavior.

## Next occurrences (`schedule.next`)

| Params member | Type | Required | Default | Since | Meaning |
| --- | --- | --- | --- | --- | --- |
| `schedule` | cron schedule object | Yes | — | 1.3 | Schedule to preview |
| `after` | UTC time | Yes | — | 1.3 | Return occurrences strictly later |
| `count` | integer | No | 5 | 1.3 | 1–200 occurrences |

| Result member | Type | Since | Meaning |
| --- | --- | --- | --- |
| `occurrences` | array of UTC times | 1.3 | Strictly increasing times after `after` |

This method adds a required `after` timestamp and optional `count`. `after`
must be RFC 3339 UTC with a trailing `Z`; an offset form is rejected. `count`
defaults to 5 and must be an integer from 1 through 200. A valid integer
outside that range returns `jobu.schedule.invalid_count`.

```json
{"schedule":{"kind":"cron","expression":"@daily","timezone":"UTC"},"after":"2026-01-01T00:00:00Z","count":2}
```

```json
{"occurrences":["2026-01-02T00:00:00.000000Z","2026-01-03T00:00:00.000000Z"]}
```

Each result is strictly later than `after` and the preceding occurrence. UTC
results use six fractional digits. The timezone engine handles DST gaps and
overlaps before producing UTC times. An exhausted or unrepresentable search
returns `jobu.schedule.no_future_occurrence` or
`jobu.schedule.out_of_range`. The CLI routes are `schedule validate` and
`schedule next`; the typed client members are `ControlClient::validate_schedule()`
and `ControlClient::next_schedule_occurrences()`.

For example, an invalid cron expression can return:

```json
{"jsonrpc":"2.0","id":3,"error":{"code":-32000,"message":"Cron expression is invalid","data":{"category":"invalid_argument","code":"jobu.schedule.invalid_expression"}}}
```
