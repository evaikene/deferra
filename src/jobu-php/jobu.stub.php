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
