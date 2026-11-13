<?php
declare(strict_types=1);

function statisticsCheck(bool $condition, string $message): void
{
    if (!$condition) {
        throw new RuntimeException($message);
    }
}

function statisticsThrows(string $class, callable $call): Throwable
{
    try {
        $call();
    } catch (Throwable $error) {
        statisticsCheck($error instanceof $class, "expected $class, got " . get_class($error));
        return $error;
    }
    throw new RuntimeException("expected $class");
}

$socket = defined('STATISTICS_SOCKET') ? STATISTICS_SOCKET : $argv[1];
$c = new JobU\Client($socket);
$info = $c->getSystemInfo(timeout: JobU\Duration::seconds(5));
statisticsCheck($info->getApiMajor() === 1 && $info->getApiMinor() >= 7, 'compatible system information');
$expected = ['system.info', 'system.stats', 'queue.create', 'queue.get', 'queue.list', 'queue.update', 'queue.suspend', 'queue.resume', 'queue.delete', 'queue.stats', 'job.create', 'job.get', 'job.list', 'job.update', 'job.suspend', 'job.resume', 'job.move', 'job.delete', 'job.run_now', 'run.get', 'run.list', 'run.cancel', 'attempt.get', 'attempt.list', 'attempt.output', 'secret.set', 'secret.list', 'secret.delete', 'schedule.validate', 'schedule.next'];
sort($expected);
statisticsCheck($info->getCapabilities() === $expected, 'audited daemon capability inventory');

$q = $c->createQueue('statistics-' . bin2hex(random_bytes(6)));
$q2 = $c->createQueue('statistics-other-' . bin2hex(random_bytes(6)));
$from = new DateTimeImmutable('2100-01-01T00:00:00.123456Z');
$to = $from->modify('+1 day');
$window = new JobU\TimeRange($from, $to);
$jobs = [
    $q->createCliJob(new JobU\CliPayload('/bin/true'), new JobU\OnceSchedule($from)),
    $q->createHttpJob(new JobU\HttpPayload('https://example.invalid/'), new JobU\OnceSchedule($from)),
    $q->createCliJob(new JobU\CliPayload('/bin/true'), new JobU\OnceSchedule($from)),
];
$q2->createCliJob(new JobU\CliPayload('/bin/true'), new JobU\OnceSchedule($from));

// An ungrouped empty cohort has one zero aggregate; a grouped empty cohort has metadata but no items.
foreach ([JobU\StatisticsGroupBy::None, JobU\StatisticsGroupBy::Job] as $groupBy) {
    $p = $q->statistics(new JobU\StatisticsQuery(groupBy: $groupBy));
    $metadata = $p->getStatisticsMetadata();
    $resolved = $metadata->getWindow();
    statisticsCheck($p->items() === [] && $metadata->getGroupBy() === $groupBy, 'eager metadata');
    statisticsCheck($resolved->getTo()->getTimestamp() - $resolved->getFrom()->getTimestamp() === 86400, 'default day');
    statisticsCheck(abs($resolved->getTo()->getTimestamp() - time()) < 5, 'server current time');
    statisticsCheck($resolved->getTo()->getTimezone()->getName() === 'UTC', 'UTC window');
    statisticsCheck($resolved->getTo() !== $resolved->getTo(), 'fresh immutable dates');
    statisticsCheck($metadata->getMeasurement()->getTiming() === 'wall_clock_derived', 'timing provenance');
    statisticsCheck($metadata->getMeasurement()->getRunnableWait() === 'monotonic_observed', 'wait provenance');
    statisticsCheck($metadata->getMeasurement()->getCapture() === 'persisted_output_flags', 'capture provenance');
    if ($groupBy === JobU\StatisticsGroupBy::None) {
        statisticsCheck($p->hasNext(), 'zero aggregate exists');
        $p->next();
        $g = $p->items()[0];
        statisticsCheck($g->getKey() === null && $g->getRuns()->getTotal() === 0, 'zero aggregate');
        statisticsCheck($g->getRunnableWait()->getSamples() === 0 && $g->getRunnableWait()->getAverageMilliseconds() === null, 'zero samples');
    } else {
        statisticsCheck(!$p->hasNext(), 'empty grouped result');
        statisticsThrows(OutOfBoundsException::class, fn() => $p->next());
    }
    statisticsCheck($metadata === $p->getStatisticsMetadata(), 'stable current metadata');
}
statisticsCheck($c->listQueues()->getStatisticsMetadata() === null, 'nonstatistics page');

