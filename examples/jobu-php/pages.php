<?php
declare(strict_types=1);
require __DIR__ . '/common.php';

$client = exampleClient($argv, 0, 'pages.php SOCKET');
try {
    $queue = $client->createQueue(exampleName('pages'));
    $queue->suspend();
    foreach (['first', 'second', 'third'] as $name) {
        $queue->createCliJob(new JobU\CliPayload('/usr/bin/true'), JobU\OnceSchedule::now(), name: $name);
    }

    // Queue scope pins the stable UUID. The first next() publishes the prefetched
    // page; later calls fetch continuations, without restarting the listing.
    $page = $queue->listJobs(limit: 2);
    $pageNumber = 0;
    try {
        while ($page->hasNext()) {
            $page->next();
            echo 'Page: ', ++$pageNumber, "\n";
            foreach ($page->items() as $job) {
                echo $job->getId(), ' ', $job->getName(), "\n";
            }
        }
    } catch (JobU\InvalidCursorException $error) {
        fwrite(STDERR, "Listing stopped. Starting again may repeat items; choose explicitly.\n");
        throw $error;
    }
    echo 'Suspended queue: ', $queue->getId(), "\n";
} finally {
    $client->close();
}
