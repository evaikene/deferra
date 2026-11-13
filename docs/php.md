# JobU PHP extension

The optional `jobu` extension provides native string-backed enums, an exception
hierarchy, immutable durations, secret references and schedules, mutable attributes
and queue defaults, mutable CLI/HTTP payloads, and request-scoped `Client` connections.
The constructor eagerly connects and handshakes. Queue lookup, creation, local
edits and lifecycle operations are available through the synchronous bridge.
Job creation, active Job resources and cached Run Now snapshots are also available.
Queue/job definition lists and retained Run/Attempt history return prefetched pages.
Run cancellation, explicit history loading, typed execution results and bounded output reads are available.
Secret/schedule management and statistics remain later stages.
It supports PHP 8.1+ with 64-bit integers in NTS mode
on Linux/macOS. Runtime validation covers PHP 8.2–8.6 NTS; PHP 8.1 retains the
minimum build/API contract but is not in the test matrix. macOS runtime coverage
must be recorded separately. The independent [php-cxx helper](php-cxx.md) also
supports ZTS; the JobU extension rejects ZTS during configuration.

## Build and load

PHP features default to off. Enabling the extension also builds the helper,
without requiring `JB_BUILD_PHP_CPP=ON`:

```sh
cmake -S . -B .bld-php -DJB_BUILD_PHP_EXTENSION=ON \
    -DPHP_CONFIG=/path/to/php/bin/php-config
cmake --build .bld-php
/path/to/php/bin/php -n -dextension="$PWD/.bld-php/src/jobu-php/jobu.so" --ri jobu
ctest --test-dir .bld-php/test --output-on-failure
```

Use a separate build directory for each PHP ABI and select `PHP_CONFIG`
explicitly. Its headers and CLI must belong to the same installation. PHP
version, debug mode, thread mode, and integer width are discovered from the
selected development files; configuration does not execute PHP or link Embed.
Zend checks the module ABI when loading it. Neither the module nor the helper
has a production Embed dependency. Embed is optional for production builds. The module lifecycle regression needs
matching Embed and a matching built daemon; CI requires both to prevent skipped
checks.

To reuse a previously built native SDK, configure the standalone module entry
point. This rebuilds only PHP-dependent sources and imports the Client SDK:

```sh
cmake -S src/jobu-php -B .bld-php-module \
    -DPHP_CONFIG=/path/to/php/bin/php-config \
    -DCMAKE_PREFIX_PATH=/path/to/relocated/sdk \
    -DJOBU_PHP_TEST_DAEMON=/path/to/matching/jobud
cmake --build .bld-php-module
/path/to/php/bin/php -n -dextension="$PWD/.bld-php-module/jobu.so" -m
ctest --test-dir .bld-php-module/test --output-on-failure
```

The SDK must match the source revision, distribution, architecture, compiler,
and system libraries. Its `Client` component provides PIC static libraries and
requires fmt; it does not import the SQLite backend or daemon. The module exports
the PHP loader entry point and hides its own and linked archive implementation
symbols. Release installation/packaging is deferred; use the explicit built
module path for now.

## Implemented PHP surface

All types are in namespace `JobU`. Enum case names use PascalCase and backing
values match the wire vocabulary:

| Enum | Cases |
| --- | --- |
| `QueueState` | Active, Suspending, Suspended, Deleted |
| `JobState` | Active, Suspending, Suspended, Deleted, Succeeded, Failed, Cancelled |
| `RunCancellationDisposition` | Completed, Requested |
| `RunState` | Scheduled, Running, RetryWait, Succeeded, Failed, Interrupted, Cancelled |
| `AttemptState` | Pending, Running, Completed |
| `AttemptOutcome` | Succeeded, Failed, Interrupted, Cancelled |
| `RetryMode` | Blocking, Reschedule |
| `RecoveryPolicy` | FailInterrupted, RetryInterrupted |
| `JobType` | Cli, Http |
| `OutputChannel` | Stdout, Stderr, Body, Headers |

For example, `JobU\RunState::RetryWait->value` is `retry_wait`, and
`JobU\JobType::from('cli')` returns `JobU\JobType::Cli`. Enums retain PHP's
native `cases()`, `from()`, `tryFrom()`, and serialization behavior. An unfinished
attempt's outcome is represented by null, not an extra enum case.

`JobU\Exception` extends PHP's `\Exception`. Its direct subclasses are
`ConnectionException`, `TimeoutException`, `NotFoundException`,
`RevisionConflictException`, `InvalidCursorException`, `UnsavedChangesException`,
`OutcomeUnknownException`, and `UnsupportedFeatureException`.

