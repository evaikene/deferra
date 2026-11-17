<?php
declare(strict_types=1);
require __DIR__ . '/common.php';

$client = exampleClient($argv, 0, 'cron.php SOCKET');
try {
    $schedule = new JobU\CronSchedule('0 9 * * MON-FRI', new DateTimeZone('Europe/Tallinn'));
    $client->validateSchedule($schedule);
    foreach ($client->nextScheduleOccurrences($schedule, new DateTimeImmutable('now'), count: 3) as $instant) {
        echo 'Next UTC occurrence: ', $instant->format('Y-m-d\TH:i:s.u\Z'), "\n";
    }

    // Suspend before creating the recurring job so trying the example cannot
    // start ongoing execution. Resume this queue explicitly when ready.
    $queue = $client->createQueue(exampleName('cron'));
    $queue->suspend();
    $job = $queue->createCliJob(
        new JobU\CliPayload('/usr/bin/printf', ['%s\n', 'Weekday report']),
        $schedule,
        name: 'weekday-report',
    );
    printExampleJob($queue, $job);
    echo "Queue remains suspended. Creation already schedules its first occurrence.\n";
} finally {
    $client->close();
}
