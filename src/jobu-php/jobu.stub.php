<?php

/**
 * @generate-class-entries
 * @generate-legacy-arginfo 80100
 */

namespace JobU;

/** Immutable signed fixed duration with checked integer millisecond storage. @not-serializable */
final class Duration
{
    private function __construct() {}
    /** Return the exact signed millisecond count. */
    public function getMilliseconds(): int {}
    public static function milliseconds(int $value): Duration {}
    public static function seconds(int $value): Duration {}
    public static function minutes(int $value): Duration {}
    public static function hours(int $value): Duration {}
    /** One day is exactly 24 hours, independent of calendar/timezone changes. */
    public static function days(int $value): Duration {}
}

/** Immutable symbolic secret name; never resolves a secret. @not-serializable */
final class SecretReference
{
    public function __construct(string $name) {}
    public function getName(): string {}
}

/** Mutable CLI template. Local edits never execute a command or resolve secrets. @not-serializable */
final class CliPayload
{
    /** Tokenize command without shell evaluation, then append the supplied list verbatim.
     * @param list<string|SecretReference> $arguments
     */
    public function __construct(string $command, array $arguments = []) {}
    /** Atomically replace executable and arguments, preserving all other fields.
     * @param list<string|SecretReference> $arguments
     */
    public function setCommand(string $command, array $arguments = []): void {}
    public function getExecutable(): string {}
    /** Set a literal executable, preserving arguments; bare names require PATH at submission. */
    public function setExecutable(string $executable): void {}
    /** @return list<string|SecretReference> Independent values in argv order. */
    public function getArguments(): array {}
    /** @param list<string|SecretReference> $arguments */
    public function setArguments(array $arguments): void {}
    public function addArgument(string|SecretReference $argument): void {}
    /** @return array<string,string|SecretReference|null> Missing keys have no override; null means explicit unset. */
    public function getEnvironment(): array {}
    public function setEnvironment(string $name, string|SecretReference $value): void {}
    /** Record explicit removal at execution. */
    public function unsetEnvironment(string $name): void {}
    /** Remove this payload override. */
    public function clearEnvironment(string $name): void {}
    /** Absolute directory; defaults to /. */
    public function getWorkingDirectory(): string {}
    public function setWorkingDirectory(string $directory): void {}
    /** @return list<int> Ascending distinct exit codes; defaults to [0]. */
    public function getExpectedExitCodes(): array {}
    /** @param list<int> $codes Distinct integers 0–255; an empty list accepts no exit code. */
    public function setExpectedExitCodes(array $codes): void {}
}

/** Mutable HTTP template; setters are local and never resolve secrets. @not-serializable */
final class HttpPayload
{
    /** Accept an HTTP(S) URL and GET, POST, PUT, PATCH, DELETE or HEAD (case-insensitive). */
    public function __construct(string $url, string $method = 'GET') {}
    public function getUrl(): string {}
    public function setUrl(string $url): void {}
    public function getMethod(): string {}
    /** Canonicalize uppercase; HEAD rejects any present body, including empty bytes. */
    public function setMethod(string $method): void {}
    /** Append without merging duplicate names. References and credential headers are always sensitive. */
    public function addHeader(string $name, string|SecretReference $value, bool $sensitive = false): void {}
    /** @return list<HttpHeader> Independent immutable snapshots in insertion order. */
    public function getHeaders(): array {}
    /** Remove every case-insensitive match, preserving other entries and their order. */
    public function clearHeader(string $name): void {}
    /** Accept arbitrary bytes or a symbolic reference; reject bodies for HEAD. */
    public function setBody(string|SecretReference $body): void {}
    /** Null means absent, distinct from an empty string. */
    public function getBody(): string|SecretReference|null {}
    public function clearBody(): void {}
    /** Encode using json_encode(value, JSON_THROW_ON_ERROR, 512), then atomically set the body
     * and append Content-Type: application/json only when no Content-Type exists.
     * @throws \JsonException On JSON encoding errors; callback exceptions propagate unchanged.
     */
    public function setJsonBody(mixed $value): void {}
    /** @return list<string> Ordered status selectors; defaults to ['200-299']. */
    public function getExpectedStatuses(): array {}
    /** @param list<string> $statuses Between 1 and 64 selectors (100–599 or inclusive ranges). */
    public function setExpectedStatuses(array $statuses): void {}
}