The inherited constructor accepts `message`, integer `code`, and `previous`.
`getErrorCode(): ?string` and `getCategory(): ?string` read separate daemon
metadata, initially null for user-created exceptions. `getCode()` keeps its
standard integer meaning. Constructor and private operation failures populate stable error
code/category metadata. `OutcomeUnknownException` additionally exposes
`getOperation(): ?string` and `getIdempotencyKey(): ?string`; both default to null
for user-created exceptions. Represented remote errors retain their numeric
JSON-RPC code in `getCode()`. PHP `TypeError` and `ValueError`
remain native engine errors. Exceptions and their subclasses reject serialization and
unserialization, including user-defined serialization hooks.

Registration happens during MINIT. Zend owns the registered classes and
exception properties. RINIT creates a private request owner; its event loop is attached
lazily when the first native session is created. Each session owns a separate
socket, raw RPC client, and typed client. The request registry borrows sessions
and holds no Zend references. All native and Zend access stays on the PHP
calling thread; there is no worker thread or process-wide connection cache.

RSHUTDOWN clears the active request pointer, closes and detaches every remaining
session, then releases the loop and restores the previous thread binding.
Session close first removes callback receivers, then destroys typed/raw clients
and the socket. Cleanup is synchronous and does not dispatch pending work.
Later native wrapper destruction is safe even when Zend frees object storage
after RSHUTDOWN. Explicit close is idempotent and affects only that session.
The loop stays attached until request shutdown, even after all sessions close.

`new JobU\Client(string $socketPath, ?JobU\Duration $timeout = null)` eagerly
connects to the Unix socket and completes `system.info`. It requires API major 1,
minor at least 7, and all thirty v1 capabilities; extra capabilities and newer
compatible minors are accepted. Failure releases the partial session. `close()`
is idempotent and affects only that client. Client cloning and serialization
are rejected; reconstructing an already initialized object is also rejected.

The default timeout is ten seconds. A custom constructor timeout becomes the
client's default for future operations. It must be positive and representable
by the monotonic clock. One absolute deadline covers all phases of an operation;
connection, handshake, and typed-call waiting never receive fresh full budgets.
Deadlines are cooperative: an executing native callback cannot be preempted.
Nested waits and displaced loop bindings fail before submitting an RPC.

`Duration::milliseconds(int)`, `seconds(int)`, `minutes(int)`, `hours(int)`, and
`days(int)` create immutable signed durations with checked integer millisecond
storage. Days are exactly 24 hours. Overflow raises `ValueError`; no floating-point
time conversion is used. Zero/negative durations can be represented but are
invalid as timeouts. Direct construction, cloning, and serialization are rejected.

Wrong argument types/ranges raise PHP `TypeError`/`ValueError`. Constructor
connection/handshake failures raise `ConnectionException`, deadline expiry raises
`TimeoutException`, and incompatible API/capabilities raise
`UnsupportedFeatureException`. Other initialization failures use `JobU\Exception`.
Messages omit socket paths and remote/backend detail; stable codes/categories are
available through the existing getters. An already pending PHP exception wins.

The private bridge wraps typed `ControlClient` calls, preserving owning replies,
local/remote failures, and mutation uncertainty. A terminal notification does not
replace its subsequent per-call outcome. Timeout or stopped-wait cleanup retires
local observation only; it does not cancel remote work or prove rollback. A
terminal failure already queued but not delivered at expiry is conservatively
uncertain. The bridge implements reconnect/retry and operational exception
translation; public `getSystemInfo()` remains later-stage work. There is no public raw
RPC entry point. Test-only bindings exercise calls without adding placeholder APIs.
No C++ exception may unwind into Zend. Allocation failure remains fatal, and
PHP bailout does not guarantee C++ stack cleanup.

## Recovery contract

Recovery currently lives behind the private typed bridge and test-only bindings;
public management/resource methods will use it as those APIs are implemented.
Each operation owns frozen request parameters. Replacement connections repeat the
API/capability handshake and replace the whole socket/raw/typed-client bundle.
They never revive a terminal native client. All attempts, reconnect delays, and
internal reads share the original monotonic deadline. A per-call Duration override
does not change the client's default. Reconnect waits use bounded event-loop timers.

| Operation after transport loss | Replay policy |
| --- | --- |
| Definitely not transmitted | Retry within the deadline, provided the failure is a recoverable transport failure |
| Reads, including cursor reads | Retry the identical request; never restart pagination |
| Queue/job suspend or resume | Repeat the same desired-state action |
| Queue/job creation and Run Now | Repeat identical parameters with the original idempotency key |
| Save, move, delete, run cancellation, secret changes, other mutations | Never replay after possible transmission |

