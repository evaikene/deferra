# JobU PHP extension

The optional `jobu` extension provides native string-backed enums, an exception
hierarchy, immutable durations, secret references and schedules, mutable attributes
and queue defaults, mutable CLI/HTTP payloads, and request-scoped `Client` connections.
The constructor eagerly connects and handshakes; public resource and management
methods remain deferred. A private synchronous bridge is exercised by tests.
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
translation; public `getSystemInfo()` and resources remain later-stage work. There is no public raw
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
existing views in place. Public Queue/Job resource operations remain deferred.

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