/** Immutable detached header snapshot; obtained through HttpPayload. @not-serializable */
final class HttpHeader
{
    private function __construct() {}
    public function getName(): string {}
    public function getValue(): string|SecretReference {}
    public function isSensitive(): bool {}
}

/** Immutable schedule value. @not-serializable */
abstract class Schedule {}

/** Absolute instant or a creation-only symbolic now marker. @not-serializable */
final class OnceSchedule extends Schedule
{
    /** Copy the instant, preserving microseconds within the native clock range. */
    public function __construct(\DateTimeInterface $at) {}
    /** The daemon resolves now when creating the job, never when constructing this value. */
    public static function now(): OnceSchedule {}
    public function isNow(): bool {}
    /** A fresh UTC value, or null for symbolic now. */
    public function getAt(): ?\DateTimeImmutable {}
}

/** Cron input; grammar is validated by the daemon. @not-serializable */
final class CronSchedule extends Schedule
{
    /** Null preserves the daemon default; explicit named timezones remain explicit. */
    public function __construct(string $expression, ?\DateTimeZone $timezone = null) {}
    public function getExpression(): string {}
    /** A fresh timezone value, or null while the daemon default is unresolved. */
    public function getTimezone(): ?\DateTimeZone {}
}

/** Partial job attributes; getters are local, and absent values return null. @not-serializable */
class JobAttributes
{
    public function __construct() {}
    public function get(string $name): mixed {}
    public function has(string $name): bool {}
    /** Validate one value; cross-field validation occurs at submission. */
    public function set(string $name, mixed $value): void {}
    /** Supplied job.timeout, or null when absent. */
    public function getTimeout(): ?Duration {}
    public function setTimeout(Duration $value): void {}
    /** Supplied output.capture, or null when absent. */
    public function getOutputCapture(): ?string {}
    public function setOutputCapture(string $value): void {}
    /** Supplied retry.initial_delay, or null when absent. */
    public function getRetryInitialDelay(): ?Duration {}
    public function setRetryInitialDelay(Duration $value): void {}
    /** Supplied retry.max_delay, or null when absent. */
    public function getRetryMaxDelay(): ?Duration {}
    public function setRetryMaxDelay(Duration $value): void {}
    /** Supplied retry.max_attempts, or null when absent. */
    public function getRetryMaxAttempts(): ?int {}
    public function setRetryMaxAttempts(int $value): void {}
    /** Supplied retry.jitter, or null when absent. */
    public function getRetryJitter(): ?float {}
    public function setRetryJitter(float $value): void {}
    /** Supplied retry.multiplier, or null when absent. */
    public function getRetryMultiplier(): ?float {}
    public function setRetryMultiplier(float $value): void {}
    /** Supplied retry.mode, or null when absent. */
    public function getRetryMode(): ?RetryMode {}
    public function setRetryMode(RetryMode $value): void {}
    /** Supplied retry.strategy, or null when absent. */
    public function getRetryStrategy(): ?string {}
    public function setRetryStrategy(string $value): void {}
}

/** Runner-specific partial attributes or a shared queue-default view. @not-serializable */
final class CliJobAttributes extends JobAttributes
{
    /** Supplied cli.retry_exit_codes, or null when absent. @return list<string>|null */
    public function getRetryExitCodes(): ?array {}
    /** @param list<string> $value */
    public function setRetryExitCodes(array $value): void {}
    /** Supplied cli.termination_grace, or null when absent. */
    public function getTerminationGrace(): ?Duration {}
    public function setTerminationGrace(Duration $value): void {}
    /** Supplied output.stdout_limit, or null when absent. */
    public function getStdoutLimit(): ?int {}
    public function setStdoutLimit(int $value): void {}
    /** Supplied output.stderr_limit, or null when absent. */
    public function getStderrLimit(): ?int {}
    public function setStderrLimit(int $value): void {}
}

