# Cron schedule preview methods

`schedule.validate` and `schedule.next` use the daemon's existing cron engine.
Both require a cron-only schedule object with exactly `kind`, `expression`,
and `timezone`; once schedules are rejected as invalid params (`-32602`).
`timezone` is an IANA name or `UTC`. Neither method stores or changes a job.

## Validate (`schedule.validate`)

```json
{"schedule":{"kind":"cron","expression":"@daily","timezone":"Europe/Tallinn"}}
```

A valid schedule returns `{"valid":true}`. Invalid expressions or timezones
return the ordinary structured application error, never `{"valid":false}`.
The existing engine accepts named fields and aliases, cyclic weekday ranges,
and applies its established DST rules.

## Next occurrences (`schedule.next`)

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
`jobu.schedule.out_of_range`. The CLI and typed client will expose these
methods in later Phase 8 stages.
