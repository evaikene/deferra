# JobU C++ control client

`jb::jobu::ControlClient` provides typed asynchronous calls for the JobU 1.x
control protocol. Include `control_client.hpp` and link `jobu-client`. The
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

In a source checkout, `examples/jobu-client/idempotent_creation.cpp` shows a
once-now `create_job` request with a caller-selected key. The read-only example
never invokes it. The key and complete request must be retained by the
application until it has reconciled a possible lost response; the client does
not generate keys or repeat calls automatically.

`jobu-client` and its example are source-tree CMake targets. This repository
does not provide an installed C++ package export.
