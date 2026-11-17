<?php
declare(strict_types=1);

// Keep diagnostics useful without dumping traces or submitted payloads.
set_exception_handler(static function (Throwable $error): void {
    fwrite(STDERR, $error::class . ': ' . $error->getMessage() . "\n");
    if ($error instanceof JobU\Exception) {
        fwrite(STDERR, 'Code: ' . ($error->getErrorCode() ?? 'none') . "\n");
        fwrite(STDERR, 'Category: ' . ($error->getCategory() ?? 'none') . "\n");
    }
    if ($error instanceof JobU\OutcomeUnknownException) {
        fwrite(STDERR, 'Operation: ' . ($error->getOperation() ?? 'unknown') . "\n");
        fwrite(STDERR, 'Idempotency key: ' . ($error->getIdempotencyKey() ?? 'none') . "\n");
        fwrite(STDERR, "The mutation may have committed. Inspect state before retrying.\n");
    }
    exit(1);
});

/** Validate CLI usage before connecting to the explicitly selected test daemon. */
function exampleClient(array $arguments, int $extraArguments, string $usage): JobU\Client
{
    if (count($arguments) !== 2 + $extraArguments) {
        throw new InvalidArgumentException('Usage: php ' . $usage);
    }
    if (!extension_loaded('jobu')) {
        throw new RuntimeException('Enable the jobu extension first.');
    }
    return new JobU\Client($arguments[1], timeout: JobU\Duration::seconds(5));
}

/** Avoid changing resources from a previous invocation or another application. */
function exampleName(string $topic): string
{
    return 'php-example-' . $topic . '-' . bin2hex(random_bytes(6));
}

/** Print stable IDs for subsequent read-only examples. */
function printExampleJob(JobU\Queue $queue, JobU\Job $job): void
{
    echo 'Queue: ', $queue->getId(), "\n";
    echo 'Job: ', $job->getId(), "\n";
}