/** Runner-specific partial attributes or a shared queue-default view. @not-serializable */
final class HttpJobAttributes extends JobAttributes
{
    /** Supplied http.follow_redirects, or null when absent. */
    public function getFollowRedirects(): ?bool {}
    public function setFollowRedirects(bool $value): void {}
    /** Supplied http.idempotency_key, or null when absent. */
    public function getIdempotencyKey(): ?bool {}
    public function setIdempotencyKey(bool $value): void {}
    /** Supplied http.tls_verify, or null when absent. */
    public function getTlsVerify(): ?bool {}
    public function setTlsVerify(bool $value): void {}
    /** Supplied http.max_redirects, or null when absent. */
    public function getMaxRedirects(): ?int {}
    public function setMaxRedirects(int $value): void {}
    /** Supplied output.http_body_limit, or null when absent. */
    public function getHttpBodyLimit(): ?int {}
    public function setHttpBodyLimit(int $value): void {}
    /** Supplied output.http_headers_limit, or null when absent. */
    public function getHttpHeadersLimit(): ?int {}
    public function setHttpHeadersLimit(int $value): void {}
    /** Supplied http.retry_errors, or null when absent. @return list<string>|null */
    public function getRetryErrors(): ?array {}
    /** @param list<string> $value */
    public function setRetryErrors(array $value): void {}
    /** Supplied http.retry_statuses, or null when absent. @return list<string>|null */
    public function getRetryStatuses(): ?array {}
    /** @param list<string> $value */
    public function setRetryStatuses(array $value): void {}
}

/** Partial multi-runner defaults; typed views share this layer. Clones are detached. @not-serializable */
final class QueueDefaults
{
    public function __construct() {}
    public function get(string $name): mixed {}
    public function has(string $name): bool {}
    public function set(string $name, mixed $value): void {}
    /** Remove one override; clearing an absent known name does nothing. */
    public function clear(string $name): void {}
    public function cli(): CliJobAttributes {}
    public function http(): HttpJobAttributes {}
}

/** Request-scoped synchronous Unix-socket client. @not-serializable */
final class Client
{
    /** Connect and validate the daemon eagerly; timeout defaults to ten seconds and must be positive. */
    public function __construct(string $socketPath, ?Duration $timeout = null) {}
    /** Idempotently close this session. Further operations fail locally. */
    public function close(): void {}
    /** Eagerly fetch definitions; null state includes every state permitted by includeDeleted.
     * Limit is 1–200. @return Page<Queue>
     */
    public function listQueues(
        ?QueueState $state = null,
        bool $includeDeleted = false,
        int $limit = 100,
        ?Duration $timeout = null
    ): Page {}
    /** Eagerly fetch full definitions. Select at most one literal queueName or canonical queueId.
     * Both null selects all queues. Names are resolved on each page; IDs keep queue identity stable.
     * Filters combine with AND; Deleted state still requires includeDeleted. Limit is 1–200.
     * @return Page<Job>
     */
    public function listJobs(
        ?string $queueName = null,
        ?string $queueId = null,
        ?JobState $state = null,
        ?JobType $type = null,
        bool $includeDeleted = false,
        int $limit = 100,
        ?Duration $timeout = null
    ): Page {}
    /** Eagerly fetch summaries across queues; null query uses limit 100. @return Page<RunSummary> */
    public function listRuns(?RunQuery $query = null, ?Duration $timeout = null): Page {}
    /** Look up an exact literal name, including names that look like UUIDs. */
    public function getQueue(string $name, bool $includeDeleted = false, ?Duration $timeout = null): Queue {}
    /** Look up a canonical lowercase UUID. */
    public function getQueueById(string $id, bool $includeDeleted = false, ?Duration $timeout = null): Queue {}
    /** Copy defaults and create an active queue. Null retention inherits; null warning uses ten seconds.
     * Omitted keys are generated once and retained across retries. Replay returns the original creation snapshot.
     */
    public function createQueue(
        string $name,
        int $weight = 1,
        int $concurrencyLimit = 1,
        RecoveryPolicy $recoveryPolicy = RecoveryPolicy::FailInterrupted,
        ?QueueDefaults $defaults = null,
        ?Duration $historyRetention = null,
        ?Duration $runnableWaitWarning = null,
        ?string $idempotencyKey = null,
        ?Duration $timeout = null
    ): Queue {}

}

