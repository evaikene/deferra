# JobU PHP API reference

The `jobu.so` module exposes the `JobU` namespace for managing queues, scheduled
CLI and HTTP jobs, execution history, output, secrets, and statistics from PHP.
It talks synchronously to a local `jobud` daemon through a Unix-domain socket.
PHP submits and manages work; the daemon executes the commands and HTTP requests.

This manual documents the current public v1 API. PHP 8.1+ with 64-bit integers
and NTS is required. The tested runtime matrix is PHP 8.2–8.6 NTS on Linux;
PHP 8.1 and macOS runtime execution are not covered by that matrix. See the
[root README](../README.md#php-builds-and-installation) for building, installing,
and enabling the module and for the complete compatibility matrix.

## Contents

- [Quick start](#quick-start)
- [API conventions and connection lifetime](#php-api-overview)
- [Exceptions](#exceptions) and [reconnect/retry behavior](#recovery-contract)
- [Durations, secret references, and schedules](#immutable-value-primitives)
- [Attributes and queue defaults](#mutable-attributes-and-queue-defaults)
- [CLI payloads](#cli-payloads) and [HTTP payloads](#http-payload-configuration)
- [Queries and result values](#queries-metadata-and-execution-results)
- [Queues](#queue-resources) and [jobs](#jobs-and-manual-runs)
- [Pages](#definition-lists-and-pages), [history](#retained-run-and-attempt-history), and [output](#retained-output)
- [Secrets and cron helpers](#secrets-and-cron-helpers)
- [System information and statistics](#system-and-statistics)
- [Compatibility and limitations](#compatibility-and-limitations)
- [Class index](#class-index), [class reference](#class-reference), and [enums](#enum-reference)

## Quick start

Runnable scripts for CLI/HTTP submission, cron, revisions, pages, output, and reconnect
are in [examples/jobu-php](../examples/jobu-php/README.md).

Enable the module and start a disposable test daemon using the
[root instructions](../README.md#run-jobu). The PHP process needs permission to
connect to its socket. Executables, working directories, and HTTP destinations
are interpreted on the daemon's host with the daemon's execution permissions.

Save this example as a PHP script and pass the test daemon's socket path as its
first command-line argument. It creates a uniquely named queue and one CLI job.
It leaves those resources in that test daemon for inspection; it does not delete
or modify existing queues. `/usr/bin/printf` must exist on the daemon's host.

```php
<?php
declare(strict_types=1);

if (!extension_loaded('jobu')) {
    throw new RuntimeException('Enable the jobu extension first.');
}
if ($argc !== 2) {
    throw new InvalidArgumentException('Supply the test daemon socket path.');
}

$client = new JobU\Client($argv[1], timeout: JobU\Duration::seconds(5));
try {
    $queue = $client->createQueue(name: 'php-demo-' . bin2hex(random_bytes(6)));
    $attributes = new JobU\CliJobAttributes();
    $attributes->setOutputCapture('always');

    $job = $queue->createCliJob(
        payload: new JobU\CliPayload('/usr/bin/printf', ['%s\n', 'Hello from JobU']),
        schedule: JobU\OnceSchedule::now(),
        name: 'hello',
        attributes: $attributes,
    );

    echo 'Queue: ', $queue->getName(), "\n";
    echo 'Job: ', $job->getId(), "\n";
} finally {
    $client->close();
}
```

Creation already schedules the initial run. Calling `runNow()` would add another
run. Successful submission does not mean that execution has finished. Closing
the Client ends the management connection and leaves the submitted work in the
daemon. See [history](#retained-run-and-attempt-history) and
[output](#retained-output) to inspect it using a new connection.

Subsequent snippets demonstrate individual operations. Unless a snippet creates
its own Client, assume `$client` is open, `$queue` is a Queue loaded through it,
and `$job`, `$run`, or `$attempt` is the corresponding resource. Use a test daemon
and synthetic values when trying examples that create or modify state.

## PHP API overview

All classes and enums are in namespace `JobU`. The [class index](#class-index)
links to the exact public signatures; the sections below explain their behavior.
Backed enums accept their documented string values through `from()` and
`tryFrom()`. See [enum reference](#enum-reference) for every case and value.

A PHP call waits for its management operation to complete. Creating a job or
calling `runNow()` does not wait for the external command or HTTP request to
finish. A successful mutation means the daemon accepted and committed the
management change. Inspect run history and explicitly refresh snapshots to
observe execution afterward.

| Kind of operation | Effect |
| --- | --- |
| Resource field getters, query/value getters, `Page::items()` and `hasNext()` | Local reads; no daemon request. |
| Queue/Job setters and attached payload/default/attribute edits | Local changes, persisted only by `save()`. |
| `discardChanges()` and a clean `save()` | Local operations. Discard restores the last accepted snapshot. |
| Lookups, creation, lifecycle operations, `refresh()`, summary `load()`, output, secret and cron helpers, statistics | Synchronous daemon operations. Output helpers may make several requests under one timeout. |
| List methods and `Page::next()` | Initial list call fetches eagerly; first `next()` publishes that buffer locally; later `next()` calls fetch continuations. |

Resources and pages retain their originating Client. Unsetting your Client
variable does not close a connection still used by those objects. Explicit
`close()` prevents further daemon calls; cached fields and detached values
remain readable. Connections belong to one PHP request and are released at
request shutdown. Create a new Client for a later request.

Mutable configuration (`JobAttributes`, its CLI/HTTP subclasses, `QueueDefaults`,
`CliPayload`, and `HttpPayload`) supports detached deep cloning. Immutable values,
resources, summaries, and pages reject cloning. Extension-owned ordinary objects
reject serialization; store IDs and application configuration instead. Enums
retain PHP's native serialization and conversion behavior. Use `===` to compare
PHP object identity and IDs to compare resource identity; value comparisons are
explained with the relevant value types below.

### Connecting and timeouts

`new JobU\Client(string $socketPath, ?JobU\Duration $timeout = null)` eagerly
connects to the Unix socket and completes `system.info`. It requires API major 1,
minor at least 7, and all thirty v1 capabilities; extra capabilities and newer
compatible minors are accepted. Failure releases the partial session. `close()`
is idempotent and affects only that client. Client cloning and serialization
are rejected; reconstructing an already initialized object is also rejected.

The default timeout is ten seconds. A custom constructor timeout becomes the
client's default for future operations. It must be positive and representable
by the monotonic clock. One absolute deadline covers all phases of an operation;
connection, handshake, requests, and retries never receive fresh full budgets.
A timeout stops the PHP wait; it does not cancel a submitted job or establish
that a mutation was rolled back. Timeouts are cooperative and cannot interrupt
an executing callback.

`Duration::milliseconds(int)`, `seconds(int)`, `minutes(int)`, `hours(int)`, and
`days(int)` create immutable signed durations with checked integer millisecond
storage. Days are exactly 24 hours. Overflow raises `ValueError`; no floating-point
time conversion is used. Zero/negative durations can be represented but are
invalid as timeouts. Direct construction, cloning, and serialization are rejected.

## Exceptions

`JobU\Exception` extends PHP's `\Exception`. Its direct subclasses are
`ConnectionException`, `TimeoutException`, `NotFoundException`,
`RevisionConflictException`, `InvalidCursorException`, `UnsavedChangesException`,
`OutcomeUnknownException`, and `UnsupportedFeatureException`.

The inherited constructor accepts `message`, integer `code`, and `previous`.
`getErrorCode(): ?string` and `getCategory(): ?string` read separate daemon
metadata, initially null for user-created exceptions. `getCode()` keeps its
standard integer meaning. Constructor and operation failures populate stable error
code/category metadata. `OutcomeUnknownException` additionally exposes
`getOperation(): ?string` and `getIdempotencyKey(): ?string`; both default to null
for user-created exceptions. Represented remote errors retain their numeric
JSON-RPC code in `getCode()`. PHP `TypeError` and `ValueError`
remain native engine errors. Exceptions and their subclasses reject serialization and
unserialization, including user-defined serialization hooks.

Wrong argument types/ranges raise PHP `TypeError`/`ValueError`. Constructor
connection/handshake failures raise `ConnectionException`, deadline expiry raises
`TimeoutException`, and incompatible API/capabilities raise
`UnsupportedFeatureException`. Other initialization failures use `JobU\Exception`.
Messages omit socket paths and remote/backend detail; stable codes/categories are
available through the existing getters. An already pending PHP exception wins.

| Exception | Meaning and caller response |
| --- | --- |
| `JobU\Exception` | Other operational/domain failures. Inspect `getErrorCode()` and `getCategory()` for the reason. |
| `ConnectionException` | Connection/handshake failure or a closed/unusable Client. Recreate a Client when needed. |
| `TimeoutException` | The operation deadline expired. Do not infer rollback or execution cancellation. |
| `NotFoundException` | Missing, purged, deleted where disallowed, or outside the receiving resource's scope. |
| `RevisionConflictException` | Another writer changed the resource. Local edits remain; reconcile explicitly before trying again. |
| `InvalidCursorException` | Listing continuation expired or became invalid. Starting a new listing is an application decision and may repeat items. |
| `UnsavedChangesException` | Save or discard local edits before refreshing or performing the guarded action. |
| `OutcomeUnknownException` | A mutation may have committed. Inspect state independently; keep any returned idempotency key. |
| `UnsupportedFeatureException` | The daemon API or capabilities cannot support the operation. |

PHP `TypeError`, `ValueError`, and `ArgumentCountError` describe invalid calls.
`HttpPayload::setJsonBody()` can throw `JsonException` or propagate a callback's
exception. Advancing an exhausted Page throws `OutOfBoundsException`. These are
outside the `JobU\Exception` hierarchy.

For example, handle an uncertain save without blindly resubmitting it. This
function assumes the supplied Queue is editable and the caller will decide how
to reconcile the pending name with a fresh independently loaded snapshot:

```php
function renameQueue(JobU\Queue $queue, string $name): void
{
    $queue->setName($name);

    try {
        $queue->save();
    } catch (JobU\OutcomeUnknownException $error) {
        // The edit stays pending, and the daemon may already have committed it.
        echo 'Uncertain operation: ', $error->getOperation() ?? 'unknown', "\n";
        throw $error;
    }
}
```

Avoid logging full exception traces containing application arguments when those
arguments include secrets. See [secret trace behavior](#secrets-and-cron-helpers)
for the PHP 8.1 limitation and PHP 8.2+ parameter redaction.

## Recovery contract

Public management and resource methods reconnect after recoverable transport
failures according to the policy below. Retries preserve the original request
parameters and repeat the daemon compatibility check. All attempts, reconnect
delays, and internal reads share one operation deadline. A per-call Duration
override does not change the client's default.

| Operation after transport loss | Replay policy |
| --- | --- |
| Definitely not transmitted | Retry within the deadline, provided the failure is a recoverable transport failure |
| Reads, including cursor reads | Retry the identical request; never restart pagination |
| Queue/job suspend or resume | Repeat the same desired-state action |
| Queue/job creation and Run Now | Repeat identical parameters with the original idempotency key |
| Save, move, delete, run cancellation, secret changes, other mutations | Never replay after possible transmission |

Creation and Run Now receive a randomly generated idempotency key unless one
was supplied. The same key survives every in-call retry. Explicit keys remain
unchanged and must contain 1–128 UTF-8 bytes.
Daemon idempotency records last only as long as their associated retained resources;
replay safety is not a promise of permanent key retention.

An uncertain unsafe mutation returns `OutcomeUnknownException` promptly without
reconciliation. It leaves caller-owned request/dirty inputs untouched; a later
independent operation may reconnect. If a keyed mutation remains unresolved after
recovery fails, its exception retains the operation and original key for later
reconciliation. Connection/reconnect deadline exhaustion makes the client unusable;
explicit close also prevents subsequent reconnection. An uncertain unsafe mutation
whose wait expires remains eligible for reconnection on a later independent call.

Observed remote application errors are not retried. Invalid/expired cursors become
`InvalidCursorException`, revision conflicts become `RevisionConflictException`,
and represented post-commit `jobu.response.too_large` mutation errors become
`OutcomeUnknownException`. Error messages omit submitted values, peer text, socket
paths, and backend detail. Stable code/category metadata remains available, and an
already pending PHP exception is preserved.

## Immutable value primitives

`Duration::milliseconds/seconds/minutes/hours/days(int)` stores an exact signed
64-bit millisecond count; `getMilliseconds(): int` returns that count. Multiplication
overflow raises `ValueError`. A day is exactly 24 hours; zero and negative durations
are values, although client timeouts must be positive.

`new SecretReference(name: 'service.token')` stores only the symbolic name.
`getName(): string` returns it without looking up any secret. Names are at most 128
bytes, with lowercase dot-separated segments beginning with a letter and continuing
with lowercase letters, digits or underscores. Invalid names are rejected without
normalization. Secret administration is available through Client; payloads retain symbolic references.

`Schedule` is abstract. Its implemented subclasses are final:

```php
$once = new JobU\OnceSchedule(new DateTimeImmutable('2030-01-01T12:00:00.123456+02:00'));
$utc = $once->getAt(); // fresh UTC DateTimeImmutable, preserving .123456
$now = JobU\OnceSchedule::now(); // isNow() is true; getAt() is null
$defaultZone = new JobU\CronSchedule('@daily');
$utcCron = new JobU\CronSchedule(expression: '@daily', timezone: new DateTimeZone('UTC'));
```

Absolute once schedules copy a `DateTimeInterface` instant. Date getters return
fresh UTC `DateTimeImmutable` values with exact microsecond precision. Values outside
the native system clock range raise `ValueError` instead of overflowing; this range
can be narrower than PHP's date range. Symbolic `now` remains unresolved until daemon
job creation and cannot be used with `Job::setSchedule()`.

`CronSchedule::getExpression(): string` preserves the expression. `getTimezone():
?DateTimeZone` returns a fresh timezone object, or null for an unresolved daemon
default. Omission and explicit null have the same meaning; explicit UTC stays
explicit. Named PHP timezones are accepted; offset and abbreviation forms and
uninitialized timezone objects are rejected. Mutating a supplied or returned date
or timezone cannot modify a schedule. Cron grammar and occurrence calculation remain
the daemon's responsibility; constructors perform no RPC. Loaded schedules carry
the resolved timezone.

Duration, SecretReference, and the concrete schedules reject cloning, serialization,
dynamic properties, and repeated initialization. Ordinary invalid arguments use
native PHP `TypeError`/`ValueError`; pending PHP exceptions are preserved. UUIDs remain
canonical lowercase strings, validated by resource and query methods. There is no public UUID wrapper or validation function.

### Value comparison

For values of the same concrete class, `==`, `!=`, ordering operators, and `<=>`
compare native contents. This also makes non-strict `in_array()` and `array_search()`
compare contents. `===` and strict array searches still compare object identity.

- Duration compares signed milliseconds, so `seconds(1)` equals `milliseconds(1000)`.
- SecretReference compares names lexically.
- OnceSchedule compares absolute instants chronologically, including microseconds.
  Two symbolic `now` values are equal; symbolic now sorts before every absolute
  instant without resolving the current time.
- CronSchedule compares expression text first, then timezone names lexically.
  Omitted timezone sorts before an explicit timezone and is distinct from explicit
  UTC. Equivalent cron expressions or timezone aliases are not normalized.

Different concrete classes are unequal and have no defined relative ordering.
Standard PHP comparisons with scalars retain PHP's coercion rules.

## Mutable attributes and queue defaults

`new JobU\JobAttributes()`, `new JobU\CliJobAttributes()`,
`new JobU\HttpJobAttributes()` and `new JobU\QueueDefaults()` create empty,
partial configuration layers. They do not connect to a daemon or insert built-in
defaults. All four provide `get(string $name): mixed`, `has(string $name): bool`
and `set(string $name, mixed $value): void`. A known, applicable but absent value
returns `null`; null itself cannot be stored. Unknown names, wrong runner families
and invalid ranges raise `ValueError`; incompatible PHP types raise `TypeError`.
Generic setters accept the registry's scalar types, Duration objects and lists of
strings. Lists are copied in both directions, including referenced input elements.

`JobAttributes` accepts both runner families. Its final CLI/HTTP subclasses limit
access to common and matching-runner attributes. Jobs have no generic removal
method. `QueueDefaults::clear(string $name): void` removes one override and does
nothing for an already absent known attribute. Queue defaults accept both families.

```php
$defaults = new JobU\QueueDefaults();
$cli = $defaults->cli();
$http = $defaults->http();
$cli->setTimeout(JobU\Duration::seconds(30));
$http->getTimeout()->getMilliseconds(); // 30000: common values share backing
$cli->setTerminationGrace(JobU\Duration::seconds(5));
$http->setTlsVerify(true);

$copy = clone $defaults;
$copy->clear('job.timeout'); // original defaults and views are unchanged
```

`cli(): CliJobAttributes` and `http(): HttpJobAttributes` return cached mutable
views: repeated calls return the same PHP object. A retained view keeps its parent
alive. `Queue::setDefaults()` updates existing views in place.

Every suffix below supplies `getSuffix(): ?Type` and
`setSuffix(Type $value): void`. Getters return null when the value is absent.
Typed scalar setters follow PHP's normal weak/strict argument rules; generic
mixed setters use definition-directed type checks. Numbers allow integer widening.

| Available on | Suffix | Type | Registry name |
| --- | --- | --- | --- |
| All JobAttributes | Timeout | Duration | job.timeout |
| All JobAttributes | OutputCapture | string | output.capture |
| All JobAttributes | RetryInitialDelay | Duration | retry.initial_delay |
| All JobAttributes | RetryMaxDelay | Duration | retry.max_delay |
| All JobAttributes | RetryMaxAttempts | int | retry.max_attempts |
| All JobAttributes | RetryJitter | float | retry.jitter |
| All JobAttributes | RetryMultiplier | float | retry.multiplier |
| All JobAttributes | RetryMode | RetryMode | retry.mode |
| All JobAttributes | RetryStrategy | string | retry.strategy |
| CLI | RetryExitCodes | `list<string>` | cli.retry_exit_codes |
| CLI | TerminationGrace | Duration | cli.termination_grace |
| CLI | StdoutLimit | int | output.stdout_limit |
| CLI | StderrLimit | int | output.stderr_limit |
| HTTP | FollowRedirects | bool | http.follow_redirects |
| HTTP | IdempotencyKey | bool | http.idempotency_key |
| HTTP | TlsVerify | bool | http.tls_verify |
| HTTP | MaxRedirects | int | http.max_redirects |
| HTTP | HttpBodyLimit | int | output.http_body_limit |
| HTTP | HttpHeadersLimit | int | output.http_headers_limit |
| HTTP | RetryErrors | `list<string>` | http.retry_errors |
| HTTP | RetryStatuses | `list<string>` | http.retry_statuses |

`OutputCapture` accepts `none`, `on_error` or `always`; `RetryStrategy` accepts
`fixed` or `exponential`. Generic `get/set('retry.mode', ...)` uses `blocking` or
`reschedule` strings; typed methods use the existing `RetryMode` enum.
`IdempotencyKey` is the HTTP execution policy boolean, not a management RPC key.
The accepted bounds and built-in defaults are listed below. These defaults are
materialized when the daemon accepts a new job: built-in values are overlaid
by daemon defaults, queue defaults, and then explicit job attributes, in that order. A newly constructed partial attribute object still
returns null for every absent value.

| Attribute suffix | Accepted values | Built-in default |
| --- | --- | --- |
| Timeout | 1 millisecond–30 days | 120 seconds |
| RetryInitialDelay | 0–24 hours | 0 milliseconds |
| RetryMaxDelay | 0–30 days; at least the initial delay | 24 hours |
| RetryMaxAttempts | 1–100, including the first attempt | 1 |
| RetryJitter | Finite number from 0 through 1 | 0.0 |
| RetryMultiplier | Finite number from 1 through 100 | 2.0 |
| RetryMode | `RetryMode::Blocking` or `RetryMode::Reschedule` | Reschedule |
| RetryStrategy | `fixed` or `exponential` | `fixed` |
| OutputCapture | `none`, `on_error`, or `always` | `on_error` |
| TerminationGrace | 0–300,000 milliseconds | 5 seconds |
| StdoutLimit / StderrLimit / HttpBodyLimit | 0–67,108,864 bytes | 1,048,576 bytes each |
| HttpHeadersLimit | 0–4,194,304 bytes | 65,536 bytes |
| FollowRedirects / IdempotencyKey | Boolean | false |
| TlsVerify | Boolean | true |
| MaxRedirects | 0–20; positive when redirects are enabled | 5 |
| RetryExitCodes | List of exit-code selectors, e.g. `['1', '3-5']`; at most 64 normalized selectors | `['0-255']` |
| RetryStatuses | List of HTTP status selectors, e.g. `['429', '500-599']` | `['408', '429', '500-599']` |
| RetryErrors | List of supported HTTP error names | `['resolve', 'connect', 'tls_handshake', 'timeout', 'send', 'receive']` |

Selectors use decimal codes or inclusive ranges: 0–255 for CLI and 100–599 for
HTTP. Invalid or overlapping retry selectors are rejected. Supported HTTP retry
error names are `resolve`, `connect`, `tls_handshake`, `tls_verification`,
`timeout`, `send`, `receive`, `redirect`, and `protocol`.
`Blocking` holds queue capacity during the retry delay; `Reschedule` releases it.
Retry criteria apply to failed executions and remain subject to the maximum
attempt count. Output limits bound retained bytes and do not redact secrets.

A setter validates its individual value before changing anything. Partial local
edits may temporarily violate cross-field relationships, such as initial retry
delay exceeding maximum delay. Submission, merging attributes, and replacing
configuration check those relationships before accepting the complete supplied layer.
They never materialize defaults; effective configuration validation remains the
daemon's responsibility. Failed edits or failed configuration validation preserve the
previous layer. Merge copies every supplied attribute key, while defaults
replacement copies the entire supplied layer, preserving attached object identity.

Cloning any configuration creates independent native backing and drops attachment
and mutation notification. A cloned typed view contains only its applicable
attributes; a cloned QueueDefaults contains both families and creates its own
cached views. Returned arrays cannot mutate the original configuration. Comparison
uses visible attribute names and values in canonical name order, independent of
insertion order or attachment; `===` still tests identity. All four classes reject
serialization and repeated initialization. Internal classes disallow dynamic
properties. These objects remain local to the owning PHP request.

## CLI payloads

`CliPayload` is a local mutable template. Construction and setters perform no
RPC, execute no commands, and never look up secret values:

```php
$payload = new JobU\CliPayload(
    '"/opt/My Tools/worker" --verbose',
    arguments: ['--token', new JobU\SecretReference('service.token')]
);
$payload->setWorkingDirectory('/app');
$payload->setExpectedExitCodes([0, 2]);
$payload->setEnvironment('TOKEN', new JobU\SecretReference('service.token'));
$payload->unsetEnvironment('HOME'); // explicit null/removal at execution
$payload->clearEnvironment('HOME'); // remove the payload override
```

The constructor and `setCommand(string $command, array $arguments = [])`
reuse JobU's shell-like tokenizer, preserving quoted executable paths and
appending the supplied argument list verbatim. There is no variable expansion,
globbing, pipe/redirection interpretation, shell execution, or inherited PATH.
Executable names containing spaces must be quoted or escaped within the command
string. `setExecutable(string $executable)` instead accepts a literal executable
and preserves arguments. `setArguments(array $arguments)` replaces only argv;
`addArgument(string|SecretReference $argument)` appends one value.
`getExecutable()` and `getArguments()` return lossless independent values;
there is no `getCommand()`.

`setCommand()` replaces executable and argv atomically while preserving the
working directory, environment and expected exit codes. Each setter updates only
its own fields after validation; a failed setter applies none of its own changes.
Independent edits made by PHP code during argument coercion remain in effect.
Local field checks reject malformed UTF-8, NUL bytes,
invalid paths, invalid environment names and collection limits. Executables must
be absolute paths or bare names; working directories must be absolute. Each path
is limited to 4096 bytes. Argument lists contain at most 1024 values, each an exact
string or `SecretReference`. Array keys must form a list starting at zero.

`getWorkingDirectory()` defaults to `/`. `setExpectedExitCodes(array $codes)`
accepts distinct integers 0–255, including an empty list to accept no exit code;
`getExpectedExitCodes()` returns ascending codes and defaults to `[0]`.
`getEnvironment()` returns an independent map with strings, symbolic references,
and explicit null removals. Missing keys differ from present nulls. Set one entry
with `setEnvironment(string $name, string|SecretReference $value)`, record null
with `unsetEnvironment(string $name)`, or remove it with
`clearEnvironment(string $name)`. At most 256 entries are allowed. Names follow
`[A-Za-z_][A-Za-z0-9_]*`; injected `JOBU_JOB_ID`, `JOBU_RUN_ID`, and `JOBU_ATTEMPT`
are reserved. `PATH` and all `JOBU_*` entries cannot hold secret references.

A bare executable may be edited before its PATH is supplied. Submission requires an explicit literal PATH of at most 256 absolute nonempty
directories, bounded candidate paths, and the protocol's 256 KiB serialized and
prepared-request limits. No process environment fills missing values. The daemon
remains authoritative and rechecks secret-dependent bytes after resolution.
Submit CLI jobs with `Queue::createCliJob()`.

Cloning deep-copies the payload and detaches it from any owner. Equality compares
native values, ignoring environment insertion order and exit-code input order;
argument order matters. Strict identity remains PHP object identity. Getters and
clones retain no mutable caller arrays or references. Serialization, dynamic
properties and repeated construction are rejected. Error messages and native
object debugging do not expose submitted argument/environment values.


## HTTP payload configuration

`HttpPayload` is a local mutable request template. It performs no RPC and never
resolves a `SecretReference`. Ordered, repeated header names are preserved
through submission and execution.

```php
$payload = new JobU\HttpPayload('https://example.test/events', method: 'POST');
$payload->addHeader('Authorization', new JobU\SecretReference('service.token'));
$payload->addHeader('X-Tag', 'first');
$payload->addHeader('x-tag', 'second');
$payload->setJsonBody(['event' => 'ready']);
$payload->clearHeader('X-TAG'); // Removes both entries; other headers keep their order.
```

| Methods | Behavior |
| --- | --- |
| `__construct(string $url, string $method = 'GET')` | HTTP(S) final-wire URL and one supported method. |
| `getUrl(): string`, `setUrl(string $url): void` | Literal URL; no userinfo, fragment, whitespace, raw non-ASCII or malformed percent escapes. |
| `getMethod(): string`, `setMethod(string $method): void` | GET, POST, PUT, PATCH, DELETE or HEAD; ASCII case-insensitive input becomes uppercase, without trimming. |
| `addHeader(string $name, string\|SecretReference $value, bool $sensitive = false): void` | Append one entry; preserve spelling and duplicates. |
| `getHeaders(): array` | Fresh immutable `HttpHeader` snapshots in insertion order. |
| `clearHeader(string $name): void` | Remove every case-insensitive match. |
| `setBody(string\|SecretReference $body): void`, `getBody(): string\|SecretReference\|null`, `clearBody(): void` | Arbitrary bytes or a symbolic reference; null means absent, distinct from empty bytes. |
| `setJsonBody(mixed $value): void` | Native PHP JSON encoding followed by an atomic body/Content-Type update. |
| `getExpectedStatuses(): array`, `setExpectedStatuses(array $statuses): void` | Ordered list of 1–64 string selectors, each 100–599 or an inclusive range such as `200-299`. Defaults to `['200-299']`; duplicates remain explicit. |

`HttpHeader` exposes `getName(): string`, `getValue(): string|SecretReference`
and `isSensitive(): bool`. It has no public constructor or setters and cannot be
cloned. References always mark their header sensitive. Authorization, Cookie,
and Proxy-Authorization are sensitive regardless of the supplied flag. Sensitivity
controls redirect forwarding; literal values remain available through explicit
getters, while secret references return only symbolic names. Native debug output
and validation errors do not dump submitted credentials or body bytes.

Header names use token syntax; transport-owned and JobU metadata names remain
reserved. Literal header values must be valid UTF-8 without CR, LF or NUL. Body
strings preserve all bytes, including NUL and invalid UTF-8; UTF-8/base64 wire
encoding is internal. HEAD rejects every present body, including empty strings
and references. Changing a payload with a body to HEAD requires clearing the
body first. Other supported methods, including GET, allow bodies.

`setJsonBody()` is equivalent to `json_encode($value, JSON_THROW_ON_ERROR, 512)`
for encoding and errors. It supports `JsonSerializable`, preserves callback
exceptions, and throws `JsonException` for encoding failures. It appends
`Content-Type: application/json` only when no case-insensitive match exists,
preserving all existing Content-Type entries. It does not interpret HTTP responses.

Setters validate before changing their owned fields. Invalid edits do not partly
change the payload. PHP string coercion and JSON callbacks may independently edit
the payload; those nested edits survive even if the outer operation fails. The
outer operation validates current state after callbacks and does not overwrite
unrelated nested edits. JSON body and optional header insertion commit together.
Known header counts/bytes and body bounds are checked locally; complete serialized
payload size is checked when submitting or replacing the payload. The daemon
remains authoritative for resolved secret bytes and effective execution limits.

Cloning `HttpPayload` makes a detached deep copy, including all headers. Comparison
uses URL, canonical method, ordered header names/values/effective sensitivity,
body presence/bytes/reference, and ordered status selectors; attachment does not
affect equality. Strict comparison remains PHP object identity. Returned arrays,
headers and references do not alias mutable input state. Both classes reject
serialization, dynamic properties and repeated construction. Type mismatches use
`TypeError`, invalid values use `ValueError`, and JSON errors retain PHP semantics.

Use `Queue::createHttpJob()` to submit HTTP jobs. Edit an existing job through
`Job::payload()` or `Job::setPayload()` and persist those edits with `Job::save()`.
Attached payload identity is preserved across replacement, save, refresh, and discard.

## Queries, metadata and execution results

`TimeRange`, `RunQuery` and `StatisticsQuery` are immutable, detached inputs.
Construct them with named arguments; all getters are local. They own copies of
input dates and lists. Like the other immutable values, they reject cloning,
serialization, dynamic properties and repeated construction. `==` compares
stored content; `===` compares identity. List order is significant for value
comparison, even when a query's selection semantics are order-independent.

```php
$window = new JobU\TimeRange(
    from: new DateTimeImmutable('2030-01-01T00:00:00Z'),
    to: new DateTimeImmutable('2030-01-02T00:00:00Z'),
);
$query = new JobU\RunQuery(
    states: [JobU\RunState::Failed, JobU\RunState::Interrupted],
    planned: $window,
    limit: 50,
);
$statistics = new JobU\StatisticsQuery(
    planned: $window,
    groupBy: JobU\StatisticsGroupBy::Queue,
);
```

A `TimeRange` includes `from` and excludes `to`. Both bounds are optional; when
both exist, `from` must precede `to`. Date getters return fresh UTC
`DateTimeImmutable` values preserving microseconds within the native clock's
range. Null query ranges normalize to unbounded ranges; the range getters
always return `TimeRange` objects.

`RunQuery` accepts `queueId`, `jobId`, `state`, `states`, `origin`, `type`,
`planned`, `started`, `completed`, and `limit`. Each has a matching `getX()`
method. IDs must be canonical lowercase non-nil UUIDs. State and states are
mutually exclusive; states is either null or a list of 1–7 distinct `RunState`
cases. `RunOrigin` has `Scheduled` and `Manual`; `JobType` selects CLI or HTTP.
Limits are 1–200, defaulting to 100. No cursor is exposed by these input values.

`StatisticsQuery` accepts `queueId`, `jobId`, `type`, `origin`, `planned`,
`groupBy` and `limit`, with matching getters. `StatisticsGroupBy` has `None`,
`Queue`, `Job`, `Type`, `Origin` and `State`; the default is `None`. A fully
specified window cannot exceed 31 days. Omitted bounds remain unresolved:
the daemon will choose its current UTC time for the upper bound and 24 hours
before that for the lower bound. Construction never reads the server or
substitutes the PHP clock.

The following final result classes have private constructors. Client and resource
methods return these detached snapshots. No result getter performs RPC.

| Class | Getters |
| --- | --- |
| `SystemInfo` | `getDaemonVersion(): string`, `getApiMajor(): int`, `getApiMinor(): int`, `getCapabilities(): array`, `hasCapability(string): bool` |
| `SecretMetadata` | `getName(): string`, `getCreatedAt(): DateTimeImmutable`, `getUpdatedAt(): DateTimeImmutable` |
| `CliExecutionResult` | `getExitCode(): ?int`, `getSignal(): ?int`, `getCaptureLost(): ?bool`, plus shared result methods |
| `HttpExecutionResult` | `getStatus(): ?int`, `getDuration(): ?Duration`, `getRedirects(): ?int`, `getTlsVerified(): ?bool`, `getErrorCategory(): ?string`, `getRetryAfterAccepted(): ?bool`, `getRetryAfterClamped(): ?bool`, plus shared result methods |

Capabilities are independent sorted unique string lists. Secret metadata never
contains secret bytes, length, digest or preview. Dates are fresh UTC values.
All these snapshots use the same immutable clone/serialization/comparison
policy as the query values.

The abstract `ExecutionResult` base provides `getType(): JobType`,
`getOutcome(): ?string`, `getReason(): ?string`, `getErrorCode(): ?string` and
`getOutcomeUnknown(): ?bool`. Type comes from trusted run context, independently
of the optional result discriminator. Runner outcome strings are distinct
from `AttemptOutcome`; future outcome tokens remain readable. A missing result
is null, while an empty result object is a valid snapshot.

Both subclasses also expose `getCapturedBytes(OutputChannel): ?int`,
`getTotalBytes(OutputChannel): ?int` and `isTruncated(OutputChannel): ?bool`.
CLI accepts Stdout/Stderr and HTTP accepts Body/Headers; incompatible channels
raise `ValueError`. These are capture metadata, not output bytes. HTTP duration
maps exact integer `duration_ms` to the existing `Duration` type.

Typed projections return null for absent, null or malformed optional fields;
valid zero and false values remain distinct. Exit codes must be 0–255, signals
1–INT32_MAX, HTTP statuses 100–599, redirects 0–UINT32_MAX, and durations/counts
nonnegative signed 64-bit integers. `has(string $field): bool` and
`get(string $field): mixed` preserve all fields of the daemon's safe result
object, including unrecognized fields and malformed typed projections.
`has()` distinguishes an absent field from explicit null. Generic getters
return copied scalars, lists for JSON arrays and fresh `stdClass` objects for
JSON objects. Unsigned integers above PHP_INT_MAX become exact decimal strings;
no integer is rounded to a float. Conversion is bounded to 64 container levels
and 256 KiB of serialized result JSON. Errors do not echo result contents.
This is access to the safe terminal metadata object, not application JSON
interpretation or a raw RPC facility.

`OutputChunk` exposes `getRunId(): string`, `getAttemptNumber(): int`,
`getChannel(): OutputChannel`, `getStatus(): OutputStatus`, `getOffset(): int`,
`getBytesReturned(): int`, `getNextOffset(): ?int`, `getRetainedBytes(): int`,
`getTotalBytes(): ?int`, `getOmittedBytes(): ?int`, `isTruncated(): bool`,
`isCaptureLost(): bool` and `getData(): string`.

`getData()` returns raw bytes, including NUL; UTF-8/base64 wire encoding is
internal. Offsets address retained bytes, excluding omitted content.
`OutputStatus` distinguishes Available, Pending, NotCaptured and Lost. Empty
bytes do not establish availability, and Lost may still contain retained bytes.
Null observation counts remain unknown; null next offset means EOF/no further
slice. Metadata beyond PHP's signed integer range is rejected rather than
rounded. Output operations are described below.

## Queue resources

`Client::getQueue(string $name, bool $includeDeleted = false, ?Duration $timeout = null)`
always selects a literal name, even a UUID-looking name or an `id:`/`name:` prefix.
`getQueueById(string $id, bool $includeDeleted = false, ?Duration $timeout = null)`
requires a canonical lowercase UUID. Both return a new cached `Queue` retaining
its originating Client. Internal requests always use that stable UUID after rename.

```php
$queue = $client->createQueue(
    name: 'emails', weight: 1, concurrencyLimit: 4,
    recoveryPolicy: JobU\RecoveryPolicy::FailInterrupted,
    defaults: new JobU\QueueDefaults(), historyRetention: null,
    runnableWaitWarning: JobU\Duration::seconds(10),
    idempotencyKey: null, timeout: null
);
$queue->defaults()->cli()->setTimeout(JobU\Duration::seconds(30));
$queue->setName('outgoing');
$queue->save();
```

Creation defaults weight/capacity to 1, recovery to FailInterrupted and defaults
to an empty layer. It copies supplied configuration. An omitted idempotency key
is generated once; reconnect repeats the same frozen request/key within one total
deadline. Replay returns the original creation snapshot, which may be stale;
call `refresh()` explicitly before editing it. Keys remain safe for replay only
while the daemon retains their resource-bound records.

Cached getters expose `getId`, `getRevision`, `getName`, `getState`, `getWeight`,
`getConcurrencyLimit`, `getRecoveryPolicy`, `getHistoryRetention`,
`getRunnableWaitWarning`, `getCreatedAt`, `getUpdatedAt`, `getDeletedAt`,
`isDirty` and `isDeleted`. Dates are UTC `DateTimeImmutable`; only deletion time
and retention are nullable. Getters perform no RPC, including after Client closure.

Local setters are `setName(string)`, `setWeight(int)`, `setConcurrencyLimit(int)`,
`setRecoveryPolicy(RecoveryPolicy)`, `setHistoryRetention(?Duration)` and
`setRunnableWaitWarning(Duration)`. Names contain 1–128 UTF-8 bytes without ASCII
controls or reserved deletion suffixes. Weight/capacity range from 1 through
4294967295. Retention requires nonnegative whole seconds: null inherits daemon
policy and zero is unlimited. Warning duration requires nonnegative milliseconds;
zero disables warnings. Null warning at creation selects the ten-second default.
Out-of-range or inexact duration conversions fail instead of rounding.

`getDefaults()` returns a detached deep copy; `defaults()` returns the stable
attached object. `setDefaults(QueueDefaults)` replaces the whole layer in place;
an empty layer clears defaults on save. Issued CLI/HTTP views keep their identity
across replacement, save, refresh and discard. Defaults affect future jobs only.

`save(?Duration $timeout = null)` sends net changed fields and the accepted
revision. A clean save performs no RPC. Conflicts and uncertain outcomes preserve
edits and revision; no recovery policy is changed. `discardChanges(): void` restores
the last accepted snapshot locally, including after Client closure.
`refresh(bool $includeDeleted = false, ?Duration $timeout = null)` rejects dirty
state and adopts a validated snapshot without replacing attached objects.

`suspend`, `resume` and `delete` each accept `?Duration $timeout = null` and return
void. They reject dirty/deleted state locally. Suspend/resume adopt returned
snapshots; suspension may still be draining. Delete requires a fully suspended
queue remotely. Its null reply marks `isDeleted()` true but preserves cached
server fields and revision: it does not invent a deletion timestamp or token.
`refresh(includeDeleted: true)` can then load retained historical state.
All mutation, including retained defaults/view edits, is blocked after deletion.
Detached configuration copies remain editable. Queue objects cannot be cloned or
serialized. Retained defaults and views keep their resource and Client alive.

`Queue::statistics(?StatisticsQuery $query = null, ?Duration $timeout = null): Page`
reads retained statistics by stable UUID, including for dirty or deleted queues.
It leaves local edits and cached state untouched. See [system and statistics](#system-and-statistics)
for typed groups, window metadata and cursor behavior.

## Jobs and manual runs

Create definitions through a clean, nondeleted Queue with `createCliJob()` or
`createHttpJob()`. Both accept named `payload`, `schedule`, `name`, `priority`,
`attributes`, `idempotencyKey` and `timeout` arguments. Inputs are copied; the
daemon materializes applicable defaults and commits the initial scheduled run.
Suspended queues admit creation but prevent dispatch. Save or discard local queue
changes before creating jobs, so unsaved defaults cannot be mistaken for persisted
configuration. `Queue::getJob($id)` accepts a canonical lowercase UUID and returns
only a Job belonging to that queue; a different parent raises `NotFoundException`.
Lookup preserves local queue edits. Deleted jobs raise `NotFoundException`; use
`listJobs(includeDeleted: true)` to read retained deleted definitions.

Job names are null or UTF-8 strings of at most 256 bytes without NUL; empty
strings are allowed and null clears the name. Priority is a signed 32-bit integer
(−2,147,483,648 through 2,147,483,647),
defaulting to zero. The daemon requires a suspended job for a move. Deletion
requires a suspended or terminal job with no nonterminal work.

Jobs expose cached identity, queue UUID, revision, nullable name, runner type,
state, priority, schedule and timestamps. Getters never perform RPC. All returned
instants are UTC `DateTimeImmutable` values. Runner type is fixed at creation.

`attributes()` and `payload()` return attached mutable objects. Their identities
survive save, refresh, discard and configuration replacement. `getAttributes()`
and `getPayload()` return detached deep copies, including local edits.
`setAttributes()` merges every supplied key, preserving omitted target values;
`setPayload()` replaces the complete payload and requires the matching runner.
Schedules are immutable: `getSchedule()` returns the local schedule value and
`setSchedule()` replaces it. Symbolic `OnceSchedule::now()` is creation-only;
an omitted cron timezone stays unresolved until the daemon accepts the save.

`save()` sends net changed fields with the last accepted revision. Conflicts and
uncertain outcomes preserve edits and the cached revision. No-op save and
`discardChanges()` are local. `refresh()`, `suspend()`, `resume()`, `move()`,
`delete()` and `runNow()` reject dirty Jobs. `move($queue)` requires a clean,
nondeleted destination from the same Client and submits its stable UUID with the
Job revision. Server lifecycle constraints remain authoritative. Deletion marks
the local object nonmutable, including retained children, without inventing a
revision or timestamp from the null reply. Locally deleted Jobs reject refresh;
cached getters remain available. Detached copies remain editable.

`runNow(?string $idempotencyKey = null, ?Duration $timeout = null): Run` creates a
separate manual run and returns after management completion. It never waits for
execution or replaces the future scheduled occurrence. Creation and Run Now
freeze a generated or explicit key and request across reconnects; retained keys
replay their original snapshots, which may be stale. Retention is resource-bound,
not permanent. Each operation has one total deadline. Existing unsafe-mutation
recovery rules also apply to Job save, move and delete.

The Run returned here is a complete cached snapshot retaining its Client. It
exposes IDs, captured job revision, origin, runner/state, schedule ownership,
priority, planned/runnable/optional start/completion timestamps, detached typed
attributes/payload and nullable typed execution result. These getters do not
fetch details or resolve secrets. Run refresh/cancel, history lookup/list APIs and
attempts and output operations are described below. Job and Run resources cannot be
cloned or serialized. Closing the Client preserves cached reads and local edits;
subsequent RPC operations fail locally.


### HTTP and recurring jobs

This example assumes a test HTTP service at the supplied URL. Creating the job
schedules its first cron occurrence; `setJsonBody()` only prepares the request.
The response is available later as retained bytes, without automatic JSON parsing.

```php
$payload = new JobU\HttpPayload('http://127.0.0.1:8080/tasks', method: 'POST');
$payload->setJsonBody(['task' => 'daily-report']);

$schedule = new JobU\CronSchedule('0 9 * * MON-FRI', new DateTimeZone('Europe/Tallinn'));
$client->validateSchedule($schedule);

$job = $queue->createHttpJob(
    payload: $payload,
    schedule: $schedule,
    name: 'weekday-report',
);
```

### Revision conflicts and explicit reconciliation

Each lookup returns an independent cached resource. This example starts with a
queue of weight 1, loads it twice, and demonstrates a competing update:

```php
$first = $client->getQueueById($queue->getId());
$second = $client->getQueueById($queue->getId());
$first->setWeight(2);
$first->save();

$second->setWeight(3);
try {
    $second->save();
} catch (JobU\RevisionConflictException $error) {
    $intendedWeight = $second->getWeight(); // Still 3: the failed save kept the edit.
    $second->discardChanges();
    $second->refresh();

    echo 'Current weight: ', $second->getWeight(), "\n";
    echo 'Proposed weight: ', $intendedWeight, "\n";
    // Decide whether to reapply the proposal after inspecting the current state.
}
```

`discardChanges()` restores the previous local baseline, so call `refresh()`
afterward to read the competing edit. Refreshing a dirty object directly raises
`UnsavedChangesException`. The same rules apply to Jobs. If a save instead raises
`OutcomeUnknownException`, independently load the resource to inspect the
possibly committed update before choosing which edits to retain.

## Definition lists and pages

`JobU\Page` represents a mutable position in a paged result. Listing methods
perform the first request eagerly; the returned Page starts **before** that
prefetched page. The public definition-list entry points are:

```php
$client->listQueues(
    state: null, includeDeleted: false, limit: 100, timeout: null
);
$client->listJobs(
    queueName: null, queueId: null, state: null, type: null,
    includeDeleted: false, limit: 100, timeout: null
);
$queue->listJobs(
    state: null, type: null, includeDeleted: false, limit: 100, timeout: null
);
```

Queue state is `?QueueState`; job state and runner are `?JobState` and `?JobType`.
Limits are integers from 1 through 200. Timeout is `?Duration`, using the Client's
configured default when null. Each initial call and later fetching `next()` has
one deadline covering its complete synchronous operation.

Client job listing accepts at most one `queueName` or `queueId`; both null selects
all queues. A name is exact literal UTF-8 under the same rules as `getQueue()`,
even if it looks like a UUID or begins with `id:`/`name:`. IDs are canonical
lowercase UUIDs. There is no preliminary lookup. Names are resolved on every
page, so rename or name reuse can change a later page's selection; use `queueId`
or `Queue::listJobs()` when queue identity must stay stable. Missing queues and
ambiguous retained names propagate the normal daemon errors.

Filters combine with AND. With no state filter, all nondeleted states are
included, including suspended and terminal jobs. Both runner families are included
by default. `includeDeleted: true` permits soft-deleted definitions and selection
of a retained deleted queue; a Deleted state filter alone does not enable that
visibility. These methods list current retained definitions, not run or attempt
history. Run/Attempt history, secret administration and retained statistics are described below.

`Queue::listJobs()` uses its stable UUID and originating Client, including after
rename. It permits dirty queues and leaves their edits and cached fields untouched.
For a locally deleted or historically loaded Queue, pass `includeDeleted: true`;
otherwise native admission returns not-found. Purged queues also return not-found.

Queue pages contain full `Queue` objects; job pages contain full `Job` objects
with the usual typed configuration, revision, attached objects and deletion guards.
No item getter fetches details. Each Page and resource retains the originating
Client. Separate listings create independent resource instances.

```php
$page = $queue->listJobs(type: JobU\JobType::Cli, limit: 50);
while ($page->hasNext()) {
    $page->next();
    foreach ($page->items() as $job) {
        echo $job->getId(), "\n";
    }
}
```

- `hasNext(): bool` checks only local state. An empty first result returns false.
- `next(?Duration $timeout = null): void` publishes the prefetched first page
  without RPC. Later calls fetch the continuation using one operation deadline,
  including any reconnect/retry. Calling it when `hasNext()` is false throws
  `\OutOfBoundsException` and retains the last page.
- `items(): array` returns `[]` before the first successful `next()`, otherwise
  the current indexed collection. It performs no RPC. Editing the returned array
  leaves Page's collection unchanged; contained objects retain their identities.

A fetch or conversion failure preserves the current items, statistics metadata and continuation.
A successful empty continuation publishes `[]` and ends enumeration. Short pages
may still have continuations: Page follows the returned boundary rather than
comparing the item count to the requested limit. Listings are live reads, not
frozen snapshots across requests.

Queue/job listings advance by UUID and secret listings advance by name.
Run/attempt history and statistics use server-managed continuation cursors. Expired cursors, including those invalidated by daemon restart, raise
`InvalidCursorException`; enumeration never silently restarts.

Page strongly retains its originating Client and current items.
Unsetting the caller's Client variable does not close a live Page's session.
After explicit `Client::close()`, cached access and consumption of the prefetched
buffer still work, while a subsequent fetch fails. Arrays or resource objects
retained by the caller survive advancement. Page cannot be cloned or serialized.

### Retained run and attempt history

The public API follows resource ownership:

| Owner | Lookup and enumeration |
| --- | --- |
| `Client` | Cross-queue `listJobs()` and `listRuns()`, plus queue creation and lookup |
| `Queue` | `getJob($id)`, `listJobs()` and `listRuns()` within that queue |
| `Job` | `getRun($id)` and `listRuns()` for that job |
| `Run` | `getAttempt($number)` and `listAttempts()` for that run |

`Job::getRun($id)` returns the same cached `Run` class used by `Job::runNow()`.
A run belonging to another job raises `NotFoundException`. Lookup permits dirty
or locally deleted jobs without changing their local state.

`Client::listRuns(?RunQuery $query = null)`, `Queue::listRuns()` and
`Job::listRuns()` return `Page<RunSummary>`. The query defaults to all retained
runs within the receiver's scope with a limit of 100 (accepted range 1–200).
Filters combine with AND. A Queue binds its stable UUID as `queueId`; a Job binds
its UUID as `jobId`. A conflicting query ID raises `ValueError`, while an absent
or matching ID is accepted. Other filters are preserved without changing the
supplied query. No current-queue filter is inferred from the Job.

History filters use the queue/job IDs in retained run snapshots. Terminal history
keeps its original queue after a Job move; the daemon moves nonterminal runs
and updates their captured revision together with the suspended definition.
History remains readable for deleted owners while records are retained. Scoped
listing permits dirty and locally deleted objects and never saves, refreshes or
discards local edits. History does not require a live parent or an
`includeDeleted` argument.

```php
$page = $job->listRuns(new JobU\RunQuery(
    states: [JobU\RunState::Failed, JobU\RunState::Interrupted], limit: 25
));
while ($page->hasNext()) {
    $page->next();
    foreach ($page->items() as $summary) {
        $run = $summary->load(); // explicit detail load
        $result = $run->getResult(); // cached CliExecutionResult, HttpExecutionResult or null
    }
}
```

Run summaries expose identity, captured job revision, origin, runner type, state,
schedule ownership, priority, planned/runnable times and nullable start/completion
times. They omit attributes, payload, result and output. `load(?Duration $timeout
= null): Run` creates a separate full resource; the original summary stays
unchanged. Full Run configuration getters return detached copies of the captured
template, including symbolic secret references.

`Run::listAttempts(int $limit = 100)` returns `Page<AttemptSummary>`. Attempt
summaries expose `getRunId()`, `getNumber()`, `getDueAt()`, nullable
`getStartedAt()`/`getCompletedAt()`, `getState()` and nullable `getOutcome()`.
They have no result or output getter. Attempts are ordered by descending number;
the largest number is the latest attempt, regardless of timestamps or outcome.
History pages use opaque cursor-only continuations, including after short pages;
invalid or expired cursors raise `InvalidCursorException` without restarting.
First-page prefetch, local first `next()`, and atomic continuation failure follow
the normal Page contract.

`Run::getAttempt(int $number)` and `AttemptSummary::load()` return a cached
`Attempt`, adding `getResult(): ?ExecutionResult` to the summary getters.
Attempt numbers range from 1 through `PHP_INT_MAX`. Results have the originating
Run's immutable runner type: `CliExecutionResult` or `HttpExecutionResult`, or
null when no result exists. Loading an attempt does not require a separate
parent lookup. Cached result access never performs I/O.

Summaries from cross-queue queries can still load retained details when their
original Queue or Job is deleted. Callers do not need to reconstruct the parent
object hierarchy to inspect that history.

`Run::refresh()` and `Attempt::refresh()` replace their cached snapshots only
after the complete reply has been validated. Failures preserve the previous
cache. `Run::cancel()` returns `RunCancellationDisposition::Completed` or
`RunCancellationDisposition::Requested` and atomically adopts the returned Run.
`Requested` acknowledges an active executor's cancellation request; the returned
snapshot can still be Running. Neither disposition causes PHP to wait for
execution completion. Cancelling terminal work raises the daemon's state-conflict
error. A possibly transmitted cancellation is not replayed automatically;
`OutcomeUnknownException` preserves uncertainty, and an explicit later refresh
can inspect the outcome. Cancellation does not refresh the owning Job.

All history operations accept a final `?Duration $timeout = null`, inheriting the
Client default when omitted. Getters perform no RPC, interpret no application
JSON, and resolve no secrets. Results preserve unknown safe metadata through the
existing generic accessor. Summary/resource instances retain their
Client, reject cloning/serialization/dynamic properties, and remain request-local.
Strict comparison (`===`) identifies a PHP instance; use ID getters (and attempt
number) to compare record identity rather than relying on loose object comparison.
Explicit Client closure leaves cached getters usable but prevents further RPCs.

### Retained output

For a Run with at least one retained attempt, read a bounded tail or inspect a
specific attempt’s capture metadata. Attempt 1 must still exist for this example;
use `listAttempts()` to discover retained attempts when their numbers are unknown.

```php
$tail = $run->getOutput(maxBytes: 4096, timeout: JobU\Duration::seconds(2));
$attempt = $run->getAttempt(1);
$chunk = $attempt->readOutput(JobU\OutputChannel::Stderr, offset: 0, length: 16384);
$bytes = $chunk->getData();
```

`Run::getOutput(int $maxBytes = 4096, ?Duration $timeout = null): ?string`
selects the latest attempt once by largest attempt number. No attempts returns
null. `Attempt::getOutput(int $maxBytes = 4096, ?Duration $timeout = null): ?string`
uses that specific attempt. Both accept 1–65,536 bytes. For CLI they prefer
non-empty retained stderr, otherwise stdout; channels are never concatenated.
For HTTP they select the response body. If neither CLI channel provides bytes,
confirmed empty capture returns `''` (including empty captured stderr with
unavailable stdout); otherwise the result is null.

The result is the last `min(maxBytes, retainedBytes)` raw bytes of the selected
channel. Offsets address retained bytes, excluding omitted middle content, not
original stream positions. A tail may start inside a UTF-8 character. NUL and
other binary bytes are preserved; the extension does not parse application JSON.
Partial, truncated or lost capture can still provide useful retained bytes.
Lost capture without bytes is unavailable, rather than confirmed empty capture.
Use `readOutput()` when capture status and loss/truncation metadata matter.

`Attempt::readOutput(OutputChannel $channel, int $offset = 0, int $length = 16384,
?Duration $timeout = null): OutputChunk` accepts offsets from 0 through
`PHP_INT_MAX - 1` and lengths from 1 through 65,536. CLI accepts Stdout/Stderr;
HTTP accepts Body/Headers. Wrong argument types raise `TypeError`; invalid local
bounds or channel applicability raise `ValueError` before RPC. The daemon checks
the offset against retained length: equality requests EOF, a larger offset is an
error. Wire UTF-8/base64 decoding is internal, including chunks split within a
UTF-8 character. Unknown total/omitted counts remain null.

These operations neither wait for execution completion nor refresh cached
Run/Attempt state. Incomplete attempts expose Pending, with no live output
stream. A later call may observe completion. Convenience reads use bounded
metadata probes and at most one tail fetch for the chosen channel. One total
monotonic deadline, inherited from Client or explicitly overridden, includes
latest-attempt discovery, all reads and reconnects. No timeout is reset between
requests. Transport and daemon errors propagate; they never become empty output.

Multi-read operations are not atomic snapshots. Completed retained output is
immutable, but retention may delete the selected attempt between reads; that
not-found error propagates without restarting selection. An attempt created
after latest-attempt discovery does not replace the selected identity. Retained
output remains readable after owner deletion while its history still exists.
Explicit Client closure prevents further output RPCs; already returned strings
and detached OutputChunk values remain usable. Summaries and Client have no
output methods.


## Secrets and cron helpers

These global operations belong to `Client`. Every operation accepts a nullable
`Duration` timeout override; omission uses the Client default (10 seconds unless
configured at construction). One monotonic deadline covers argument conversion,
connection, handshake, request and any safe reconnect/replay.

```php
$metadata = $client->setSecret(name: 'demo.token', value: 'synthetic-example-token');
$page = $client->listSecrets(limit: 100);
while ($page->hasNext()) {
    $page->next();
    foreach ($page->items() as $secret) {
        echo $secret->getName(), "\n";
    }
}
$client->deleteSecret($metadata->getName()); // The example has not referenced this secret.

$cron = new JobU\CronSchedule('@daily', new DateTimeZone('Europe/Tallinn'));
$client->validateSchedule($cron);
$times = $client->nextScheduleOccurrences(
    schedule: $cron,
    after: new DateTimeImmutable('2030-01-01T00:00:00Z'),
    count: 5
);
```

`setSecret(string $name, #[\SensitiveParameter] string $value, ?Duration $timeout = null): SecretMetadata`
sets or rotates 0–65,536 raw bytes, including empty strings, NUL and non-UTF-8
bytes. Encoding is handled internally by the SDK. Names have at most 128 bytes
and consist of lowercase dot-separated segments; each segment starts with a
letter and continues with lowercase letters, digits or underscores. Names are
validated without normalization. Rotation preserves the creation timestamp and
updates the modification timestamp. Returned metadata is an immutable detached
snapshot with only name and UTC timestamps. No read method, value length, digest
or preview is exposed. References in CLI arguments/environment and HTTP
headers/body remain symbolic names and are never resolved by PHP getters.

`listSecrets(int $limit = 100, ?Duration $timeout = null): Page` accepts 1–200
items per page and returns `SecretMetadata` items. It uses the same eager,
before-first Page contract as other listings. `hasNext()` and the first `next()`
perform no RPC. Later pages use the original limit and an exclusive ascending
name boundary; that secret need not still exist. This is a live view: concurrent
sets/deletes may change later pages. A failed continuation preserves the current
items and boundary. The Page retains its Client, while metadata does not.

`deleteSecret(string $name, ?Duration $timeout = null): void` propagates missing
and in-use errors, including protections for job definitions and nonterminal run
snapshots. There is no force flag. Set/delete are not replayed after possible
transmission: a lost reply produces `OutcomeUnknownException` promptly. A later,
independent list can inspect metadata before the application decides what to do.
Listing and cron helpers retain the existing safe-read reconnect policy.

Generated error messages and native debug views omit submitted secret bytes and
backend diagnostics. The value parameter is marked `SensitiveParameter`, which
PHP 8.2+ redacts in the extension call's exception-trace frame. This does not
redact values deliberately logged by application code or passed to its own
unmarked wrapper parameters. PHP 8.1 does not implement this trace-redaction
attribute; use `zend.exception_ignore_args=1` when collecting exception traces
there if your application requires that protection. The module does not change
PHP exception-trace settings. Secret use by executed commands and captured output
follows the daemon's
[named-secret exposure contract](secrets.md).

`validateSchedule(CronSchedule $schedule, ?Duration $timeout = null): void`
returns normally on success and throws on daemon validation failure; it never
returns false. `nextScheduleOccurrences(CronSchedule $schedule,
DateTimeInterface $after, int $count = 5, ?Duration $timeout = null): array`
accepts 1–200 occurrences. Results are strictly increasing UTC
`DateTimeImmutable` values strictly later than `after`. Input instants use exact,
checked microsecond conversion; the caller's date and schedule are unchanged.
Only CronSchedule is accepted. Local invalid types/ranges produce TypeError or
ValueError before submission; cron grammar and timezone availability are checked
by the daemon, with stable JobU error codes preserved.

An omitted timezone remains omitted and uses `schedule.default_timezone` on the
daemon. Explicit UTC remains UTC. The PHP default timezone does not override
these semantics. The daemon handles aliases, its 512-byte expression limit and
DST: nonexistent local times shift forward by the gap, and repeated local times
use the earlier UTC instant once. Validation can succeed for an impossible date;
preview reports `jobu.schedule.no_future_occurrence` or
`jobu.schedule.out_of_range` when no representable occurrence can be found.
See [cron semantics](cron.md) for the complete grammar. Helpers never create jobs
or evaluate cron locally.


## System and statistics

`Client::getSystemInfo(?Duration $timeout = null): SystemInfo` explicitly reads
current daemon metadata. It does not return the constructor's cached handshake.
The detached immutable result exposes daemon version, API major/minor, sorted
capabilities and `hasCapability(string $name): bool`. Its getters perform no RPC.

`Client::systemStatistics(?StatisticsQuery $query = null, ?Duration $timeout = null): Page`
reads across queues. `Queue::statistics()` has the same query/timeout signature and
uses that resource's stable UUID. A supplied query `queueId` must match the Queue;
a conflicting ID raises `ValueError` before any RPC. The input query stays unchanged.
Queue-scoped reads work with dirty, renamed and retained deleted resources; they
never save, refresh, or change pending edits. A purged/missing queue produces the
normal daemon not-found error. `jobId` filters run history without looking up a live
Job or inferring its current queue.

A null query uses `StatisticsQuery` defaults: no identity/type/origin filters,
`StatisticsGroupBy::None`, omitted window bounds and limit 100. All filters combine.
Limits are 1–200 groups. The daemon resolves an omitted upper bound to its current
UTC time, then an omitted lower bound to 24 hours before that upper bound. Windows
are increasing, half-open and at most 31 days. PHP never substitutes its own clock
for omitted bounds. A fully specified invalid range is rejected locally; invalid
partially specified windows propagate the daemon's stable error code/category.

```php
$query = new JobU\StatisticsQuery(groupBy: JobU\StatisticsGroupBy::State, limit: 20);
$page = $queue->statistics($query);
$metadata = $page->getStatisticsMetadata();
$window = $metadata->getWindow(); // Both bounds are resolved, even for empty results.

while ($page->hasNext()) {
    $page->next();
    foreach ($page->items() as $group) {
        echo $group->getKey()->value, ': ', $group->getRuns()->getTotal(), "\n";
    }
}
```

`Page::getStatisticsMetadata(): ?StatisticsMetadata` observes the prefetched/current
batch without RPC. It is available before the first `next()` and for empty grouped
results; other page sources return null. Metadata exposes `getWindow(): TimeRange`,
`getGroupBy(): StatisticsGroupBy` and `getMeasurement(): StatisticsMeasurement`.
Both window bounds return fresh UTC `DateTimeImmutable` values with microseconds.
Successful continuation replaces items, metadata and cursor together. Failed
transport, decoding or projection leaves all three untouched. Previously returned
metadata and groups remain detached immutable snapshots and retain no Client.

The Page still starts before its eagerly fetched first batch: `items()` initially
returns `[]`, and its first successful `next()` performs no RPC. With no matching
runs, ungrouped statistics has one zero-count group with a null key; grouped
statistics has no groups and `hasNext()` is false, but metadata remains accessible.
Short pages use the returned cursor, never the item count, to determine continuation.

Each `StatisticsGroup` exposes these immutable typed values:

| Method | Return type and meaning |
| --- | --- |
| `getKey()` | `string\|JobType\|RunOrigin\|RunState\|null`: canonical UUID for Queue/Job, backed enum for Type/Origin/State, null for None |
| `getRuns()` | `StatisticsRunCounts`: `getTotal(): int`, `getStateCount(RunState): int`, `getTypeCount(JobType): int`, `getOriginCount(RunOrigin): int` |
| `getAttempts()` | `StatisticsAttemptCounts`: `getTotal(): int`, `getStateCount(AttemptState): int`, `getOutcomeCount(AttemptOutcome): int`, `getRetries(): int` |
| `getCapture()` | `StatisticsCaptureCounts`: `getTruncatedAttempts(): int`, `getLostAttempts(): int` |
| `getScheduleLateness()` | `StatisticsDuration`: first run start minus planned time, once per run |
| `getExecutionWallDuration()` | `StatisticsDuration`: completed attempt start/end differences |
| `getRunnableWait()` | `?StatisticsDuration`: Complete terminal runs only, once per run across attempts; null when measurement is unavailable |
| `getRunnableWaitCoverage()` | `StatisticsRunnableWaitCoverage`: `getComplete()`, `getPartial()`, `getUnmeasured()`, `getUnfinished()`, all returning int |

Every enum counter is present, including zero. Counts retain integer precision;
a response with a count above `PHP_INT_MAX` raises `ValueError` before publication.
`StatisticsDuration` exposes `getSamples(): int`, `getAverageMilliseconds(): ?float`
and `getMaximumMilliseconds(): ?float`. Fractional milliseconds are preserved;
these aggregates do not use the integer `Duration` input class. No samples means
zero samples and null average/maximum. A measured zero contributes a sample with
`0.0` average/maximum. An unavailable runnable-wait measurement is null, distinct
from an available measurement with no Complete terminal samples.

`StatisticsMeasurement` exposes `getTiming(): string` (currently
`wall_clock_derived`), `getRunnableWait(): string` (`monotonic_observed` or
`unavailable`), and `getCapture(): string` (`persisted_output_flags`). The strings
preserve daemon provenance, including future safe values. Coverage partitions
runs into Complete, Partial, Unmeasured and Unfinished; its counters sum to run
total. Partial lower bounds are excluded from duration samples. Missing capture
rows prove neither successful capture nor a loss/truncation event.

Runs enter the cohort by planned time. All retained attempts of those runs count,
including retries outside the window. Reads neither checkpoint timing nor write
history. The resolved window stays fixed for the cursor lifetime, while counts
reflect live retained rows; retention can remove members between pages. This is
not an all-time or frozen database snapshot.

Initial reads and each continuation share one total operation deadline with any
reconnect/replay. Null timeout uses the Client's configured timeout (10 seconds
by default). Statistics and explicit system-info reads use existing safe-read
recovery. Continuation preserves the original selection and resolved window, including
for Queue statistics. Cursors expire five minutes
after the initial page and can be invalidated earlier by eviction or restart.
Expired or wrong-scope cursors raise `InvalidCursorException` without restarting
enumeration. Configured response limits and service failures preserve normal
JobU error categories/codes, including `jobu.response.too_large`.

## Compatibility and limitations

The public method names, parameter names/defaults, return types, and enum values
in the reference below describe the current v1 surface. Use resource-scoped
lookups: `Queue::getJob()`, `Job::getRun()`, and `Run::getAttempt()`. Cross-queue
run summaries can be loaded directly through `RunSummary::load()`.

| Boundary | Current behavior |
| --- | --- |
| PHP and platform | PHP 8.1+, 64-bit integers, NTS. Linux/macOS target; runtime tests cover Linux PHP 8.2–8.6 NTS. See the root compatibility matrix. |
| Daemon compatibility | API major 1, minor 7 or newer, with the required v1 capabilities. Connection validates compatibility eagerly. |
| Connection transport | Local Unix-domain socket; no TCP/TLS/authenticated remote session API. |
| Lifetime | One PHP request; no persistent cross-request or cross-thread Client. JobU ZTS is unsupported. |
| Execution model | CLI and HTTP jobs run through the daemon. No PHP callback runner, wait-for-completion method, or live output stream. |
| Scheduling | Once and Cron schedules. `ManualSchedule` is unavailable; creation schedules an initial run and `runNow()` adds a separate run. |
| Output | Bounded binary strings and capture metadata. No automatic interpretation of application JSON/JSON-RPC responses. |
| Resources | Cached snapshots with explicit refresh and revision-checked edits. No job runner-type conversion, automatic resource serialization, or raw RPC escape hatch. |
| Secrets | Symbolic references and write/metadata/delete operations; no read-back of secret values. PHP 8.1 lacks SensitiveParameter trace redaction; the module does not change PHP settings. |
| Windows | No JobU runtime target. |

Management operation idempotency and HTTP execution idempotency are separate.
The optional key on creation/Run Now protects that management request while the
daemon retains its record. `HttpJobAttributes::setIdempotencyKey(true)` controls
the outgoing HTTP request's idempotency header; it does not make every remote
service or side effect automatically safe to replay.

## Class index

- **Connections and resources:** [Client](#client), [Queue](#queue), [Job](#job), [Run](#run), [Attempt](#attempt), [RunSummary](#runsummary), [AttemptSummary](#attemptsummary), [Page](#page).
- **Values and schedules:** [Duration](#duration), [SecretReference](#secretreference), [Schedule](#schedule), [OnceSchedule](#onceschedule), [CronSchedule](#cronschedule).
- **Attributes and payloads:** [JobAttributes](#jobattributes), [CliJobAttributes](#clijobattributes), [HttpJobAttributes](#httpjobattributes), [QueueDefaults](#queuedefaults), [CliPayload](#clipayload), [HttpPayload](#httppayload), [HttpHeader](#httpheader).
- **Queries and results:** [TimeRange](#timerange), [RunQuery](#runquery), [StatisticsQuery](#statisticsquery), [SystemInfo](#systeminfo), [SecretMetadata](#secretmetadata), [ExecutionResult](#executionresult), [CliExecutionResult](#cliexecutionresult), [HttpExecutionResult](#httpexecutionresult), [OutputChunk](#outputchunk).
- **Statistics:** [StatisticsGroup](#statisticsgroup), [StatisticsRunCounts](#statisticsruncounts), [StatisticsAttemptCounts](#statisticsattemptcounts), [StatisticsCaptureCounts](#statisticscapturecounts), [StatisticsDuration](#statisticsduration), [StatisticsRunnableWaitCoverage](#statisticsrunnablewaitcoverage), [StatisticsMetadata](#statisticsmetadata), [StatisticsMeasurement](#statisticsmeasurement).
- **Exceptions:** [Exception](#exception), [ConnectionException](#connectionexception), [TimeoutException](#timeoutexception), [NotFoundException](#notfoundexception), [RevisionConflictException](#revisionconflictexception), [InvalidCursorException](#invalidcursorexception), [UnsavedChangesException](#unsavedchangesexception), [OutcomeUnknownException](#outcomeunknownexception), [UnsupportedFeatureException](#unsupportedfeatureexception).
- **Enums:** [AttemptOutcome](#attemptoutcome), [AttemptState](#attemptstate), [JobState](#jobstate), [JobType](#jobtype), [OutputChannel](#outputchannel), [OutputStatus](#outputstatus), [QueueState](#queuestate), [RecoveryPolicy](#recoverypolicy), [RetryMode](#retrymode), [RunCancellationDisposition](#runcancellationdisposition), [RunOrigin](#runorigin), [RunState](#runstate), [StatisticsGroupBy](#statisticsgroupby).

## Class reference

All unqualified JobU type names below are in `JobU`; `DateTimeInterface`,
`DateTimeImmutable`, and `DateTimeZone` are PHP built-ins. Signatures show exact
parameter names, types, and defaults for named-argument calls. `?T` means `T|null`.
Collections use PHP arrays; their element types are described in the Behavior column or
the linked topic. `Page<T>` in prose describes its item type, not PHP runtime generic
syntax.

Tables list methods declared by each class. Inherited methods are linked to the base
class; enum conversion methods and inherited PHP Exception methods retain PHP behavior.
Constructor rows are written as `__construct(...)`; call them with `new`. Methods marked
`static` are called on the class. A class without a public constructor is obtained
through the listed factory/resource operations.

All local edits and value construction may reject invalid types or values before
changing state. Daemon operations can raise the [documented exceptions](#exceptions).
Unless noted otherwise, a nullable timeout inherits the Client default and is a single
total operation deadline. Limits, ownership, null/empty distinctions, revision behavior,
and specific errors are defined in each linked behavior topic.

### Client

Owns a request-local management connection. Construct it with the daemon socket path.
Global administration and cross-queue queries belong here; queue/job/run/attempt detail
lookups belong to their resource owners.

Final class. Behavior: [php api overview](#php-api-overview).

| Signature | Behavior |
| --- | --- |
| `setSecret(string $name, #[\SensitiveParameter] string $value, ?Duration $timeout = null): SecretMetadata` | Daemon mutation; 0–65,536 bytes. Value is SensitiveParameter on PHP 8.2+. |
| `listSecrets(int $limit = 100, ?Duration $timeout = null): Page` | Daemon read; returns `Page<SecretMetadata>`, ascending names, limit 1–200. |
| `deleteSecret(string $name, ?Duration $timeout = null): void` | Daemon mutation; requires the secret to be unreferenced. |
| `validateSchedule(CronSchedule $schedule, ?Duration $timeout = null): void` | Daemon read; returns normally on success, throws on invalid cron. |
| `nextScheduleOccurrences(CronSchedule $schedule, \DateTimeInterface $after, int $count = 5, ?Duration $timeout = null): array` | Daemon read; `list<DateTimeImmutable>`, 1–200 instants strictly later than the supplied timestamp. |
| `__construct(string $socketPath, ?Duration $timeout = null)` | Connect and validate compatibility; establish the default timeout. |
| `close(): void` | Close locally and permanently; idempotent, no cancellation of submitted work. |
| `getSystemInfo(?Duration $timeout = null): SystemInfo` | Daemon read; returns a fresh detached SystemInfo. |
| `systemStatistics(?StatisticsQuery $query = null, ?Duration $timeout = null): Page` | Daemon read; returns `Page<StatisticsGroup>` across queues. |
| `listQueues(?QueueState $state = null, bool $includeDeleted = false, int $limit = 100, ?Duration $timeout = null): Page` | Daemon read; returns `Page<Queue>`. Limit 1–200. |
| `listJobs(?string $queueName = null, ?string $queueId = null, ?JobState $state = null, ?JobType $type = null, bool $includeDeleted = false, int $limit = 100, ?Duration $timeout = null): Page` | Daemon read; returns `Page<Job>`. At most one queue selector; limit 1–200. |
| `listRuns(?RunQuery $query = null, ?Duration $timeout = null): Page` | Daemon read; returns `Page<RunSummary>` across queues. |
| `getQueue(string $name, bool $includeDeleted = false, ?Duration $timeout = null): Queue` | Daemon read; literal name lookup, optionally including retained deleted queues. |
| `getQueueById(string $id, bool $includeDeleted = false, ?Duration $timeout = null): Queue` | Daemon read; stable UUID lookup. |
| `createQueue(string $name, int $weight = 1, int $concurrencyLimit = 1, RecoveryPolicy $recoveryPolicy = RecoveryPolicy::FailInterrupted, ?QueueDefaults $defaults = null, ?Duration $historyRetention = null, ?Duration $runnableWaitWarning = null, ?string $idempotencyKey = null, ?Duration $timeout = null): Queue` | Daemon mutation; copies defaults; generated/explicit replay key. |

### Queue

Cached queue obtained from Client creation, lookup, or queue pages. Setters edit local
state. Creation and lifecycle calls reject dirty/deleted queues; scoped reads preserve
local edits.

Final class. No public constructor. Behavior: [queue resources](#queue-resources).

| Signature | Behavior |
| --- | --- |
| `statistics(?StatisticsQuery $query = null, ?Duration $timeout = null): Page` | Daemon read; `Page<StatisticsGroup>` scoped to this queue; preserves local edits. |
| `getJob(string $id, ?Duration $timeout = null): Job` | Daemon read; job must belong to this queue and be nondeleted. |
| `listRuns(?RunQuery $query = null, ?Duration $timeout = null): Page` | Daemon read; `Page<RunSummary>` scoped to this queue; preserves local edits. |
| `listJobs(?JobState $state = null, ?JobType $type = null, bool $includeDeleted = false, int $limit = 100, ?Duration $timeout = null): Page` | Daemon read; `Page<Job>` scoped to this queue, limit 1–200. |
| `createCliJob(CliPayload $payload, Schedule $schedule, ?string $name = null, int $priority = 0, ?JobAttributes $attributes = null, ?string $idempotencyKey = null, ?Duration $timeout = null): Job` | Daemon mutation; creates definition and initial run, copies inputs. |
| `createHttpJob(HttpPayload $payload, Schedule $schedule, ?string $name = null, int $priority = 0, ?JobAttributes $attributes = null, ?string $idempotencyKey = null, ?Duration $timeout = null): Job` | Daemon mutation; creates definition and initial run, copies inputs. |
| `getId(): string` | Local cached field; never fetches or refreshes. |
| `getRevision(): int` | Local cached field; never fetches or refreshes. |
| `getName(): string` | Local cached field; never fetches or refreshes. |
| `setName(string $name): void` | Local edit; see field-specific constraints in the linked behavior topic. |
| `getState(): QueueState` | Local cached field; never fetches or refreshes. |
| `getWeight(): int` | Local cached field; never fetches or refreshes. |
| `setWeight(int $weight): void` | Local edit; see field-specific constraints in the linked behavior topic. |
| `getConcurrencyLimit(): int` | Local cached field; never fetches or refreshes. |
| `setConcurrencyLimit(int $concurrencyLimit): void` | Local edit; see field-specific constraints in the linked behavior topic. |
| `getRecoveryPolicy(): RecoveryPolicy` | Local cached field; never fetches or refreshes. |
| `setRecoveryPolicy(RecoveryPolicy $recoveryPolicy): void` | Local edit; see field-specific constraints in the linked behavior topic. |
| `getHistoryRetention(): ?Duration` | Local value; null inherits daemon policy, zero means unlimited. |
| `setHistoryRetention(?Duration $historyRetention): void` | Local edit; see field-specific constraints in the linked behavior topic. |
| `getRunnableWaitWarning(): Duration` | Local value; zero disables the warning. |
| `setRunnableWaitWarning(Duration $runnableWaitWarning): void` | Local edit; see field-specific constraints in the linked behavior topic. |
| `getCreatedAt(): \DateTimeImmutable` | Local cached field; never fetches or refreshes. |
| `getUpdatedAt(): \DateTimeImmutable` | Local cached field; never fetches or refreshes. |
| `getDeletedAt(): ?\DateTimeImmutable` | Local cached field; never fetches or refreshes. |
| `isDirty(): bool` | Local check for net changes from the accepted snapshot. |
| `isDeleted(): bool` | Local deletion knowledge; cached state/timestamps can predate a delete reply. |
| `getDefaults(): QueueDefaults` | Local detached deep copy, including pending edits. |
| `defaults(): QueueDefaults` | Local attached mutable defaults; stable identity. |
| `setDefaults(QueueDefaults $defaults): void` | Local replacement of the entire defaults layer, preserving attached identity. |
| `save(?Duration $timeout = null): void` | Revision-checked daemon mutation when dirty; clean save is local. Failure preserves edits. |
| `refresh(bool $includeDeleted = false, ?Duration $timeout = null): void` | Daemon read replacing the snapshot; rejects dirty state and preserves attached identity. |
| `discardChanges(): void` | Local restoration of the last accepted snapshot, including after Client closure. |
| `suspend(?Duration $timeout = null): void` | Daemon mutation; may return while draining; rejects dirty/deleted state. |
| `resume(?Duration $timeout = null): void` | Daemon mutation; rejects dirty/deleted state; adopts the returned snapshot. |
| `delete(?Duration $timeout = null): void` | Daemon mutation; marks locally deleted after success, retaining cached fields/revision. |

### Job

Cached definition obtained from Queue creation/lookup or job pages. The runner type is
fixed. Setters edit locally; save and lifecycle calls obey dirty-state, deletion, and
revision rules.

Final class. No public constructor. Behavior: [jobs and manual
runs](#jobs-and-manual-runs).

| Signature | Behavior |
| --- | --- |
| `getRun(string $id, ?Duration $timeout = null): Run` | Daemon read; retained run must belong to this job. |
| `listRuns(?RunQuery $query = null, ?Duration $timeout = null): Page` | Daemon read; `Page<RunSummary>` for this job, including retained deleted owners. |
| `getId(): string` | Local cached field; never fetches or refreshes. |
| `getQueueId(): string` | Local cached field; never fetches or refreshes. |
| `getRevision(): int` | Local cached field; never fetches or refreshes. |
| `getName(): ?string` | Local nullable display name; null means unnamed. |
| `getPriority(): int` | Local cached field; never fetches or refreshes. |
| `getState(): JobState` | Local cached field; never fetches or refreshes. |
| `getType(): JobType` | Local cached field; never fetches or refreshes. |
| `getCreatedAt(): \DateTimeImmutable` | Local cached field; never fetches or refreshes. |
| `getUpdatedAt(): \DateTimeImmutable` | Local cached field; never fetches or refreshes. |
| `getDeletedAt(): ?\DateTimeImmutable` | Local cached field; never fetches or refreshes. |
| `getSchedule(): Schedule` | Local cached field; never fetches or refreshes. |
| `getAttributes(): JobAttributes` | Local detached copy of typed attributes. |
| `attributes(): JobAttributes` | Local attached mutable attributes; stable identity. |
| `getPayload(): CliPayload\|HttpPayload` | Local detached copy; secret references remain symbolic. |
| `payload(): CliPayload\|HttpPayload` | Local attached mutable payload; stable identity. |
| `isDirty(): bool` | Local check for net changes from the accepted snapshot. |
| `isDeleted(): bool` | Local deletion knowledge; cached state/timestamps can predate a delete reply. |
| `setName(?string $name): void` | Local edit; null clears, otherwise UTF-8 without NUL, at most 256 bytes. |
| `setPriority(int $priority): void` | Local edit; signed 32-bit integer. |
| `setSchedule(Schedule $schedule): void` | Local replacement; creation-only symbolic now is rejected. |
| `setAttributes(JobAttributes $attributes): void` | Local merge of every supplied key; omitted keys remain unchanged. |
| `setPayload(CliPayload\|HttpPayload $payload): void` | Local replacement of every payload field; runner type must match. |
| `save(?Duration $timeout = null): void` | Revision-checked daemon mutation when dirty; clean save is local. Failure preserves edits. |
| `refresh(?Duration $timeout = null): void` | Daemon read replacing the snapshot; rejects dirty state and preserves attached identity. |
| `discardChanges(): void` | Local restoration of the last accepted snapshot, including after Client closure. |
| `suspend(?Duration $timeout = null): void` | Daemon mutation; may return while draining; rejects dirty/deleted state. |
| `resume(?Duration $timeout = null): void` | Daemon mutation; rejects dirty/deleted state; adopts the returned snapshot. |
| `move(Queue $queue, ?Duration $timeout = null): void` | Daemon mutation with revision; destination must be clean, nondeleted, same Client. |
| `delete(?Duration $timeout = null): void` | Daemon mutation; marks locally deleted after success, retaining cached fields/revision. |
| `runNow(?string $idempotencyKey = null, ?Duration $timeout = null): Run` | Daemon mutation; keyed additional manual run, no execution wait or Job refresh. |

### Run

Cached execution snapshot obtained from Job::runNow(), Job::getRun(), or
RunSummary::load(). Field getters and result access are local; output methods read
retained bytes without refreshing the snapshot.

Final class. No public constructor. Behavior: [retained run and attempt
history](#retained-run-and-attempt-history).

| Signature | Behavior |
| --- | --- |
| `getOutput(int $maxBytes = 4096, ?Duration $timeout = null): ?string` | Daemon reads; latest-attempt tail, 1–65,536 bytes; no snapshot refresh. |
| `getAttempt(int $number, ?Duration $timeout = null): Attempt` | Daemon read; attempt number 1–PHP_INT_MAX within this run. |
| `refresh(?Duration $timeout = null): void` | Daemon read replacing the cached snapshot; failure preserves it. |
| `cancel(?Duration $timeout = null): RunCancellationDisposition` | Daemon mutation; adopts returned snapshot/disposition without waiting for termination. |
| `listAttempts(int $limit = 100, ?Duration $timeout = null): Page` | Daemon read; `Page<AttemptSummary>`, descending number, limit 1–200. |
| `getId(): string` | Local cached field; never fetches or refreshes. |
| `getJobId(): string` | Local cached field; never fetches or refreshes. |
| `getQueueId(): string` | Local cached field; never fetches or refreshes. |
| `getJobRevision(): int` | Local cached field; never fetches or refreshes. |
| `getPriority(): int` | Local cached field; never fetches or refreshes. |
| `getOrigin(): RunOrigin` | Local cached field; never fetches or refreshes. |
| `getType(): JobType` | Local cached field; never fetches or refreshes. |
| `getState(): RunState` | Local cached field; never fetches or refreshes. |
| `isScheduleOwned(): bool` | Local cached field; never fetches or refreshes. |
| `getPlannedAt(): \DateTimeImmutable` | Local cached field; never fetches or refreshes. |
| `getRunnableAt(): \DateTimeImmutable` | Local cached field; never fetches or refreshes. |
| `getStartedAt(): ?\DateTimeImmutable` | Local cached field; never fetches or refreshes. |
| `getCompletedAt(): ?\DateTimeImmutable` | Local cached field; never fetches or refreshes. |
| `getAttributes(): JobAttributes` | Local detached copy of typed attributes. |
| `getPayload(): CliPayload\|HttpPayload` | Local detached copy; secret references remain symbolic. |
| `getResult(): ?ExecutionResult` | Local CliExecutionResult/HttpExecutionResult or null while unavailable. |

### Attempt

Cached attempt obtained from Run::getAttempt() or AttemptSummary::load(). Identify it by
run ID and attempt number. Result getters read the cache; output methods read retained
capture.

Final class. No public constructor. Behavior: [retained run and attempt
history](#retained-run-and-attempt-history).

| Signature | Behavior |
| --- | --- |
| `readOutput(OutputChannel $channel, int $offset = 0, int $length = 16384, ?Duration $timeout = null): OutputChunk` | Daemon read; retained-byte offset 0–PHP_INT_MAX−1, length 1–65,536. |
| `getOutput(int $maxBytes = 4096, ?Duration $timeout = null): ?string` | Daemon reads; this attempt’s tail, 1–65,536 bytes; no snapshot refresh. |
| `getRunId(): string` | Local cached field; never fetches or refreshes. |
| `getNumber(): int` | Local cached field; never fetches or refreshes. |
| `getDueAt(): \DateTimeImmutable` | Local cached field; never fetches or refreshes. |
| `getStartedAt(): ?\DateTimeImmutable` | Local cached field; never fetches or refreshes. |
| `getCompletedAt(): ?\DateTimeImmutable` | Local cached field; never fetches or refreshes. |
| `getState(): AttemptState` | Local cached field; never fetches or refreshes. |
| `getOutcome(): ?AttemptOutcome` | Local cached field; never fetches or refreshes. |
| `refresh(?Duration $timeout = null): void` | Daemon read replacing the cached snapshot; failure preserves it. |
| `getResult(): ?ExecutionResult` | Local CliExecutionResult/HttpExecutionResult or null while unavailable. |

### RunSummary

Immutable item from a run Page. It retains its Client and exposes only listed fields.
load() creates a separate full Run; it does not update the summary.

Final class. No public constructor. Behavior: [retained run and attempt
history](#retained-run-and-attempt-history).

| Signature | Behavior |
| --- | --- |
| `getId(): string` | Local cached field; never fetches or refreshes. |
| `getJobId(): string` | Local cached field; never fetches or refreshes. |
| `getQueueId(): string` | Local cached field; never fetches or refreshes. |
| `getJobRevision(): int` | Local cached field; never fetches or refreshes. |
| `getPriority(): int` | Local cached field; never fetches or refreshes. |
| `getOrigin(): RunOrigin` | Local cached field; never fetches or refreshes. |
| `getType(): JobType` | Local cached field; never fetches or refreshes. |
| `getState(): RunState` | Local cached field; never fetches or refreshes. |
| `isScheduleOwned(): bool` | Local cached field; never fetches or refreshes. |
| `getPlannedAt(): \DateTimeImmutable` | Local cached field; never fetches or refreshes. |
| `getRunnableAt(): \DateTimeImmutable` | Local cached field; never fetches or refreshes. |
| `getStartedAt(): ?\DateTimeImmutable` | Local cached field; never fetches or refreshes. |
| `getCompletedAt(): ?\DateTimeImmutable` | Local cached field; never fetches or refreshes. |
| `load(?Duration $timeout = null): Run` | Daemon read returning a new full resource; the summary remains unchanged. |

### AttemptSummary

Immutable item from Run::listAttempts(). It retains its Client and has no result/output
accessors. load() creates a separate full Attempt.

Final class. No public constructor. Behavior: [retained run and attempt
history](#retained-run-and-attempt-history).

| Signature | Behavior |
| --- | --- |
| `getRunId(): string` | Local cached field; never fetches or refreshes. |
| `getNumber(): int` | Local cached field; never fetches or refreshes. |
| `getDueAt(): \DateTimeImmutable` | Local cached field; never fetches or refreshes. |
| `getStartedAt(): ?\DateTimeImmutable` | Local cached field; never fetches or refreshes. |
| `getCompletedAt(): ?\DateTimeImmutable` | Local cached field; never fetches or refreshes. |
| `getState(): AttemptState` | Local cached field; never fetches or refreshes. |
| `getOutcome(): ?AttemptOutcome` | Local cached field; never fetches or refreshes. |
| `load(?Duration $timeout = null): Attempt` | Daemon read returning a new full resource; the summary remains unchanged. |

### Page

A list method returns Page positioned before its eagerly fetched first batch. It is
advanced explicitly and is not a PHP Iterator/Traversable. items() is a list of the
source-specific item type; previously returned items remain usable.

Final class. No public constructor. Behavior: [definition lists and
pages](#definition-lists-and-pages).

| Signature | Behavior |
| --- | --- |
| `hasNext(): bool` | Local check for buffered page or continuation. |
| `next(?Duration $timeout = null): void` | First advance is local; later advances fetch atomically. Throws if exhausted. |
| `items(): array` | Local list of current items; [] before first next(). Array edits are independent. |
| `getStatisticsMetadata(): ?StatisticsMetadata` | Local buffered/current metadata; null for non-statistics sources. |

### Duration

Immutable signed millisecond duration. Use the static factories; days are fixed 24-hour
intervals. Checked overflow raises ValueError. Negative/zero values are representable,
but timeouts must be positive.

Final class. No public constructor. Behavior: [immutable value
primitives](#immutable-value-primitives).

| Signature | Behavior |
| --- | --- |
| `getMilliseconds(): int` | Local exact signed millisecond count. |
| `static milliseconds(int $value): Duration` | Local immutable factory; checked conversion to integer milliseconds. |
| `static seconds(int $value): Duration` | Local immutable factory; checked conversion to integer milliseconds. |
| `static minutes(int $value): Duration` | Local immutable factory; checked conversion to integer milliseconds. |
| `static hours(int $value): Duration` | Local immutable factory; checked conversion to integer milliseconds. |
| `static days(int $value): Duration` | Local immutable factory; checked conversion to integer milliseconds. |

### SecretReference

Immutable symbolic secret name. Construction validates the name locally; no getter
resolves or returns the underlying secret.

Final class. Behavior: [immutable value primitives](#immutable-value-primitives).

| Signature | Behavior |
| --- | --- |
| `__construct(string $name)` | Local construction; copies supplied inputs. See the constraints above. |
| `getName(): string` | Local value; no daemon request. |

### Schedule

Abstract schedule base with no public methods of its own. Use OnceSchedule or
CronSchedule; custom schedule implementations are not supported inputs.

Abstract class. Behavior: [immutable value primitives](#immutable-value-primitives).

### OnceSchedule

Immutable absolute instant, or creation-only symbolic now. Copies the input instant;
getAt() returns a fresh UTC date or null for symbolic now.

Inherits [Schedule](#schedule). Final class. Behavior: [immutable value
primitives](#immutable-value-primitives).

| Signature | Behavior |
| --- | --- |
| `__construct(\DateTimeInterface $at)` | Local construction; copies supplied inputs. See the constraints above. |
| `static now(): OnceSchedule` | Local immutable factory; symbolic marker accepted only at job creation. |
| `isNow(): bool` | Local value; no daemon request. |
| `getAt(): ?\DateTimeImmutable` | Local fresh UTC DateTimeImmutable, or null for symbolic now. |

### CronSchedule

Immutable cron expression and optional named timezone. Construction is local. Use Client
helpers to validate grammar and preview occurrences; null timezone means the daemon
default.

Inherits [Schedule](#schedule). Final class. Behavior: [secrets and cron
helpers](#secrets-and-cron-helpers).

| Signature | Behavior |
| --- | --- |
| `__construct(string $expression, ?\DateTimeZone $timezone = null)` | Local construction; copies supplied inputs. See the constraints above. |
| `getExpression(): string` | Local value; no daemon request. |
| `getTimezone(): ?\DateTimeZone` | Local fresh DateTimeZone, or null for unresolved daemon default. |

### JobAttributes

Partial local attribute layer supporting common and runner-specific names. Absent values
return null. Typed subclasses restrict runner applicability. Mutable and cloneable;
attached edits mark the owning resource dirty.

Behavior: [mutable attributes and queue
defaults](#mutable-attributes-and-queue-defaults).

| Signature | Behavior |
| --- | --- |
| `__construct()` | Local construction; copies supplied inputs. See the constraints above. |
| `get(string $name): mixed` | Local supplied attribute value, or null if absent; unknown names raise ValueError. |
| `has(string $name): bool` | Local presence check; unknown/inapplicable names raise ValueError. |
| `set(string $name, mixed $value): void` | Validate and edit one local attribute; see the attribute names, types, and bounds in the linked topic. |
| `getTimeout(): ?Duration` | Local supplied attribute, or null if absent; see the attribute table. |
| `setTimeout(Duration $value): void` | Local edit; see field-specific constraints in the linked behavior topic. |
| `getOutputCapture(): ?string` | Local supplied attribute, or null if absent; see the attribute table. |
| `setOutputCapture(string $value): void` | Local edit; see field-specific constraints in the linked behavior topic. |
| `getRetryInitialDelay(): ?Duration` | Local supplied attribute, or null if absent; see the attribute table. |
| `setRetryInitialDelay(Duration $value): void` | Local edit; see field-specific constraints in the linked behavior topic. |
| `getRetryMaxDelay(): ?Duration` | Local supplied attribute, or null if absent; see the attribute table. |
| `setRetryMaxDelay(Duration $value): void` | Local edit; see field-specific constraints in the linked behavior topic. |
| `getRetryMaxAttempts(): ?int` | Local supplied attribute, or null if absent; see the attribute table. |
| `setRetryMaxAttempts(int $value): void` | Local edit; see field-specific constraints in the linked behavior topic. |
| `getRetryJitter(): ?float` | Local supplied attribute, or null if absent; see the attribute table. |
| `setRetryJitter(float $value): void` | Local edit; see field-specific constraints in the linked behavior topic. |
| `getRetryMultiplier(): ?float` | Local supplied attribute, or null if absent; see the attribute table. |
| `setRetryMultiplier(float $value): void` | Local edit; see field-specific constraints in the linked behavior topic. |
| `getRetryMode(): ?RetryMode` | Local supplied attribute, or null if absent; see the attribute table. |
| `setRetryMode(RetryMode $value): void` | Local edit; see field-specific constraints in the linked behavior topic. |
| `getRetryStrategy(): ?string` | Local supplied attribute, or null if absent; see the attribute table. |
| `setRetryStrategy(string $value): void` | Local edit; see field-specific constraints in the linked behavior topic. |

### CliJobAttributes

Adds CLI-specific typed accessors to JobAttributes. Inherits its constructor, generic
operations, and common typed accessors. Also returned as the attached CLI view of
QueueDefaults.

Inherits [JobAttributes](#jobattributes). Final class. Behavior: [mutable attributes and
queue defaults](#mutable-attributes-and-queue-defaults).

| Signature | Behavior |
| --- | --- |
| `getRetryExitCodes(): ?array` | Local supplied attribute, or null if absent; see the attribute table. |
| `setRetryExitCodes(array $value): void` | Local edit; see field-specific constraints in the linked behavior topic. |
| `getTerminationGrace(): ?Duration` | Local supplied attribute, or null if absent; see the attribute table. |
| `setTerminationGrace(Duration $value): void` | Local edit; see field-specific constraints in the linked behavior topic. |
| `getStdoutLimit(): ?int` | Local supplied attribute, or null if absent; see the attribute table. |
| `setStdoutLimit(int $value): void` | Local edit; see field-specific constraints in the linked behavior topic. |
| `getStderrLimit(): ?int` | Local supplied attribute, or null if absent; see the attribute table. |
| `setStderrLimit(int $value): void` | Local edit; see field-specific constraints in the linked behavior topic. |

### HttpJobAttributes

Adds HTTP-specific typed accessors to JobAttributes. Inherits its constructor, generic
operations, and common typed accessors. Also returned as the attached HTTP view of
QueueDefaults.

Inherits [JobAttributes](#jobattributes). Final class. Behavior: [mutable attributes and
queue defaults](#mutable-attributes-and-queue-defaults).

| Signature | Behavior |
| --- | --- |
| `getFollowRedirects(): ?bool` | Local supplied attribute, or null if absent; see the attribute table. |
| `setFollowRedirects(bool $value): void` | Local edit; see field-specific constraints in the linked behavior topic. |
| `getIdempotencyKey(): ?bool` | Local supplied attribute, or null if absent; see the attribute table. |
| `setIdempotencyKey(bool $value): void` | Local edit; see field-specific constraints in the linked behavior topic. |
| `getTlsVerify(): ?bool` | Local supplied attribute, or null if absent; see the attribute table. |
| `setTlsVerify(bool $value): void` | Local edit; see field-specific constraints in the linked behavior topic. |
| `getMaxRedirects(): ?int` | Local supplied attribute, or null if absent; see the attribute table. |
| `setMaxRedirects(int $value): void` | Local edit; see field-specific constraints in the linked behavior topic. |
| `getHttpBodyLimit(): ?int` | Local supplied attribute, or null if absent; see the attribute table. |
| `setHttpBodyLimit(int $value): void` | Local edit; see field-specific constraints in the linked behavior topic. |
| `getHttpHeadersLimit(): ?int` | Local supplied attribute, or null if absent; see the attribute table. |
| `setHttpHeadersLimit(int $value): void` | Local edit; see field-specific constraints in the linked behavior topic. |
| `getRetryErrors(): ?array` | Local supplied attribute, or null if absent; see the attribute table. |
| `setRetryErrors(array $value): void` | Local edit; see field-specific constraints in the linked behavior topic. |
| `getRetryStatuses(): ?array` | Local supplied attribute, or null if absent; see the attribute table. |
| `setRetryStatuses(array $value): void` | Local edit; see field-specific constraints in the linked behavior topic. |

### QueueDefaults

Partial defaults layer shared by its CLI and HTTP views. Common attributes set through
either view are visible through both. clear() removes one override; replacing queue
defaults replaces the whole layer.

Final class. Behavior: [mutable attributes and queue
defaults](#mutable-attributes-and-queue-defaults).

| Signature | Behavior |
| --- | --- |
| `__construct()` | Local construction; copies supplied inputs. See the constraints above. |
| `get(string $name): mixed` | Local supplied attribute value, or null if absent; unknown names raise ValueError. |
| `has(string $name): bool` | Local presence check; unknown/inapplicable names raise ValueError. |
| `set(string $name, mixed $value): void` | Validate and edit one local attribute; see the attribute names, types, and bounds in the linked topic. |
| `clear(string $name): void` | Local override removal; an absent known name is a no-op. |
| `cli(): CliJobAttributes` | Local shared CLI view; repeated calls return the same object. |
| `http(): HttpJobAttributes` | Local shared HTTP view; repeated calls return the same object. |

### CliPayload

Mutable CLI template. Construction parses a command and appends literal arguments. Local
setters never execute it; defaults are directory /, no arguments/environment overrides,
and expected exit code 0. Clones detach from any Job.

Final class. Behavior: [cli payloads](#cli-payloads).

| Signature | Behavior |
| --- | --- |
| `__construct(string $command, array $arguments = [])` | Local construction; copies supplied inputs. See the constraints above. |
| `setCommand(string $command, array $arguments = []): void` | Local atomic replacement of executable/arguments; preserves other fields. |
| `getExecutable(): string` | Local value; no daemon request. |
| `setExecutable(string $executable): void` | Local literal executable replacement; preserves arguments. |
| `getArguments(): array` | Local `list<string\|SecretReference>`, in argv order. |
| `setArguments(array $arguments): void` | Local edit; see field-specific constraints in the linked behavior topic. |
| `addArgument(string\|SecretReference $argument): void` | Local edit; see field-specific constraints in the linked behavior topic. |
| `getEnvironment(): array` | Local `array<string,string\|SecretReference\|null>`; null entries explicitly unset variables. |
| `setEnvironment(string $name, string\|SecretReference $value): void` | Local edit; see field-specific constraints in the linked behavior topic. |
| `unsetEnvironment(string $name): void` | Local explicit removal marker for execution. |
| `clearEnvironment(string $name): void` | Local removal of the payload override, distinct from an explicit unset. |
| `getWorkingDirectory(): string` | Local value; no daemon request. |
| `setWorkingDirectory(string $directory): void` | Local edit; see field-specific constraints in the linked behavior topic. |
| `getExpectedExitCodes(): array` | Local sorted `list<int>`; defaults to [0]. |
| `setExpectedExitCodes(array $codes): void` | Local edit; see field-specific constraints in the linked behavior topic. |

### HttpPayload

Mutable HTTP template. Defaults are GET, no headers/body, and expected statuses 200–299.
Body bytes and ordered duplicate headers are preserved. Clones detach from any Job.

Final class. Behavior: [http payload configuration](#http-payload-configuration).

| Signature | Behavior |
| --- | --- |
| `__construct(string $url, string $method = 'GET')` | Local construction; copies supplied inputs. See the constraints above. |
| `getUrl(): string` | Local value; no daemon request. |
| `setUrl(string $url): void` | Local edit; see field-specific constraints in the linked behavior topic. |
| `getMethod(): string` | Local value; no daemon request. |
| `setMethod(string $method): void` | Local edit; see field-specific constraints in the linked behavior topic. |
| `addHeader(string $name, string\|SecretReference $value, bool $sensitive = false): void` | Local edit; see field-specific constraints in the linked behavior topic. |
| `getHeaders(): array` | Local `list<HttpHeader>`; detached snapshots in insertion order. |
| `clearHeader(string $name): void` | Local removal of every case-insensitive match; preserves other header order. |
| `setBody(string\|SecretReference $body): void` | Local edit; see field-specific constraints in the linked behavior topic. |
| `getBody(): string\|SecretReference\|null` | Local binary string or symbolic reference; null means absent, distinct from empty. |
| `clearBody(): void` | Local edit; see field-specific constraints in the linked behavior topic. |
| `setJsonBody(mixed $value): void` | Local JSON encoding/body update; may throw JsonException or callback exceptions. |
| `getExpectedStatuses(): array` | Local `list<string>` of status selectors; defaults to ['200-299']. |
| `setExpectedStatuses(array $statuses): void` | Local edit; see field-specific constraints in the linked behavior topic. |

### HttpHeader

Immutable detached item returned by HttpPayload::getHeaders(). Sensitivity controls
forwarding; literal values remain readable and SecretReference values remain symbolic.

Final class. No public constructor. Behavior: [http payload
configuration](#http-payload-configuration).

| Signature | Behavior |
| --- | --- |
| `getName(): string` | Local value; no daemon request. |
| `getValue(): string\|SecretReference` | Local value; no daemon request. |
| `isSensitive(): bool` | Local value; no daemon request. |

### TimeRange

Immutable half-open interval: include from, exclude to. Bounds may be null; if both
exist, from must precede to. Copies dates and returns fresh UTC dates.

Final class. Behavior: [queries metadata and execution
results](#queries-metadata-and-execution-results).

| Signature | Behavior |
| --- | --- |
| `__construct(?\DateTimeInterface $from = null, ?\DateTimeInterface $to = null)` | Local construction; copies supplied inputs. See the constraints above. |
| `getFrom(): ?\DateTimeImmutable` | Local value; no daemon request. |
| `getTo(): ?\DateTimeImmutable` | Local value; no daemon request. |

### RunQuery

Immutable filters combined with AND. state and states are mutually exclusive; states is
null or a nonempty distinct list of RunState values. Null ranges become unbounded
TimeRange values. Scoped listings validate receiver IDs.

Final class. Behavior: [queries metadata and execution
results](#queries-metadata-and-execution-results).

| Signature | Behavior |
| --- | --- |
| `__construct(?string $queueId = null, ?string $jobId = null, ?RunState $state = null, ?array $states = null, ?RunOrigin $origin = null, ?JobType $type = null, ?TimeRange $planned = null, ?TimeRange $started = null, ?TimeRange $completed = null, int $limit = 100)` | Local construction; copies supplied inputs. See the constraints above. |
| `getQueueId(): ?string` | Local value; no daemon request. |
| `getJobId(): ?string` | Local value; no daemon request. |
| `getState(): ?RunState` | Local value; no daemon request. |
| `getStates(): ?array` | Local null or `list<RunState>`; independent copied selection. |
| `getOrigin(): ?RunOrigin` | Local value; no daemon request. |
| `getType(): ?JobType` | Local value; no daemon request. |
| `getPlanned(): TimeRange` | Local value; no daemon request. |
| `getStarted(): TimeRange` | Local value; no daemon request. |
| `getCompleted(): TimeRange` | Local value; no daemon request. |
| `getLimit(): int` | Local value; no daemon request. |

### StatisticsQuery

Immutable filters for retained run statistics. A supplied window is at most 31 days. The
daemon resolves omitted bounds when queried. Scoped Queue statistics requires a matching
or absent queueId.

Final class. Behavior: [system and statistics](#system-and-statistics).

| Signature | Behavior |
| --- | --- |
| `__construct(?string $queueId = null, ?string $jobId = null, ?JobType $type = null, ?RunOrigin $origin = null, ?TimeRange $planned = null, StatisticsGroupBy $groupBy = StatisticsGroupBy::None, int $limit = 100)` | Local construction; copies supplied inputs. See the constraints above. |
| `getQueueId(): ?string` | Local value; no daemon request. |
| `getJobId(): ?string` | Local value; no daemon request. |
| `getType(): ?JobType` | Local value; no daemon request. |
| `getOrigin(): ?RunOrigin` | Local value; no daemon request. |
| `getPlanned(): TimeRange` | Local value; no daemon request. |
| `getGroupBy(): StatisticsGroupBy` | Local value; no daemon request. |
| `getLimit(): int` | Local value; no daemon request. |

### SystemInfo

Detached immutable daemon version/API/capability snapshot returned by
Client::getSystemInfo(). All getters are local. Capabilities are a sorted unique list of
strings.

Final class. No public constructor. Behavior: [system and
statistics](#system-and-statistics).

| Signature | Behavior |
| --- | --- |
| `getDaemonVersion(): string` | Local value; no daemon request. |
| `getApiMajor(): int` | Local value; no daemon request. |
| `getApiMinor(): int` | Local value; no daemon request. |
| `getCapabilities(): array` | Local sorted unique `list<string>`. |
| `hasCapability(string $name): bool` | Local exact capability-name check. |

### SecretMetadata

Detached immutable name and UTC timestamps from setSecret() or listSecrets(). It
contains no secret bytes, preview, digest, or value length.

Final class. No public constructor. Behavior: [secrets and cron
helpers](#secrets-and-cron-helpers).

| Signature | Behavior |
| --- | --- |
| `getName(): string` | Local value; no daemon request. |
| `getCreatedAt(): \DateTimeImmutable` | Local value; no daemon request. |
| `getUpdatedAt(): \DateTimeImmutable` | Local value; no daemon request. |

### ExecutionResult

Abstract base of safe cached execution metadata. Obtain a concrete subclass from
Run::getResult() or Attempt::getResult(). Optional typed observations can be null;
generic access preserves safe unknown fields. This is not the application response body.

Abstract class. No public constructor. Behavior: [queries metadata and execution
results](#queries-metadata-and-execution-results).

| Signature | Behavior |
| --- | --- |
| `getType(): JobType` | Local value; no daemon request. |
| `getOutcome(): ?string` | Local nullable runner vocabulary string; not an AttemptOutcome enum. |
| `getReason(): ?string` | Local value; no daemon request. |
| `getErrorCode(): ?string` | Local value; no daemon request. |
| `getOutcomeUnknown(): ?bool` | Local nullable execution metadata, separate from management OutcomeUnknownException. |
| `has(string $field): bool` | Local presence test; distinguishes absent fields from explicit null. |
| `get(string $field): mixed` | Local copied safe value; missing returns null; objects become stdClass, arrays become lists. |
| `getCapturedBytes(OutputChannel $channel): ?int` | Local value; no daemon request. |
| `getTotalBytes(OutputChannel $channel): ?int` | Local value; no daemon request. |
| `isTruncated(OutputChannel $channel): ?bool` | Local value; no daemon request. |

### CliExecutionResult

Immutable CLI result. Inherits ExecutionResult accessors. Exit code, signal, and
capture-loss observations are nullable; the presence of a result does not guarantee a
normal process exit.

Inherits [ExecutionResult](#executionresult). Final class. Behavior: [queries metadata
and execution results](#queries-metadata-and-execution-results).

| Signature | Behavior |
| --- | --- |
| `getExitCode(): ?int` | Local value; no daemon request. |
| `getSignal(): ?int` | Local value; no daemon request. |
| `getCaptureLost(): ?bool` | Local value; no daemon request. |

### HttpExecutionResult

Immutable HTTP result. Inherits ExecutionResult accessors. Transport failures may have
no status; absent optional observations remain null.

Inherits [ExecutionResult](#executionresult). Final class. Behavior: [queries metadata
and execution results](#queries-metadata-and-execution-results).

| Signature | Behavior |
| --- | --- |
| `getStatus(): ?int` | Local value; no daemon request. |
| `getDuration(): ?Duration` | Local value; no daemon request. |
| `getRedirects(): ?int` | Local value; no daemon request. |
| `getTlsVerified(): ?bool` | Local value; no daemon request. |
| `getErrorCategory(): ?string` | Local value; no daemon request. |
| `getRetryAfterAccepted(): ?bool` | Local value; no daemon request. |
| `getRetryAfterClamped(): ?bool` | Local value; no daemon request. |

### OutputChunk

Detached immutable slice from Attempt::readOutput(). Data is binary-safe. Read status,
loss, truncation, and byte counts together; an empty string alone does not establish
captured-empty output.

Final class. No public constructor. Behavior: [retained output](#retained-output).

| Signature | Behavior |
| --- | --- |
| `getRunId(): string` | Local value; no daemon request. |
| `getAttemptNumber(): int` | Local value; no daemon request. |
| `getChannel(): OutputChannel` | Local value; no daemon request. |
| `getStatus(): OutputStatus` | Local value; no daemon request. |
| `getOffset(): int` | Local value; no daemon request. |
| `getBytesReturned(): int` | Local value; no daemon request. |
| `getNextOffset(): ?int` | Local next retained-byte offset, or null at EOF/no further slice. |
| `getRetainedBytes(): int` | Local value; no daemon request. |
| `getTotalBytes(): ?int` | Local value; no daemon request. |
| `getOmittedBytes(): ?int` | Local value; no daemon request. |
| `isTruncated(): bool` | Local value; no daemon request. |
| `isCaptureLost(): bool` | Local value; no daemon request. |
| `getData(): string` | Local raw bytes, including NUL. |

### StatisticsGroup

Detached immutable item from systemStatistics() or Queue::statistics(). The key type
depends on groupBy; None uses null, Queue/Job use UUID strings, and Type/Origin/State
use their enums.

Final class. No public constructor. Behavior: [system and
statistics](#system-and-statistics).

| Signature | Behavior |
| --- | --- |
| `getKey(): string\|JobType\|RunOrigin\|RunState\|null` | Local grouping key: UUID, backed enum, or null for ungrouped statistics. |
| `getRuns(): StatisticsRunCounts` | Local value; no daemon request. |
| `getAttempts(): StatisticsAttemptCounts` | Local value; no daemon request. |
| `getCapture(): StatisticsCaptureCounts` | Local value; no daemon request. |
| `getScheduleLateness(): StatisticsDuration` | Local value; no daemon request. |
| `getExecutionWallDuration(): StatisticsDuration` | Local value; no daemon request. |
| `getRunnableWait(): ?StatisticsDuration` | Local duration aggregate; null if measurement is unavailable. |
| `getRunnableWaitCoverage(): StatisticsRunnableWaitCoverage` | Local value; no daemon request. |

### StatisticsRunCounts

Retained run totals and counters returned by StatisticsGroup::getRuns(). Every enum
category has a counter, including zero.

Final class. No public constructor. Behavior: [system and
statistics](#system-and-statistics).

| Signature | Behavior |
| --- | --- |
| `getTotal(): int` | Local value; no daemon request. |
| `getStateCount(RunState $state): int` | Local value; no daemon request. |
| `getTypeCount(JobType $type): int` | Local value; no daemon request. |
| `getOriginCount(RunOrigin $origin): int` | Local value; no daemon request. |

### StatisticsAttemptCounts

Counts all retained attempts for the selected runs, including retries outside the
selected planned-time window. Returned by StatisticsGroup::getAttempts().

Final class. No public constructor. Behavior: [system and
statistics](#system-and-statistics).

| Signature | Behavior |
| --- | --- |
| `getTotal(): int` | Local value; no daemon request. |
| `getStateCount(AttemptState $state): int` | Local value; no daemon request. |
| `getOutcomeCount(AttemptOutcome $outcome): int` | Local value; no daemon request. |
| `getRetries(): int` | Local value; no daemon request. |

### StatisticsCaptureCounts

Persisted truncation/loss event counts from StatisticsGroup::getCapture(). A missing
output row proves neither event.

Final class. No public constructor. Behavior: [system and
statistics](#system-and-statistics).

| Signature | Behavior |
| --- | --- |
| `getTruncatedAttempts(): int` | Local value; no daemon request. |
| `getLostAttempts(): int` | Local value; no daemon request. |

### StatisticsDuration

Sample count and fractional milliseconds from a StatisticsGroup duration getter. No
samples means zero samples and null average/maximum; a measured zero has a sample and
0.0 values.

Final class. No public constructor. Behavior: [system and
statistics](#system-and-statistics).

| Signature | Behavior |
| --- | --- |
| `getSamples(): int` | Local value; no daemon request. |
| `getAverageMilliseconds(): ?float` | Local value; no daemon request. |
| `getMaximumMilliseconds(): ?float` | Local value; no daemon request. |

### StatisticsRunnableWaitCoverage

Mutually exclusive Complete, Partial, Unmeasured, and Unfinished run counts, summing to
the run total. Only Complete terminal runs supply runnable-wait duration samples.

Final class. No public constructor. Behavior: [system and
statistics](#system-and-statistics).

| Signature | Behavior |
| --- | --- |
| `getComplete(): int` | Local value; no daemon request. |
| `getPartial(): int` | Local value; no daemon request. |
| `getUnmeasured(): int` | Local value; no daemon request. |
| `getUnfinished(): int` | Local value; no daemon request. |

### StatisticsMetadata

Detached context from Page::getStatisticsMetadata(), available before first next() and
for empty statistics pages. Its window bounds are resolved; the window stays fixed
during continuation.

Final class. No public constructor. Behavior: [system and
statistics](#system-and-statistics).

| Signature | Behavior |
| --- | --- |
| `getWindow(): TimeRange` | Local value; no daemon request. |
| `getGroupBy(): StatisticsGroupBy` | Local value; no daemon request. |
| `getMeasurement(): StatisticsMeasurement` | Local value; no daemon request. |

### StatisticsMeasurement

Provenance from StatisticsMetadata::getMeasurement(). Timing currently reports
wall_clock_derived; runnable wait reports monotonic_observed or unavailable; capture
reports persisted_output_flags. Future safe strings remain readable.

Final class. No public constructor. Behavior: [system and
statistics](#system-and-statistics).

| Signature | Behavior |
| --- | --- |
| `getTiming(): string` | Local value; no daemon request. |
| `getRunnableWait(): string` | Local value; no daemon request. |
| `getCapture(): string` | Local value; no daemon request. |

### Exception

Base operational error; extends PHP \Exception and inherits its constructor,
getMessage(), integer getCode(), getPrevious(), and trace methods. JobU string error
metadata is separate from that integer code.

Behavior: [exceptions](#exceptions).

| Signature | Behavior |
| --- | --- |
| `getErrorCode(): ?string` | Local stable daemon error string, or null when unavailable. |
| `getCategory(): ?string` | Local stable daemon category, or null when unavailable. |

### ConnectionException

Connection or handshake failure, including use of an explicitly closed or unusable
Client.

Inherits [Exception](#exception). Behavior: [exceptions](#exceptions).

### TimeoutException

The operation deadline expired. This does not cancel external work or prove rollback.

Inherits [Exception](#exception). Behavior: [exceptions](#exceptions).

### NotFoundException

The requested resource is missing, purged, deleted where disallowed, or outside the
receiving resource scope.

Inherits [Exception](#exception). Behavior: [exceptions](#exceptions).

### RevisionConflictException

The accepted revision is stale. A failed save preserves local edits and the old
revision.

Inherits [Exception](#exception). Behavior: [exceptions](#exceptions).

### InvalidCursorException

A history/statistics continuation is expired, evicted, restarted away, or otherwise
invalid. Enumeration is not restarted automatically.

Inherits [Exception](#exception). Behavior: [exceptions](#exceptions).

### UnsavedChangesException

A guarded refresh or lifecycle action was attempted with pending edits. Save or discard
first.

Inherits [Exception](#exception). Behavior: [exceptions](#exceptions).

### OutcomeUnknownException

A mutation may have committed. Retains its operation and original idempotency key when
available; use independent observations for reconciliation.

Inherits [Exception](#exception). Behavior: [exceptions](#exceptions).

| Signature | Behavior |
| --- | --- |
| `getOperation(): ?string` | Local stable operation name, or null for a user-created exception. |
| `getIdempotencyKey(): ?string` | Local original key, or null when unavailable. Retain for reconciliation. |

### UnsupportedFeatureException

The daemon API version or capabilities do not support the requested operation.

Inherits [Exception](#exception). Behavior: [exceptions](#exceptions).

## Enum reference

All enums below are string-backed and cannot be subclassed. Use `Case->name` for its PHP
case name and `Case->value` for its backing string. Each supports `static cases():
array` (a list of that enum's cases), `static from(int|string $value): static`, and
`static tryFrom(int|string $value): ?static`. Use the documented strings for these
string-backed enums. An unknown string makes `from()` throw ValueError and `tryFrom()`
return null; PHP's normal strict/weak scalar rules apply. An unfinished attempt's
outcome is null, not an additional enum case.

```php
$type = JobU\JobType::from('cli');
echo $type->name, ': ', $type->value, "\n"; // Cli: cli
$unknown = JobU\JobType::tryFrom('custom'); // null
```

### AttemptOutcome

Terminal attempt outcome; unfinished attempts return null.

| Case | Backing string |
| --- | --- |
| `Succeeded` | `succeeded` |
| `Failed` | `failed` |
| `Interrupted` | `interrupted` |
| `Cancelled` | `cancelled` |

### AttemptState

Attempt lifecycle state; use AttemptOutcome for a completed attempt’s result.

| Case | Backing string |
| --- | --- |
| `Pending` | `pending` |
| `Running` | `running` |
| `Completed` | `completed` |

### JobState

Job definition lifecycle state, including terminal one-shot definitions.

| Case | Backing string |
| --- | --- |
| `Active` | `active` |
| `Suspending` | `suspending` |
| `Suspended` | `suspended` |
| `Deleted` | `deleted` |
| `Succeeded` | `succeeded` |
| `Failed` | `failed` |
| `Cancelled` | `cancelled` |

### JobType

Immutable runner family; Jobs cannot change type.

| Case | Backing string |
| --- | --- |
| `Cli` | `cli` |
| `Http` | `http` |

### OutputChannel

CLI uses Stdout/Stderr; HTTP uses Body/Headers.

| Case | Backing string |
| --- | --- |
| `Stdout` | `stdout` |
| `Stderr` | `stderr` |
| `Body` | `body` |
| `Headers` | `headers` |

### OutputStatus

Capture availability. Lost may still include retained bytes; Pending is not a live
stream.

| Case | Backing string |
| --- | --- |
| `Available` | `available` |
| `Pending` | `pending` |
| `NotCaptured` | `not_captured` |
| `Lost` | `lost` |

### QueueState

Queue lifecycle state. Suspending can mean work is still draining.

| Case | Backing string |
| --- | --- |
| `Active` | `active` |
| `Suspending` | `suspending` |
| `Suspended` | `suspended` |
| `Deleted` | `deleted` |

### RecoveryPolicy

Daemon restart treatment of interrupted attempts.

| Case | Backing string |
| --- | --- |
| `FailInterrupted` | `fail_interrupted` |
| `RetryInterrupted` | `retry_interrupted` |

### RetryMode

Blocking holds queue capacity during a retry wait; Reschedule releases it.

| Case | Backing string |
| --- | --- |
| `Blocking` | `blocking` |
| `Reschedule` | `reschedule` |

### RunCancellationDisposition

Completed acknowledges completed cancellation; Requested may still leave the cached Run
running.

| Case | Backing string |
| --- | --- |
| `Completed` | `completed` |
| `Requested` | `requested` |

### RunOrigin

Scheduled runs versus additional runs created with runNow().

| Case | Backing string |
| --- | --- |
| `Scheduled` | `scheduled` |
| `Manual` | `manual` |

### RunState

Execution lifecycle state; RetryWait is distinct from active execution.

| Case | Backing string |
| --- | --- |
| `Scheduled` | `scheduled` |
| `Running` | `running` |
| `RetryWait` | `retry_wait` |
| `Succeeded` | `succeeded` |
| `Failed` | `failed` |
| `Interrupted` | `interrupted` |
| `Cancelled` | `cancelled` |

### StatisticsGroupBy

Selects statistics grouping and the resulting StatisticsGroup key type.

| Case | Backing string |
| --- | --- |
| `None` | `none` |
| `Queue` | `queue` |
| `Job` | `job` |
| `Type` | `type` |
| `Origin` | `origin` |
| `State` | `state` |
