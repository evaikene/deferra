<?php
declare(strict_types=1);
require __DIR__ . '/common.php';

$client = exampleClient($argv, 2, 'output.php SOCKET QUEUE_ID JOB_ID');
try {
    $job = $client->getQueueById($argv[2])->getJob($argv[3]);
    $runs = $job->listRuns(new JobU\RunQuery(limit: 1));
    if (!$runs->hasNext()) {
        throw new RuntimeException('No retained run is available.');
    }
    $runs->next();
    $run = $runs->items()[0]->load();

    // Submission is not completion. Poll this run's explicit snapshot, with a
    // bounded overall wait and bounded individual refresh calls.
    $deadline = hrtime(true) + 30_000_000_000;
    while ($run->getCompletedAt() === null) {
        if (hrtime(true) >= $deadline) {
            throw new RuntimeException('Run is still pending; no work was cancelled.');
        }
        usleep(100_000);
        $run->refresh(timeout: JobU\Duration::seconds(2));
    }
    echo 'Run state: ', $run->getState()->value, "\n";

    // The convenience method returns a tail, preferring CLI stderr or HTTP body.
    // Base64 makes arbitrary bytes safe to display without assuming UTF-8.
    $tail = $run->getOutput();
    echo 'Tail (base64): ', $tail === null ? '(unavailable)' : base64_encode($tail), "\n";

    $latest = null;
    $attempts = $run->listAttempts();
    while ($attempts->hasNext()) {
        $attempts->next();
        foreach ($attempts->items() as $summary) {
            if ($latest === null || $summary->getNumber() > $latest->getNumber()) {
                $latest = $summary;
            }
        }
    }
    if ($latest === null) {
        echo "No attempt was executed.\n";
    } else {
        $attempt = $latest->load();
        $channel = $run->getType() === JobU\JobType::Cli ? JobU\OutputChannel::Stdout : JobU\OutputChannel::Body;
        $chunk = $attempt->readOutput($channel, offset: 0, length: 4096);
        echo 'Capture: ', $chunk->getStatus()->value, "\n";
        echo 'First chunk (base64): ', base64_encode($chunk->getData()), "\n";
        echo 'Next retained offset: ', $chunk->getNextOffset() ?? '(end)', "\n";
        echo 'Truncated: ', $chunk->isTruncated() ? 'yes' : 'no', "\n";
        echo 'Capture lost: ', $chunk->isCaptureLost() ? 'yes' : 'no', "\n";
    }
} finally {
    $client->close();
}