/** Cached request-local resource. Getters and setters perform no RPC. @not-serializable */
final class Queue
{
    /** Load a nondeleted job belonging to this queue; a missing or out-of-scope ID raises NotFoundException.
     * Preserves local edits. Use listJobs(includeDeleted: true) for retained deleted definitions.
     */
    public function getJob(string $id, ?Duration $timeout = null): Job {}
    /** List retained runs in this queue, preserving local edits even after deletion.
     * A supplied query queueId must match; other filters remain unchanged. @return Page<RunSummary>
     */
    public function listRuns(?RunQuery $query = null, ?Duration $timeout = null): Page {}
    /** Eagerly list persisted definitions by stable queue UUID, preserving any local edits.
     * Retained deleted queues require includeDeleted. Limit is 1–200. @return Page<Job>
     */
    public function listJobs(
        ?JobState $state = null,
        ?JobType $type = null,
        bool $includeDeleted = false,
        int $limit = 100,
        ?Duration $timeout = null
    ): Page {}
    /** Copy inputs and create a job and initial scheduled run. Reject dirty/deleted queues. */
    public function createCliJob(
        CliPayload $payload,
        Schedule $schedule,
        ?string $name = null,
        int $priority = 0,
        ?JobAttributes $attributes = null,
        ?string $idempotencyKey = null,
        ?Duration $timeout = null
    ): Job {}
    /** Copy inputs and create a job and initial scheduled run. Reject dirty/deleted queues. */
    public function createHttpJob(
        HttpPayload $payload,
        Schedule $schedule,
        ?string $name = null,
        int $priority = 0,
        ?JobAttributes $attributes = null,
        ?string $idempotencyKey = null,
        ?Duration $timeout = null
    ): Job {}
    private function __construct() {}
    public function getId(): string {}
    public function getRevision(): int {}
    public function getName(): string {}
    /** Exact UTF-8 name, 1–128 bytes, without controls or a reserved deletion suffix. */
    public function setName(string $name): void {}
    public function getState(): QueueState {}
    public function getWeight(): int {}
    /** Positive 32-bit unsigned scheduler weight. */
    public function setWeight(int $weight): void {}
    public function getConcurrencyLimit(): int {}
    /** Positive 32-bit unsigned capacity. */
    public function setConcurrencyLimit(int $concurrencyLimit): void {}
    public function getRecoveryPolicy(): RecoveryPolicy {}
    public function setRecoveryPolicy(RecoveryPolicy $recoveryPolicy): void {}
    public function getHistoryRetention(): ?Duration {}
    /** Null inherits; zero is unlimited; otherwise nonnegative whole seconds. */
    public function setHistoryRetention(?Duration $historyRetention): void {}
    public function getRunnableWaitWarning(): Duration {}
    /** Nonnegative milliseconds; zero disables warnings. */
    public function setRunnableWaitWarning(Duration $runnableWaitWarning): void {}
    public function getCreatedAt(): \DateTimeImmutable {}
    public function getUpdatedAt(): \DateTimeImmutable {}
    public function getDeletedAt(): ?\DateTimeImmutable {}
    public function isDirty(): bool {}
    /** Local deletion is known immediately even when cached server fields predate delete. */
    public function isDeleted(): bool {}
    /** Detached deep copy, including any local edits. */
    public function getDefaults(): QueueDefaults {}
    /** Stable attached mutable object, retaining this Queue and its Client. */
    public function defaults(): QueueDefaults {}
    /** Replace the whole local layer in place; an empty layer clears overrides on save. */
    public function setDefaults(QueueDefaults $defaults): void {}
    /** Save only net edits with the accepted revision. Conflict/uncertainty preserves edits. */
    public function save(?Duration $timeout = null): void {}
    /** Reject dirty state. Explicitly allow retained historical snapshots with includeDeleted. */
    public function refresh(bool $includeDeleted = false, ?Duration $timeout = null): void {}
    /** Restore the last accepted snapshot in place, without RPC, even after Client closure. */
    public function discardChanges(): void {}
    /** Reject dirty/deleted state; adopt the returned snapshot without waiting for work to drain. */
    public function suspend(?Duration $timeout = null): void {}
    public function resume(?Duration $timeout = null): void {}
    /** Requires a suspended queue remotely. Retain cached fields/revision and block further edits. */
    public function delete(?Duration $timeout = null): void {}
}

/** Queue lifecycle state, including retained deleted queues. */
enum QueueState: string
{
    case Active = 'active';
    case Suspending = 'suspending';
    case Suspended = 'suspended';
    case Deleted = 'deleted';
}

/** Job lifecycle state. */
enum JobState: string
{
    case Active = 'active';
    case Suspending = 'suspending';
    case Suspended = 'suspended';
    case Deleted = 'deleted';
    case Succeeded = 'succeeded';
    case Failed = 'failed';
    case Cancelled = 'cancelled';
}