foreach (JobU\StatisticsGroupBy::cases() as $groupBy) {
    $query = new JobU\StatisticsQuery(queueId: $q->getId(), planned: $window, groupBy: $groupBy, limit: 1);
    $p = $c->systemStatistics(query: $query);
    $total = 0;
    $keys = [];
    while ($p->hasNext()) {
        $p->next();
        statisticsCheck($p->getStatisticsMetadata()->getWindow() == $window, 'fixed resolved window');
        foreach ($p->items() as $g) {
            $key = $g->getKey();
            statisticsCheck(match ($groupBy) {
                JobU\StatisticsGroupBy::None => $key === null,
                JobU\StatisticsGroupBy::Queue => $key === $q->getId(),
                JobU\StatisticsGroupBy::Job => is_string($key),
                JobU\StatisticsGroupBy::Type => $key instanceof JobU\JobType,
                JobU\StatisticsGroupBy::Origin => $key === JobU\RunOrigin::Scheduled,
                JobU\StatisticsGroupBy::State => $key === JobU\RunState::Scheduled,
            }, 'typed grouping key');
            $keys[] = $key;
            $runs = $g->getRuns();
            $total += $runs->getTotal();
            statisticsCheck(array_sum(array_map($runs->getStateCount(...), JobU\RunState::cases())) === $runs->getTotal(), 'all run states');
            statisticsCheck(array_sum(array_map($runs->getTypeCount(...), JobU\JobType::cases())) === $runs->getTotal(), 'all types');
            statisticsCheck(array_sum(array_map($runs->getOriginCount(...), JobU\RunOrigin::cases())) === $runs->getTotal(), 'all origins');
            $attempts = $g->getAttempts();
            statisticsCheck(array_sum(array_map($attempts->getStateCount(...), JobU\AttemptState::cases())) === $attempts->getTotal(), 'all attempt states');
            statisticsCheck(array_sum(array_map($attempts->getOutcomeCount(...), JobU\AttemptOutcome::cases())) === $attempts->getStateCount(JobU\AttemptState::Completed), 'all outcomes');
            statisticsCheck($attempts->getRetries() === 0, 'future retries');
            statisticsCheck($g->getCapture()->getTruncatedAttempts() === 0 && $g->getCapture()->getLostAttempts() === 0, 'absent capture events');
            foreach ([$g->getScheduleLateness(), $g->getExecutionWallDuration()] as $duration) {
                statisticsCheck($duration->getSamples() === 0 && $duration->getAverageMilliseconds() === null && $duration->getMaximumMilliseconds() === null, 'no timing samples');
            }
            $coverage = $g->getRunnableWaitCoverage();
            statisticsCheck($coverage->getUnfinished() === $runs->getTotal() && $coverage->getComplete() === 0 && $coverage->getPartial() === 0 && $coverage->getUnmeasured() === 0, 'unfinished coverage');
        }
    }
    statisticsCheck($total === 3, 'queue filter excludes other queue');
    statisticsCheck(count($keys) === match ($groupBy) { JobU\StatisticsGroupBy::Job => 3, JobU\StatisticsGroupBy::Type => 2, default => 1 }, 'group cardinality');
    statisticsCheck($query->getQueueId() === $q->getId() && $query->getPlanned() == $window, 'query unchanged');
}

$scoped = new JobU\StatisticsQuery(queueId: $q->getId(), planned: $window, limit: 200);
$q->setName('statistics-renamed-' . bin2hex(random_bytes(6)));
$before = $q->getRevision();
$p = $q->statistics($scoped);
$p->next();
statisticsCheck($p->items()[0]->getRuns()->getTotal() === 3 && $q->isDirty() && $q->getRevision() === $before, 'dirty stable queue read');
statisticsThrows(ValueError::class, fn() => $q->statistics(new JobU\StatisticsQuery(queueId: $q2->getId())));
$q->save();
$p = $q->statistics(new JobU\StatisticsQuery(jobId: $jobs[1]->getId(), planned: $window, type: JobU\JobType::Http, origin: JobU\RunOrigin::Scheduled));
$p->next();
statisticsCheck($p->items()[0]->getRuns()->getTotal() === 1, 'combined filters');

$partial = $q->statistics(new JobU\StatisticsQuery(planned: new JobU\TimeRange(to: $to)))->getStatisticsMetadata()->getWindow();
statisticsCheck($partial == $window, 'upper-only window');
$start = new DateTimeImmutable('-1 hour');
$partial = $q->statistics(new JobU\StatisticsQuery(planned: new JobU\TimeRange(from: $start)))->getStatisticsMetadata()->getWindow();
statisticsCheck($partial->getFrom() == $start && abs($partial->getTo()->getTimestamp() - time()) < 5, 'lower-only window');
statisticsThrows(JobU\Exception::class, fn() => $q->statistics(new JobU\StatisticsQuery(planned: new JobU\TimeRange(from: $from))));
foreach ([0, 201] as $limit) {
    statisticsThrows(ValueError::class, fn() => new JobU\StatisticsQuery(limit: $limit));
}
statisticsThrows(ValueError::class, fn() => new JobU\StatisticsQuery(planned: new JobU\TimeRange($from, $from->modify('+32 days'))));
$q->statistics(new JobU\StatisticsQuery(planned: new JobU\TimeRange($from, $from->modify('+31 days'))));

$q->suspend();
foreach ($jobs as $job) {
    $job->suspend();
    $job->delete();
}
$q->delete();
$p = $q->statistics($scoped);
$p->next();
statisticsCheck($p->items()[0]->getRuns()->getTotal() === 3, 'retained deleted queue');
$historical = $c->getQueueById($q->getId(), includeDeleted: true);
statisticsCheck($historical->statistics($scoped)->getStatisticsMetadata()->getWindow() == $window, 'historically loaded queue');

// Detached results never keep a connection alive; Page does until explicitly released.
$owned = new JobU\Client($socket);
$weak = WeakReference::create($owned);
$p = $owned->systemStatistics(new JobU\StatisticsQuery(queueId: $q->getId(), planned: $window));
$metadata = $p->getStatisticsMetadata();
$owned->close();
$p->next();
$g = $p->items()[0];
statisticsThrows(JobU\ConnectionException::class, fn() => $owned->getSystemInfo());
statisticsThrows(JobU\ConnectionException::class, fn() => $owned->systemStatistics());
unset($owned, $p);
gc_collect_cycles();
statisticsCheck($weak->get() === null && $g->getRuns()->getTotal() === 3 && $metadata->getWindow() == $window, 'detached result lifetime');
foreach ([$g, $g->getRuns(), $g->getAttempts(), $g->getCapture(), $g->getScheduleLateness(), $g->getRunnableWaitCoverage(), $metadata, $metadata->getMeasurement()] as $value) {
    statisticsThrows(Error::class, fn() => $value->dynamic = 1);
    statisticsThrows(Exception::class, fn() => serialize($value));
}
$c->close();
return true;
