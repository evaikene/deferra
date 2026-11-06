<?php
declare(strict_types=1);

function check(bool $condition, string $message): void
{
    if (!$condition) {
        throw new RuntimeException($message);
    }
}

function rejects(callable $operation, string $expected): void
{
    try {
        $operation();
    } catch (Throwable $error) {
        check($error instanceof $expected, 'unexpected rejection: ' . get_class($error));
        return;
    }
    throw new RuntimeException('operation unexpectedly succeeded');
}

class CustomJobUError extends JobU\Exception
{
    // A subclass cannot shadow the native getters' base-private metadata.
    public string $_errorCode = 'shadow';
    public string $_category = 'shadow';
    public function __serialize(): array { return []; }
    public function __unserialize(array $data): void {}
}

$classes = [JobU\Exception::class, JobU\ConnectionException::class, JobU\TimeoutException::class,
    JobU\NotFoundException::class, JobU\RevisionConflictException::class, JobU\InvalidCursorException::class,
    JobU\UnsavedChangesException::class, JobU\OutcomeUnknownException::class,
    JobU\UnsupportedFeatureException::class, CustomJobUError::class];

foreach ($classes as $class) {
    $previous = new RuntimeException('previous');
    $error = new $class(message: 'message', code: 17, previous: $previous);
    check($error->getMessage() === 'message' && $error->getCode() === 17 && $error->getPrevious() === $previous,
        "$class standard exception contract");
    check($error->getErrorCode() === null && $error->getCategory() === null, "$class absent daemon metadata");
    rejects(fn() => $error->getErrorCode(1), ArgumentCountError::class);
    rejects(fn() => $error->getCategory(1), ArgumentCountError::class);
    rejects(fn() => serialize($error), Exception::class);
    rejects(fn() => unserialize('O:' . strlen($class) . ':"' . $class . '":0:{}'), Exception::class);
    try {
        throw $error;
    } catch (JobU\Exception $caught) {
        check($caught === $error, "$class catch identity");
    }
}

// Both defaults and non-null Zend-backed metadata must be read from the base
// slots, even through inherited methods. Reflection seeds data without an RPC.
$base = new ReflectionClass(JobU\Exception::class);
$error = new CustomJobUError();
$base->getProperty('_errorCode')->setValue($error, 'jobu.test');
$base->getProperty('_category')->setValue($error, 'test');
check($error->getErrorCode() === 'jobu.test' && $error->getCategory() === 'test', 'metadata values');
check($error->getCode() === 0, 'string code does not replace integer code');
unset($error, $previous, $caught);
gc_collect_cycles();
echo "exceptions ok\n";

class CustomOutcomeError extends JobU\OutcomeUnknownException
{
    public string $_operation = 'shadow';
    public string $_idempotencyKey = 'shadow';
}
$outcome = new CustomOutcomeError();
check($outcome->getOperation() === null && $outcome->getIdempotencyKey() === null, 'absent outcome identity');
$reflection = new ReflectionClass(JobU\OutcomeUnknownException::class);
$reflection->getProperty('_operation')->setValue($outcome, 'job.create');
$reflection->getProperty('_idempotencyKey')->setValue($outcome, 'original-key');
check($outcome->getOperation() === 'job.create' && $outcome->getIdempotencyKey() === 'original-key', 'outcome identity');
rejects(fn() => $outcome->getOperation(1), ArgumentCountError::class);
rejects(fn() => $outcome->getIdempotencyKey(1), ArgumentCountError::class);
