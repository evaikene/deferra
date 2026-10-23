# JobU C++ control client

`jb::jobu::ControlClient` provides typed asynchronous calls for the JobU 1.x
control protocol. Include `<jb/jobu/client/control_client.hpp>` and link
`JobU::jobu-client` from `find_package(JobU CONFIG REQUIRED COMPONENTS Client)`. The
library needs no SQLite driver or job runner in the client process. In a source
checkout, `examples/jobu-client/main.cpp` provides a buildable local-socket
example. The same typed client can wrap another already-connected
`jb::rpc::Client`.

The caller owns `Application`, the socket or other IODevice, the raw RPC client,
and an attribute registry. Construct `ControlClient` after the raw client and
destroy it first. All objects share one event-loop thread. The registry must
describe the attributes that the daemon may return. `ControlClient` does not
open a socket or run an event loop.

## Read-only example

With the Development component installed at `/opt/jobu`, build the installed
example as a standalone project:

```sh
cmake -S /opt/jobu/share/jobu/examples/jobu-client -B /tmp/jobu-client-build \
    -DCMAKE_PREFIX_PATH=/opt/jobu
cmake --build /tmp/jobu-client-build
/tmp/jobu-client-build/jobu-client-example /path/to/jobud.sock
```

Use the actual installation data directory if GNUInstallDirs was overridden.
The Client component supplies its transitive JobU libraries and fmt dependency;
it needs no SQLite, CURL, Catch2 or nlohmann_json package discovery. See the
[installation guide](installation.md#using-the-installed-sdk) for all components,
version matching and SDK relocation.

In a source checkout, build the `jobu-client-example` CMake target, then run:

```sh
.bld/examples/jobu-client/jobu-client-example /path/to/jobud.sock
```

It connects, handshakes with `system.info`, lists succeeded runs in pages, and
prints their IDs. It does not modify daemon state. Its signal handlers are
receiver-aware and are installed before `initialize()`, because an RPC reply
can arrive during the underlying call. A successful `initialize()` means the
handshake was accepted for transmission; the `ready` signal confirms that a
compatible daemon replied. Calls made before `ready` fail locally without a
request being written.

The example handles the three failure paths separately:

- `call_failed` with `ControlFailureKind::Remote` holds a represented JSON-RPC
  `RpcError`; the daemon observed and rejected that call.
- `call_failed` with `ControlFailureKind::Local` holds a `core::Error`, such as
  `jobu.client.timeout` or `jobu.client.invalid_response`.
- `failed` reports a failed handshake or terminal raw-client failure. A
  terminal failure precedes failures for its affected calls.

Every accepted call receives a nonzero local `ControlCallId`. Its successful
result arrives as a `ControlReply` alternative in `reply_received`. The ID
identifies the operation when several methods share a result type. The client
checks the daemon's advertised capabilities before writing each call. It
accepts a compatible 1.x daemon that advertises only some methods.

A handler for `ready`, `reply_received`, `failed`, or `call_failed` may destroy
the wrapper or its parent. Destruction emits no further wrapper outcomes; an
emission already in progress retains core Signal's connection snapshot and any
queued receiver deliveries from that emission. The raw client, device,
attribute registry, and event loop must still outlive the wrapper and remain
on their required event-loop thread. While the wrapper stays alive, each
accepted call receives exactly one reply or failure. Handlers may call
`close()` or `cancel_call()` reentrantly. In particular, `close()` during a
terminal `failed` handler does not discard the already-latched per-call
failures. A synchronous raw response still produces its typed signal only
after the accepting call returns.

## Method coverage

| Protocol family | Typed members |
| --- | --- |
| System | `get_system_info`, `system_statistics` |
| Queue | `create_queue`, `get_queue`, `list_queues`, `update_queue`, `suspend_queue`, `resume_queue`, `delete_queue`, `queue_statistics` |
| Job | `create_job`, `get_job`, `list_jobs`, `update_job`, `suspend_job`, `resume_job`, `move_job`, `delete_job`, `run_now` |
| Run | `get_run`, `list_runs`, `cancel_run` |
| Attempt | `get_attempt`, `list_attempts`, `read_attempt_output` |
| Secret | `set_secret`, `list_secrets`, `delete_secret` |
| Schedule | `validate_schedule`, `next_schedule_occurrences` |

Methods accept the public request DTOs used by the corresponding server codec.
An initial history or statistics query uses its typed query alternative; a
continuation uses `CursorRequest` alone. Queue and job management lists retain
their separate `after_id` contract. `attempt.output` returns raw bytes in
`AttemptOutputChunk::data`, regardless of the wire's UTF-8 or base64 encoding.
Successful queue, job, and secret deletions return `EmptyReply`; cron validation
returns `ScheduleValidationReply` only for a valid schedule. Invalid schedules
produce a remote error.

Since API 1.6, every `Queue` and `QueuePage` item carries a required
`QueueRevision` from 1 through `INT64_MAX`. Keep it as an exact integer with
the queue UUID. Fetch `get_queue()`, retain the returned `id` and `revision`,
then submit only intended settings in `UpdateQueueRequest`, for example:

```cpp
auto request = jb::jobu::UpdateQueueRequest{
    .queue = inspected.id,
    .expected_revision = inspected.revision,
    .concurrency_limit = 2,
};
auto call = client.update_queue(request);
```

Here `inspected` is the `Queue` previously delivered by `reply_received`.
The [checked queue result](protocol/examples/queue-get.result.json) and
[minimal recovery params](protocol/examples/queue-update-recovery.params.json)
illustrate the same UUID/revision while clearing the complete defaults layer and
restoring inherited retention. An omitted outer `history_retention` optional leaves retention
unchanged; an engaged outer optional containing `std::nullopt` encodes JSON null
to restore inheritance. An engaged empty `defaults` replaces the layer with `{}`.
Zero/uninitialized or out-of-range revisions fail shared validation locally.
The daemon independently enforces the token: a stale request produces remote
`-32000` with `jobu.queue.revision_conflict`, leaves state unchanged, and is
never retried. Read the same UUID and reconcile before another update. Names
can be reused; retaining only a name and numerical token cannot prove identity.
An explicit same-value update advances once; actual lifecycle transitions also
invalidate an older token. A replayed create result retains original revision
1, so use `get_queue()` to inspect the current queue before editing.

The six typed calls `create_queue`, `get_queue`, `list_queues`, `update_queue`,
`suspend_queue`, and `resume_queue` require negotiated API 1.6 or newer. Against
older peers, valid calls return local Unsupported
`jobu.client.unsupported_feature` without writing, allocating a call ID, or
emitting a later outcome. They neither synthesize missing revisions nor downgrade
updates. Readiness/closed-client errors precede request validation; shared codec
validation precedes the peer feature check. `delete_queue`, `queue_statistics`,
and other unaffected methods retain their advertised-method support. The
compatible-major handshake and capability names remain unchanged.

The minor-version requirement and advertised capability are separate checks.
An API 1.6 peer that omits the requested method fails locally with
`jobu.client.unsupported_method`; this is distinct from the older-peer
`jobu.client.unsupported_feature` rejection. Neither path writes a request.

Since API 1.6, `get_queue(GetQueueRequest const&, ControlCallOptions)` can opt
into retained deleted queues:

```cpp
auto call = client.get_queue(jb::jobu::GetQueueRequest{
    .queue = std::string{"archived-reports"},
    .include_deleted = true,
});
```

The option defaults to false. Existing calls with a UUID, string name, or
`QueueSelector` remain live-only and preserve their call options. With true,
IDs address the retained row; names prefer a live queue, then a unique original
deleted name. Multiple deleted matches fail remotely with
`jobu.queue.ambiguous_deleted_name`. Both overloads deliver the usual
revision-bearing `Queue` reply and use the same API 1.6 admission checks.
Use explicit `GetQueueRequest` construction when selecting historical visibility.

For `list_runs`, `RunFilters::state` selects one state. Since API 1.5,
`RunFilters::states` selects any of 1–7 distinct valid states, preserving caller
order in the request. The two selectors are mutually exclusive; an absent set
means no set filter, while a present empty vector is invalid. With neither
selector, the query remains unfiltered. Other filters combine with the selection
before paging, and a cursor retains the entire initial query.

The client retains the API version from its successful handshake before emitting
`ready`. A valid initial state-set request to API 1.0–1.4 returns local Unsupported
`jobu.client.unsupported_feature` with a message requiring API 1.5, without writing
a request. Scalar-state, unfiltered, and cursor-only requests keep their existing
advertised-method support. Shared codec validation still applies, and calls
before readiness or after close return `jobu.client.not_ready`. The client never
splits a state set into multiple calls or filters returned pages locally.

API 1.4 statistics replies include `StatisticsGroup::runnable_wait_coverage`
and an available `runnable_wait_ms` duration for Complete terminal runs only.
Both statistics methods use the shared decoder, which validates coverage
and sample counts while allowing unknown response fields. Zero samples have
null average/maximum; Partial and unfinished runs are excluded. See the
[statistics types](protocol/types.md#statistics-page) for provenance and
retained-history semantics.

The [observability guide](observability.md#runnable-wait-and-coverage) explains
coverage after crashes and late instrumentation; the [operations guide](operations.md#retention-and-replay-lifetimes)
explains when retained history and replay records disappear.

Cron requests use `CronScheduleInput{expression, optional_timezone}`.
Since API 1.4, omitting timezone uses the daemon's configured default; supplying `"UTC"`
keeps UTC explicit. `CreateJobRequest`, `UpdateJobRequest`, `validate_schedule`,
and `ScheduleNextRequest` share this input contract. Stored job replies use
`CronSchedule` with an explicit timezone. An omitted update schedule keeps the
stored schedule; a supplied replacement with omitted timezone uses the current
default. Retain omission when replaying a keyed creation after defaults change.

For example, `validate_schedule(CronScheduleInput{.expression = "@daily"})`
uses the daemon default, while
`validate_schedule(CronScheduleInput{.expression = "@daily", .timezone = "UTC"})`
explicitly selects UTC.

The public header documents each member's result. The
[protocol method pages](protocol/methods/system.md) and their neighboring
family pages specify request fields, limits, and durable semantics. The client
uses those shared codecs: callers do not assemble JSON or method names for
supported operations.

## Timeouts, cancellation, and mutations

Each call has a positive `ControlCallOptions::timeout` (default 5000 ms),
measured by a monotonic deadline. A wrapper admits at most 128 pending calls,
or the raw client's smaller configured limit. `cancel_call(id)` stops local
observation only; it does not send `run.cancel`. `close()` ends the wrapper's
local correlations without closing the borrowed socket or raw client.

The client never retries a mutation. `ControlFailure::outcome_unknown` is true
when a mutation may have been transmitted but its successful outcome was not
observed. It is false for represented remote errors and known pre-write
failures. After an unknown outcome, inspect the resource or replay a creation
with the **same caller-selected idempotency key and request**. Do not retry an
unkeyed write blindly.

Replay is available only while the record remains retained. Keys follow
[queue](protocol/methods/queue.md) and [job/run](protocol/methods/job.md)
resource lifetimes rather than a fixed age from creation. After retirement,
the same key is a new request subject to current eligibility; a terminal job
still rejects a new Run Now request. Inspect the resource when reconciling an
older unknown outcome instead of assuming an indefinitely retained key.

In a source checkout, `examples/jobu-client/idempotent_creation.cpp` shows a
once-now `create_job` request with a caller-selected key. The read-only example
never invokes it. The key and complete request must be retained by the
application until it has reconciled a possible lost response; the client does
not generate keys or repeat calls automatically.

The same example sources build in the checkout and through the installed SDK.
Library clients always supply their endpoint explicitly; SDK relocation does
not change the daemon or command-line client's compiled operational defaults.