/** Run lifecycle state; cancellation is distinct from a completed attempt. */
enum RunState: string
{
    case Scheduled = 'scheduled';
    case Running = 'running';
    case RetryWait = 'retry_wait';
    case Succeeded = 'succeeded';
    case Failed = 'failed';
    case Interrupted = 'interrupted';
    case Cancelled = 'cancelled';
}

/** Execution-attempt lifecycle state. */
enum AttemptState: string
{
    case Pending = 'pending';
    case Running = 'running';
    case Completed = 'completed';
}

/** Terminal attempt result. An unfinished attempt has no outcome. */
enum AttemptOutcome: string
{
    case Succeeded = 'succeeded';
    case Failed = 'failed';
    case Interrupted = 'interrupted';
    case Cancelled = 'cancelled';
}

/** Whether a retry wait holds queue capacity or reschedules the attempt. */
enum RetryMode: string
{
    case Blocking = 'blocking';
    case Reschedule = 'reschedule';
}

/** Daemon startup treatment of interrupted attempts. */
enum RecoveryPolicy: string
{
    case FailInterrupted = 'fail_interrupted';
    case RetryInterrupted = 'retry_interrupted';
}

/** Immutable runner type of a job. */
enum JobType: string
{
    case Cli = 'cli';
    case Http = 'http';
}

/** Captured CLI streams or HTTP response components. */
enum OutputChannel: string
{
    case Stdout = 'stdout';
    case Stderr = 'stderr';
    case Body = 'body';
    case Headers = 'headers';
}

/**
 * Base for JobU operational errors. Native TypeError/ValueError stay separate.
 * Inherits the standard Exception constructor and integer getCode() contract.
 * @not-serializable
 */
class Exception extends \Exception
{
    private ?string $_errorCode = null;
    private ?string $_category = null;

    /** Stable daemon error code, or null when no daemon metadata is available. */
    public function getErrorCode(): ?string {}

    /** Stable daemon error category, or null when unavailable. */
    public function getCategory(): ?string {}
}

/** @not-serializable */
class ConnectionException extends Exception {}

/** @not-serializable */
class TimeoutException extends Exception {}

/** @not-serializable */
class NotFoundException extends Exception {}

/** @not-serializable */
class RevisionConflictException extends Exception {}

/** @not-serializable */
class InvalidCursorException extends Exception {}

/** @not-serializable */
class UnsavedChangesException extends Exception {}

/** @not-serializable */
class OutcomeUnknownException extends Exception
{
    private ?string $_operation = null;
    private ?string $_idempotencyKey = null;

    /** Stable operation name, or null for an exception constructed by user code. */
    public function getOperation(): ?string {}

    /** Original replay key when available; retain it for later reconciliation. */
    public function getIdempotencyKey(): ?string {}
}

/** @not-serializable */
class UnsupportedFeatureException extends Exception {}

/** Immutable half-open UTC interval; omitted bounds remain unresolved. @not-serializable */
final class TimeRange
{
    /** Copy input instants; require from < to when both are supplied. */
    public function __construct(?\DateTimeInterface $from = null, ?\DateTimeInterface $to = null) {}
    public function getFrom(): ?\DateTimeImmutable {}
    public function getTo(): ?\DateTimeImmutable {}
}

/** Immutable initial run-list filters; all filters combine with AND. @not-serializable */
final class RunQuery
{
    /** Nonempty distinct states, mutually exclusive with state.
     * @param null|list<RunState> $states
     */
    public function __construct(?string $queueId = null, ?string $jobId = null,
        ?RunState $state = null, ?array $states = null, ?RunOrigin $origin = null,
        ?JobType $type = null, ?TimeRange $planned = null, ?TimeRange $started = null,
        ?TimeRange $completed = null, int $limit = 100) {}
    public function getQueueId(): ?string {}
    public function getJobId(): ?string {}
    public function getState(): ?RunState {}
    /** @return null|list<RunState> */
    public function getStates(): ?array {}
    public function getOrigin(): ?RunOrigin {}
    public function getType(): ?JobType {}
    public function getPlanned(): TimeRange {}
    public function getStarted(): TimeRange {}
    public function getCompleted(): TimeRange {}
    public function getLimit(): int {}
}

/** Immutable statistics filters. The daemon resolves omitted window bounds. @not-serializable */
final class StatisticsQuery
{
    public function __construct(?string $queueId = null, ?string $jobId = null,
        ?JobType $type = null, ?RunOrigin $origin = null, ?TimeRange $planned = null,
        StatisticsGroupBy $groupBy = StatisticsGroupBy::None, int $limit = 100) {}
    public function getQueueId(): ?string {}
    public function getJobId(): ?string {}
    public function getType(): ?JobType {}
    public function getOrigin(): ?RunOrigin {}
    public function getPlanned(): TimeRange {}
    public function getGroupBy(): StatisticsGroupBy {}
    public function getLimit(): int {}
}