Creation and Run Now receive an automatically generated, OS-random key unless
one was supplied. The same key survives every in-call retry. Automatic keys combine
two freshly seeded UUIDv7 values (148 random bits, 72 UTF-8 bytes), using the existing
SDK generator. Explicit keys remain unchanged; the bridge validates the 1–128 UTF-8 byte contract
before sending.
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

## Stub maintenance and validation

`src/jobu-php/jobu.stub.php` is authoritative. Generate and commit
`jobu_arginfo.h` when changing that surface, using PHP's `build/gen_stub.php` and
its matching PHP-Parser installation. The stub requests class entries and
PHP 8.1-compatible registration through `@generate-legacy-arginfo 80100`:

```sh
/path/to/php-8.5/bin/php -n -dextension=tokenizer \
    /path/to/php-8.5-source/build/gen_stub.php src/jobu-php/jobu.stub.php
```

Generation is a maintainer operation. Normal CMake builds and CI consume the
committed header without PHP sources, a generator, or a freshness check. Diagnose
the generated header through `module.cpp` with the selected ABI, and format only
handwritten C++.

CTest runs configuration rejection, module listing/information, Reflection and enum contracts, exception
behavior, and repeated child-process load/shutdown checks. An additional Embed
regression runs the production module hooks through three runtime lifetimes
with five requests each. It checks two connected sessions, close-one/use-other,
strong resource-to-client ownership, GC cycles, partial construction, pending
work, late native wrapper destruction, and reentrancy rejection. Linux also
checks descriptor accounting. This models reused PHP-FPM request boundaries;
it is not a claim of testing an actual PHP-FPM deployment.

Bridge tests additionally cover immediate/deferred replies, capability/version
rejection, constructor errors, one remaining deadline, pre-acceptance and remote
failures, timeout/stopped-wait cleanup, late replies, terminal mutation uncertainty,
and reentrancy. Recovery tests drop successful real-daemon replies before forwarding
them, compare replay parameters and keys, and assert committed resource identity
and single-transmission saves. They also cover restart, incompatible replacement
peers, terminal deadlines, next-call recovery, per-call overrides, exception
metadata/redaction, and independent clients. The real module is also loaded by a separate PHP CLI process
against the same-build daemon. Reused-runtime tests exercise production Client
construction, independent sessions, Duration conversions, and request teardown.

The fixture classes and their generated arginfo are linked only into the test
executable. The production module exports no test API and does not link Embed.
Integrated builds use the `jobud` target. Standalone tests take
`JOBU_PHP_TEST_DAEMON`; missing Embed or daemon produces explicit skips for
lifecycle/bridge checks, which the CI gate rejects. The lifecycle regression
covers normal request shutdown, not arbitrary recovery after Zend bailout or a process kill.
Standalone module tests also run the independent helper suite, including
relocated helper consumers.

The Ubuntu 26.04 and Alpine 3.24 PHP CI rows consume their native job's SDK from
**the same workflow run**. The archive also contains `fixtures/jobud`, staged
from that native build outside the installed Client SDK. PHP rows install the
existing daemon's runtime library dependencies and pass its path to the lifecycle
test. Each row rebuilds only helper/module/test sources.
The single `&php-versions` list in `.github/workflows/ci.yml` controls both
matrices. After extracting the matching archive, reproduce a row with:

```sh
CCACHE_DIR="$PWD/.ccache" tools/ci/php-test 8.5 \
    /usr/bin/php-config8.5-zend .bld/php /path/to/sdk
```

On Alpine use `/usr/bin/php-config85zend`. Omitting the SDK argument retains
helper-only validation. Both modes reject missing or skipped required checks.

## Immutable value primitives

`Duration::milliseconds/seconds/minutes/hours/days(int)` stores an exact signed
64-bit millisecond count; `getMilliseconds(): int` returns that count. Multiplication
overflow raises `ValueError`. A day is exactly 24 hours; zero and negative durations
are values, although client timeouts must be positive.

