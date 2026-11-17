<?php
declare(strict_types=1);
require __DIR__ . '/common.php';

$client = exampleClient($argv, 0, 'enqueue-cli.php SOCKET');
try {
    $defaults = new JobU\QueueDefaults();
    $defaults->cli()->setTimeout(JobU\Duration::seconds(10));
    $defaults->cli()->setOutputCapture('always');
    $queue = $client->createQueue(exampleName('cli'), defaults: $defaults);

    // The daemon executes this argv directly, without shell expansion or an
    // inherited PATH. printf must exist at this absolute path on its host.
    $payload = new JobU\CliPayload('/usr/bin/printf', ['%s\n', 'Hello from JobU']);
    $job = $queue->createCliJob($payload, JobU\OnceSchedule::now(), name: 'hello');
    printExampleJob($queue, $job);
    echo "Submission accepted; use output.php to observe completion.\n";
} finally {
    // Closing the management connection does not cancel submitted work.
    $client->close();
}
