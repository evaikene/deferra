# JobU JSON-RPC protocol

`jobud` serves JSON-RPC 2.0 over a local Unix-domain socket. A client first
calls [`system.info`](methods/system.md#daemon-information-systeminfo) and uses
its `capabilities` array to learn which methods that daemon accepts. The
current daemon advertises API version 1.6 and 30 methods. A compatible 1.x
client should check the major version, the individual capability it needs, and
the minor version required by an additive request field such as `run.list.states`.

API 1.6 deliberately makes an incompatible pre-release input change within
major version 1: `queue.update` now requires `expected_revision`. There is no
unprotected compatibility mode. Older raw clients may still read the added
queue result member, but updates without it fail with Invalid params (`-32602`).
Clients must adopt the [protected queue update contract](methods/queue.md#update-queueupdate).
The current C++ client requires API 1.6 for revision-bearing queue replies and
protected updates, while preserving support for other advertised methods.

Daemon storage is format 5. Older databases are rejected unchanged, with no
automatic migration or reset; choose a fresh database deliberately. See the
[operations guide](../operations.md#format-rejection-recovery-and-troubleshooting).

## Start here

1. [Transport and JSON-RPC envelopes](transport.md) explains framing, request
   IDs, errors, batches, and connection limits.
2. [Types, fields, and defaults](types.md) defines shared result objects,
   schedules, payloads, attributes, and enum values.
3. [Errors](errors.md) explains JSON-RPC and JobU operation failures.
4. The method pages below give parameters, results, examples, and CLI/C++
   equivalents.

| Methods | Operations |
| --- | --- |
| [System](methods/system.md) | Information and retained statistics |
| [Queue](methods/queue.md) | Queue administration and statistics |
| [Job](methods/job.md) | Definition administration and Run Now |
| [Run](methods/run.md) | Run history and cancellation |
| [Attempt](methods/attempt.md) | Attempt history and retained output |
| [Secret](methods/secret.md) | Named secret administration |
| [Schedule](methods/schedule.md) | Cron validation and occurrence preview |

The [example fixtures](examples/README.md) contain small request and result
values checked against the same codecs used by `jobud` and the C++ client.

## Reading the method tables

`Since` is the JobU API version in which a method or member first appeared.
It is not the daemon executable version. Required members must be present;
optional members may be omitted and use the stated default. JSON `null` is a
value, not an omitted member, unless a table expressly allows it. JobU
operations use object params and reject unknown members; `system.info` has
no method-specific fields and ignores params. Successful result decoders recognize the
listed members and allow unknown members so older clients can read newer
results. All times on the wire are UTC RFC 3339 strings ending in `Z`.

Method pages show the `params` object and the successful `result` value. Wrap
them in the [JSON-RPC envelope](transport.md#request-and-response-envelopes)
when sending or receiving a message. The corresponding `jobuctl` commands
accept exactly the params object with `--request-file`.
