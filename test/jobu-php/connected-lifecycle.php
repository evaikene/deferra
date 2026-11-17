<?php
declare(strict_types=1);

// The same production-only scenario runs in CLI children and reused Embed
// requests. Its final graph deliberately survives until request shutdown.
function connectedCheck(bool $condition, string $message): void
{
    if (!$condition) {
        throw new RuntimeException($message);
    }
}

function connectedClosed(callable $operation): void
{
    try {
        $operation();
    } catch (JobU\ConnectionException) {
        return;
    }
    throw new RuntimeException('closed Client accepted an RPC');
}

function connectedRejects(string $class, callable $operation): void
{
    try {
        $operation();
    } catch (Throwable $error) {
        connectedCheck($error instanceof $class, 'expected ' . $class . ', got ' . $error::class);
        return;
    }
    throw new RuntimeException('expected ' . $class);
}

connectedCheck(!isset($GLOBALS['connectedGraph']), 'previous request graph is gone');
$socket = defined('JOBU_TEST_SOCKET') ? JOBU_TEST_SOCKET : $argv[1];
$client = new JobU\Client($socket);
$peer = new JobU\Client($socket);
$queue = $client->createQueue('connected-' . bin2hex(random_bytes(8)));
$queue->defaults()->cli()->setStdoutLimit(64);
$queue->defaults()->cli()->setOutputCapture('always');
$queue->save();
$job = $queue->createCliJob(
    new JobU\CliPayload('/usr/bin/printf', ['connected-output']),
    new JobU\OnceSchedule(new DateTimeImmutable('+1 day'))
);
$run = $job->runNow();

// Synchronize on committed terminal state, with a bounded deadline rather than
// assuming that a fixed sleep is enough for the daemon to execute the job.
$deadline = hrtime(true) + 5_000_000_000;
do {
    $run->refresh();
    if ($run->getState() === JobU\RunState::Succeeded) {
        break;
    }
    connectedCheck(hrtime(true) < $deadline, 'connected fixture completed before deadline');
    usleep(1000);
} while (true);

$queueId = $queue->getId();
$jobId = $job->getId();
$runId = $run->getId();
$attempt = $run->getAttempt(1);
$runs = $job->listRuns();
$runs->next();
$summary = $runs->items()[0];
$attempts = $run->listAttempts();
$attempts->next();
$attemptSummary = $attempts->items()[0];
$statistics = $queue->statistics();
$statistics->next();
$group = $statistics->items()[0];
$metadata = $statistics->getStatisticsMetadata();
$result = $attempt->getResult();
$chunk = $attempt->readOutput(JobU\OutputChannel::Stdout);
$info = $client->getSystemInfo();
connectedCheck($chunk->getData() === 'connected-output' && $result->getExitCode() === 0, 'real execution snapshots');

// All connected owners reject cloning and serialization, including crafted
// unserialization input. Native backed enums retain their own PHP policy.
$immutable = [
    $client, $queue, $job, $run, $attempt, $summary, $attemptSummary, $runs, $group,
    $metadata, $result, $chunk, $info, $group->getRuns(), $group->getAttempts(),
    $group->getCapture(), $group->getScheduleLateness(), $group->getRunnableWaitCoverage(),
    $metadata->getMeasurement(),
];
foreach ($immutable as $object) {
    connectedRejects(Error::class, fn() => clone $object);
    connectedRejects(Exception::class, fn() => serialize($object));
    $class = $object::class;
    connectedRejects(Exception::class, fn() => unserialize('O:' . strlen($class) . ':"' . $class . '":0:{}'));
}
unset($immutable, $object);

$weak = WeakReference::create($client);
$attached = [$queue->defaults(), $job->attributes(), $job->payload()];
$copies = [$queue->getDefaults(), $job->getAttributes(), $job->getPayload()];
$client->close();
$client->close();
connectedCheck($peer->getSystemInfo()->getApiMajor() === 1, 'peer remains usable');
connectedCheck($job->getId() !== '' && $summary->getId() !== '', 'cached resources survive close');
connectedClosed(fn() => $attempt->refresh());
connectedClosed(fn() => $summary->load());
connectedClosed(fn() => $attemptSummary->load());
unset($client, $queue, $job, $run, $attempt, $summary, $attemptSummary, $runs, $attempts, $statistics);
gc_collect_cycles();
connectedCheck($weak->get() !== null, 'attached configuration retains resource and Client');
unset($attached);
gc_collect_cycles();
connectedCheck($weak->get() === null, 'detached configuration and results release Client');
connectedCheck($chunk->getData() === 'connected-output' && $group->getRuns()->getTotal() >= 1, 'detached snapshots remain readable');

// Retain real resources, their internal attachment cycles, prefetched pages,
// summaries and detached results. RSHUTDOWN must close the open peer before
// Zend frees this graph; the next Embed request must start without it.
$page = $peer->listQueues(limit: 1);
$page->next();
$heldQueue = $peer->getQueueById($queueId);
$jobs = $heldQueue->listJobs();
$jobs->next();
$heldJob = $heldQueue->getJob($jobId);
$history = $heldJob->listRuns();
$history->next();
$heldSummary = $history->items()[0];
$heldRun = $heldJob->getRun($runId);
$GLOBALS['connectedGraph'] = [
    $peer, $page, $heldQueue, $heldQueue->defaults(), $jobs, $heldJob,
    $heldJob->attributes(), $heldJob->payload(), $history, $heldSummary, $heldRun,
    $heldRun->listAttempts(), $heldRun->getAttempt(1), $copies, $info, $chunk, $result, $metadata, $group,
];
return true;