/** Detached daemon metadata; getters never perform RPC. @not-serializable */
final class SystemInfo
{
    private function __construct() {}
    public function getDaemonVersion(): string {}
    public function getApiMajor(): int {}
    public function getApiMinor(): int {}
    /** @return list<string> Sorted unique capabilities. */
    public function getCapabilities(): array {}
    public function hasCapability(string $name): bool {}
}

/** Detached metadata only; never contains a secret value. @not-serializable */
final class SecretMetadata
{
    private function __construct() {}
    public function getName(): string {}
    public function getCreatedAt(): \DateTimeImmutable {}
    public function getUpdatedAt(): \DateTimeImmutable {}
}

/** Immutable safe daemon result, distinct from application output. @not-serializable */
abstract class ExecutionResult
{
    final private function __construct() {}
    /** Trusted run type; an unrecognized result discriminator cannot override it. */
    public function getType(): JobType {}
    /** Runner vocabulary, not AttemptOutcome; absent or invalid projections return null. */
    public function getOutcome(): ?string {}
    public function getReason(): ?string {}
    public function getErrorCode(): ?string {}
    public function getOutcomeUnknown(): ?bool {}
    /** Presence test distinguishes explicit null from absence. */
    public function has(string $field): bool {}
    /** Independent JSON values: objects become stdClass, arrays become lists.
     * Unsigned integers above PHP_INT_MAX become exact decimal strings. Absent fields return null.
     */
    public function get(string $field): mixed {}
    public function getCapturedBytes(OutputChannel $channel): ?int {}
    public function getTotalBytes(OutputChannel $channel): ?int {}
    public function isTruncated(OutputChannel $channel): ?bool {}
}

/** Optional CLI observations; a result need not contain an exit code. @not-serializable */
final class CliExecutionResult extends ExecutionResult
{
    public function getExitCode(): ?int {}
    public function getSignal(): ?int {}
    public function getCaptureLost(): ?bool {}
}

/** Optional HTTP observations; transport failures need not contain a status. @not-serializable */
final class HttpExecutionResult extends ExecutionResult
{
    public function getStatus(): ?int {}
    public function getDuration(): ?Duration {}
    public function getRedirects(): ?int {}
    public function getTlsVerified(): ?bool {}
    public function getErrorCategory(): ?string {}
    public function getRetryAfterAccepted(): ?bool {}
    public function getRetryAfterClamped(): ?bool {}
}

/** Immutable retained-output slice; status and loss evidence remain independent of bytes. @not-serializable */
final class OutputChunk
{
    private function __construct() {}
    public function getRunId(): string {}
    public function getAttemptNumber(): int {}
    public function getChannel(): OutputChannel {}
    public function getStatus(): OutputStatus {}
    /** Offsets address retained bytes, excluding any omitted middle. */
    public function getOffset(): int {}
    public function getBytesReturned(): int {}
    public function getNextOffset(): ?int {}
    public function getRetainedBytes(): int {}
    public function getTotalBytes(): ?int {}
    public function getOmittedBytes(): ?int {}
    public function isTruncated(): bool {}
    public function isCaptureLost(): bool {}
    /** Raw bytes, including NUL; empty bytes do not by themselves establish availability. */
    public function getData(): string {}
}

/** Supported public run origins; submitted remains reserved. */
enum RunOrigin: string
{
    case Scheduled = 'scheduled';
    case Manual = 'manual';
}

enum StatisticsGroupBy: string
{
    case None = 'none';
    case Queue = 'queue';
    case Job = 'job';
    case Type = 'type';
    case Origin = 'origin';
    case State = 'state';
}

enum OutputStatus: string
{
    case Available = 'available';
    case Pending = 'pending';
    case NotCaptured = 'not_captured';
    case Lost = 'lost';
}

