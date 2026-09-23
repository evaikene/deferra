# Run control methods

Stage 8.15 exposes `job.run_now` and `run.cancel`. `run.get` and `run.list` are
not yet registered. The daemon still advertises API 1.2; inspect
`system.info.capabilities` for the actual method inventory.

## Full run view

Run Now returns a full run view. Cancellation embeds the same view in `run`.
The required fields are `id`, `job_id`, `queue_id`, positive `job_revision`,
`origin`, `type`, `state`, `schedule_owned`, `priority`, `planned_at`,
`runnable_at`, nullable `started_at`, nullable `completed_at`, `attributes`,
`payload`, and nullable `result`. Times use canonical UTC RFC 3339 text with
six fractional digits. The attributes are the complete immutable materialized
job set; the payload is the original durable JSON template. Neither resolved
secret bytes nor attempts or captured output are included.

## Cancel a run (`run.cancel`)

The request contains exactly one field, a canonical UUID `run_id`:

```json
{"run_id":"00112233-4455-6677-8899-aabbccddeeff"}
```

The result is `{"disposition":"completed|requested","run":<full run view>}`.
`completed` means pending scheduled or retry-waiting work became durably
cancelled before the reply. Any required recurring successor and suspension
drain commit in the same transaction. `requested` means an active executor
accepted cancellation; the returned run is still Running and retains capacity
until its completion is durably recorded. A repeated active cancellation may
return `requested` again. A terminal run returns `jobu.run.state_conflict`.

The call uses the scheduler's owner-thread cancellation and fatal storage gate.
It never cancels a transport request or edits run rows directly. A lost reply
can leave the outcome unknown; inspect the run through `run.get` when that
method becomes available. A response too large for the configured body limit
returns `jobu.response.too_large` after any completed mutation remains durable.

Malformed or extra parameters return JSON-RPC invalid params (`-32602`).
Operation failures return application error `-32000` with safe `message` and
`data:{"category":"...","code":"..."}`; private backend details are omitted.
The CLI and typed client will expose cancellation in later Phase 8 stages.