`new SecretReference(name: 'service.token')` stores only the symbolic name.
`getName(): string` returns it without looking up any secret. Names are at most 128
bytes, with lowercase dot-separated segments beginning with a letter and continuing
with lowercase letters, digits or underscores. Invalid names are rejected without
normalization. Secret administration and payload APIs remain deferred.

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
job creation and is rejected by the replacement-schedule adapter.

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
canonical lowercase strings, validated by a private adapter for future resource
methods. There is no public UUID wrapper or validation function.

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
alive. Replacing a defaults layer through the private resource adapter updates
existing views in place. Queue resources use these adapters; Job resource operations remain deferred.

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
| CLI | RetryExitCodes | list<string> | cli.retry_exit_codes |
| CLI | TerminationGrace | Duration | cli.termination_grace |
| CLI | StdoutLimit | int | output.stdout_limit |
| CLI | StderrLimit | int | output.stderr_limit |
| HTTP | FollowRedirects | bool | http.follow_redirects |
| HTTP | IdempotencyKey | bool | http.idempotency_key |
| HTTP | TlsVerify | bool | http.tls_verify |
| HTTP | MaxRedirects | int | http.max_redirects |
| HTTP | HttpBodyLimit | int | output.http_body_limit |
| HTTP | HttpHeadersLimit | int | output.http_headers_limit |
| HTTP | RetryErrors | list<string> | http.retry_errors |
| HTTP | RetryStatuses | list<string> | http.retry_statuses |

`OutputCapture` accepts `none`, `on_error` or `always`; `RetryStrategy` accepts
`fixed` or `exponential`. Generic `get/set('retry.mode', ...)` uses `blocking` or
`reschedule` strings; typed methods use the existing `RetryMode` enum.
`IdempotencyKey` is the HTTP execution policy boolean, not a management RPC key.
The standard registry remains authoritative for numeric bounds and retry selectors.

A setter validates its individual value before changing anything. Partial local
edits may temporarily violate cross-field relationships, such as initial retry
delay exceeding maximum delay. The private submission/export and merge/replacement
adapters check those relationships before accepting the complete supplied layer.
They never materialize defaults; effective configuration validation remains the
daemon's responsibility. Failed edits or failed adapter validation preserve the
previous layer. Merge copies every supplied attribute key, while defaults
replacement copies the entire supplied layer, preserving attached object identity.

Cloning any configuration creates independent native backing and drops attachment
and mutation notification. A cloned typed view contains only its applicable
attributes; a cloned QueueDefaults contains both families and creates its own
cached views. Returned arrays cannot mutate the original configuration. Comparison
uses visible attribute names and values in canonical name order, independent of
insertion order or attachment; `===` still tests identity. All four classes reject
serialization and repeated initialization. Internal classes disallow dynamic
properties. References stay within the owning PHP request and are traced for GC.

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

A bare executable may be edited before its PATH is supplied. Complete native
export requires an explicit literal PATH of at most 256 absolute nonempty
directories, bounded candidate paths, and the protocol's 256 KiB serialized and
prepared-request limits. No process environment fills missing values. The daemon
remains authoritative and rechecks secret-dependent bytes after resolution.
Public job submission methods are still deferred.

Cloning deep-copies the payload and detaches it from any owner. Equality compares
native values, ignoring environment insertion order and exit-code input order;
argument order matters. Strict identity remains PHP object identity. Getters and
clones retain no mutable caller arrays or references. Serialization, dynamic
properties and repeated construction are rejected. Error messages and native
object debugging do not expose submitted argument/environment values.


## HTTP payload configuration

`HttpPayload` is a local mutable request template. It performs no RPC and never
resolves a `SecretReference`. The native duplicate-header prerequisite preserves
ordered, repeated header names through admission and execution.

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
payload size is checked by the private submission/replacement adapter. The daemon
remains authoritative for resolved secret bytes and effective execution limits.

Cloning `HttpPayload` makes a detached deep copy, including all headers. Comparison
uses URL, canonical method, ordered header names/values/effective sensitivity,
body presence/bytes/reference, and ordered status selectors; attachment does not
affect equality. Strict comparison remains PHP object identity. Returned arrays,
headers and references do not alias mutable input state. Both classes reject
serialization, dynamic properties and repeated construction. Type mismatches use
`TypeError`, invalid values use `ValueError`, and JSON errors retain PHP semantics.

Private attachment/replacement adapters preserve attached identity, trace the
owner for GC, and notify only for actual local changes. Public Job resources,
revision tracking and saving remain later-stage work.

### Queries, metadata and execution results

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

The following final result classes have private constructors. Native adapters
create detached snapshots; public Client/resource methods that return them
are implemented in later stages. No result getter performs RPC.

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
serialized, and PHP GC traces their Client/defaults/view ownership cycles.

Stage 4.2 exercises `queue.stats` through the native typed bridge, including
cursor-only continuation and historical UUIDs. The public
`Queue::statistics(?StatisticsQuery $query = null, ?Duration $timeout = null): Page`
remains stage 5.4 after Page in stage 4.4; no placeholder or temporary result
shape is exposed. Queue job creation and definition lists are available.