/** Cached job definition. Local edits require save; lifecycle calls reject dirty state. @not-serializable */
final class Job
{
    /** Load a retained run belonging to this job; an ID outside this scope raises NotFoundException.
     * Preserves local edits and permits retained deleted resources.
     */
    public function getRun(string $id, ?Duration $timeout = null): Run {}
    /** List retained history by stable job ID, preserving local edits even after deletion.
     * A supplied query jobId must match; other filters remain unchanged. @return Page<RunSummary>
     */
    public function listRuns(?RunQuery $query = null, ?Duration $timeout = null): Page {}
    private function __construct() {}
    public function getId(): string {}
    public function getQueueId(): string {}
    public function getRevision(): int {}
    public function getName(): ?string {}
    public function getPriority(): int {}
    public function getState(): JobState {}
    public function getType(): JobType {}
    public function getCreatedAt(): \DateTimeImmutable {}
    public function getUpdatedAt(): \DateTimeImmutable {}
    public function getDeletedAt(): ?\DateTimeImmutable {}
    public function getSchedule(): Schedule {}
    /** Detached deep copy, including local edits. */
    public function getAttributes(): JobAttributes {}
    /** Attached typed configuration; identity is stable across save/refresh/discard. */
    public function attributes(): JobAttributes {}
    /** Detached deep copy of the original template; secrets remain symbolic. */
    public function getPayload(): CliPayload|HttpPayload {}
    /** Attached mutable payload retaining this Job and its Client. */
    public function payload(): CliPayload|HttpPayload {}
    public function isDirty(): bool {}
    public function isDeleted(): bool {}
    public function setName(?string $name): void {}
    public function setPriority(int $priority): void {}
    /** Replace locally; symbolic now is creation-only and omitted timezone stays unresolved until save. */
    public function setSchedule(Schedule $schedule): void {}
    /** Merge all supplied keys; omitted values remain unchanged. */
    public function setAttributes(JobAttributes $attributes): void {}
    /** Replace all fields in place; the runner must match this Job. */
    public function setPayload(CliPayload|HttpPayload $payload): void {}
    /** Submit net edits with the accepted revision; conflict/uncertainty preserves edits. */
    public function save(?Duration $timeout = null): void {}
    /** Fetch explicitly; reject dirty or locally deleted state. */
    public function refresh(?Duration $timeout = null): void {}
    /** Restore the last accepted snapshot in place without RPC. */
    public function discardChanges(): void {}
    public function suspend(?Duration $timeout = null): void {}
    public function resume(?Duration $timeout = null): void {}
    /** Move by UUID/revision to a clean, nondeleted Queue from the same Client. */
    public function move(Queue $queue, ?Duration $timeout = null): void {}
    /** Mark locally deleted after success, retaining cached fields and revision. */
    public function delete(?Duration $timeout = null): void {}
    /** Create a keyed manual run without waiting for execution or refreshing the Job. */
    public function runNow(?string $idempotencyKey = null, ?Duration $timeout = null): Run {}
}

/** Complete cached run snapshot. Field getters never fetch or wait for execution. @not-serializable */
final class Run
{
    /** Select the latest attempt once by largest number; no attempts returns null.
     * Read up to maxBytes (1..65536) from the retained tail; default 4096 bytes.
     * CLI prefers non-empty stderr, then stdout; HTTP reads body. Lost capture may retain bytes.
     * Null means unavailable; an empty string means captured empty (including empty stderr when stdout is unavailable).
     * No wait or cache refresh. All reads and reconnects share one total timeout.
     */
    public function getOutput(int $maxBytes = 4096, ?Duration $timeout = null): ?string {}

    /** Load this run's attempt by number, between 1 and PHP_INT_MAX. */
    public function getAttempt(int $number, ?Duration $timeout = null): Attempt {}
    /** Explicitly replace the cached snapshot; failure preserves it. */
    public function refresh(?Duration $timeout = null): void {}
    /** Adopt the returned snapshot and disposition, without waiting for terminal completion.
     * A possibly transmitted cancellation is never automatically replayed.
     */
    public function cancel(?Duration $timeout = null): RunCancellationDisposition {}
    /** Eager summaries with this run's trusted type; no run detail fetch. @return Page<AttemptSummary> */
    public function listAttempts(int $limit = 100, ?Duration $timeout = null): Page {}
    private function __construct() {}
    public function getId(): string {}
    public function getJobId(): string {}
    public function getQueueId(): string {}
    public function getJobRevision(): int {}
    public function getPriority(): int {}
    public function getOrigin(): RunOrigin {}
    public function getType(): JobType {}
    public function getState(): RunState {}
    public function isScheduleOwned(): bool {}
    public function getPlannedAt(): \DateTimeImmutable {}
    public function getRunnableAt(): \DateTimeImmutable {}
    public function getStartedAt(): ?\DateTimeImmutable {}
    public function getCompletedAt(): ?\DateTimeImmutable {}
    /** Detached deep copy, including local edits. */
    public function getAttributes(): JobAttributes {}
    /** Detached deep copy of the original template; secrets remain symbolic. */
    public function getPayload(): CliPayload|HttpPayload {}
    /** Cached safe execution summary, or null while unavailable. No hidden fetch. */
    public function getResult(): ?ExecutionResult {}
}

