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