## Private resource-state foundation

Stage 4.1 adds the internal cache/dirty-state machinery, used by the production
`Queue` binding in stage 4.2. The `Job` resource remains later work. The
fixture-only resource used by Embed tests is not registered by `jobu.so`.

`ResourceState<Queue>` and `ResourceState<JobDefinition>` retain the originating
PHP Client and keep independent accepted and working native snapshots. The
concrete binding traces its attached configuration objects as well as the
state's Client edge. No Client-to-resource ownership edge is added. All state
and Zend access remains on the originating request/thread; use the default
non-cloneable, non-serializable `ObjectBinding` policy for connected resources.

Setters track net differences from the accepted snapshot. Reverting a field
removes its edit; job attributes track changed keys and produce a supplied-key
patch, whereas queue defaults and payloads use full replacement. Intermediate
attribute edits may be temporarily inconsistent across fields, so dirty tracking
copies native values without whole-layer validation. Request construction
validates the complete configuration before freezing a typed operation.

A clean save returns without RPC. Dirty refresh and lifecycle guards fail
locally with `UnsavedChangesException`. Save freezes the stable UUID, accepted
revision and changed fields, and uses the existing operation/recovery bridge
with one total deadline. A validated successful reply becomes the new baseline;
conflicts, uncertain outcomes and conversion failures preserve all local edits
and the old baseline/revision. There is no added replay or reconciliation policy.
Discard restores the last accepted snapshot locally, even after Client closure.
RPC operations continue to use the existing Client boundary.

Attached attributes/defaults and CLI/HTTP payloads provide private prepared
restoration adapters. Prepare every component before committing any component;
commit moves native values into the existing objects/backing layers, without
notifications, PHP callbacks or GC. The temporary restoration borrows its target,
so it must not survive a PHP call or its target. Payload restoration accepts an
already converted configuration source; concrete Job reply decoding belongs to
the later Job resource stage. Ordinary edits preserve their existing notifications,
merge/replacement semantics and detached clone behavior.

An optional pre-commit edit guard lets an attached child reject edits after its
resource is locally deleted. Setters recheck after conversions that can execute
PHP. Clones detach both the owner and its guard/notification. Local deletion
retains cached getters and the last received revision; a null delete reply does
not fabricate a new token. Explicit historical refresh can use the shared cache
path where the concrete resource API supports it.

### Jobs and manual runs

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


## Definition lists and pages

`JobU\Page` represents a mutable position in a paged result. Its private factory
performs the first request eagerly; the returned Page starts **before** that
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
history. Run/Attempt history is described below; secrets and statistics remain later stages.

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

All phase-4 Queue/Job lookup, creation, editing, lifecycle and definition-list
entry points are available. The cached Run returned by `runNow()` is available;
`Job::listRuns()` and full Run/Attempt APIs, including output, are available.
Secret/schedule management remains stage 5.3, and `Queue::statistics()`/system
statistics remains stage 5.4.

- `hasNext(): bool` checks only local state. An empty first result returns false.
- `next(?Duration $timeout = null): void` publishes the prefetched first page
  without RPC. Later calls fetch the continuation using one operation deadline,
  including any reconnect/retry. Calling it when `hasNext()` is false throws
  `\OutOfBoundsException` and retains the last page.
- `items(): array` returns `[]` before the first successful `next()`, otherwise
  the current indexed collection. It performs no RPC. Editing the returned array
  leaves Page's collection unchanged; contained objects retain their identities.

A fetch or conversion failure preserves the current items and continuation.
A successful empty continuation publishes `[]` and ends enumeration. Short pages
may still have continuations: Page follows the returned boundary rather than
comparing the item count to the requested limit. Listings are live reads, not
frozen snapshots across requests.

Queue/job UUID keysets repeat their original filters and limit. Secret name
keysets repeat the limit. Opaque history/statistics continuations send only the
cursor. Expired cursors, including those invalidated by daemon restart, raise
`InvalidCursorException`; enumeration never silently restarts.

Page strongly retains its originating Client and traces its items for PHP GC.
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
existing generic accessor. Summary/resource instances retain and GC-trace their
Client, reject cloning/serialization/dynamic properties, and remain request-local.
Strict comparison (`===`) identifies a PHP instance; use ID getters (and attempt
number) to compare record identity rather than relying on loose object comparison.
Explicit Client closure leaves cached getters usable but prevents further RPCs.

### Retained output

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