/** Management disposition; Requested does not mean execution has terminated. */
enum RunCancellationDisposition: string
{
    case Completed = "completed";
    case Requested = "requested";
}

/** Immutable list snapshot retaining its Client. Getters never fetch details. @not-serializable */
final class RunSummary
{
    private function __construct() {}
    public function getId(): string {}
    public function getJobId(): string {}
    public function getQueueId(): string {}
    public function getJobRevision(): int {}
    public function getPriority(): int {}
    public function getOrigin(): RunOrigin {}
    public function getType(): JobType {}
    public function getState(): RunState {}
    public function isScheduleOwned(): bool {}
    public function getPlannedAt(): \DateTimeImmutable {}
    public function getRunnableAt(): \DateTimeImmutable {}
    public function getStartedAt(): ?\DateTimeImmutable {}
    public function getCompletedAt(): ?\DateTimeImmutable {}
    /** Read a new full Run without changing this summary. */
    public function load(?Duration $timeout = null): Run {}
}

/** Immutable list snapshot retaining its Client; no result or output. @not-serializable */
final class AttemptSummary
{
    private function __construct() {}
    public function getRunId(): string {}
    public function getNumber(): int {}
    public function getDueAt(): \DateTimeImmutable {}
    public function getStartedAt(): ?\DateTimeImmutable {}
    public function getCompletedAt(): ?\DateTimeImmutable {}
    public function getState(): AttemptState {}
    public function getOutcome(): ?AttemptOutcome {}
    /** Read a new Attempt without changing this summary. */
    public function load(?Duration $timeout = null): Attempt {}
}

/** Cached attempt retaining its Client and trusted runner type. @not-serializable */
final class Attempt
{
    /** Read retained bytes: offset 0..PHP_INT_MAX-1, length 1..65536.
     * CLI accepts Stdout/Stderr; HTTP accepts Body/Headers. EOF offset is allowed.
     * Preserves capture status, loss/truncation and nullable counts; never waits for completion.
     */
    public function readOutput(OutputChannel $channel, int $offset = 0, int $length = 16384, ?Duration $timeout = null): OutputChunk {}
    /** Read up to maxBytes (1..65536) from the retained tail; default 4096 bytes.
     * CLI prefers non-empty stderr, then stdout; HTTP reads body. Lost capture may retain bytes.
     * Null means unavailable; an empty string means captured empty (including empty stderr when stdout is unavailable).
     * No wait or cache refresh. All reads and reconnects share one total timeout.
     */
    public function getOutput(int $maxBytes = 4096, ?Duration $timeout = null): ?string {}

    private function __construct() {}
    public function getRunId(): string {}
    public function getNumber(): int {}
    public function getDueAt(): \DateTimeImmutable {}
    public function getStartedAt(): ?\DateTimeImmutable {}
    public function getCompletedAt(): ?\DateTimeImmutable {}
    public function getState(): AttemptState {}
    public function getOutcome(): ?AttemptOutcome {}
    /** Replace the snapshot explicitly; failure preserves all cached fields. */
    public function refresh(?Duration $timeout = null): void {}
    /** Cached typed safe metadata or null; never fetches or interprets application output. */
    public function getResult(): ?ExecutionResult {}
}

/** Mutable page position retaining its Client. @template T @not-serializable */
final class Page
{
    private function __construct() {}
    /** Whether a prefetched page or continuation is available; never performs RPC. */
    public function hasNext(): bool {}
    /** Publish the prefetched first page, or fetch the continuation atomically.
     * A failure preserves current items and position. Cursors are never restarted.
     * @throws \OutOfBoundsException If no next page exists.
     */
    public function next(?Duration $timeout = null): void {}
    /** Current items, or [] before the first next(); never performs RPC.
     * Array edits are independent; contained objects retain identity.
     * @return list<T>
     */
    public function items(): array {}
}
