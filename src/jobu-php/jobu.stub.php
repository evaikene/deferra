<?php

/**
 * @generate-class-entries
 * @generate-legacy-arginfo 80100
 */

namespace JobU;

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
class OutcomeUnknownException extends Exception {}

/** @not-serializable */
class UnsupportedFeatureException extends Exception {}
