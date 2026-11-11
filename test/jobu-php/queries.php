<?php
declare(strict_types=1);

$check = static function (bool $condition, string $message): void {
    if (!$condition) { throw new RuntimeException($message); }
};
$reject = static function (string $type, callable $operation) use ($check): void {
    try { $operation(); }
    catch (Throwable $error) {
        $check($error instanceof $type, "expected $type, got " . $error::class);
        return;
    }
    throw new RuntimeException("expected $type");
};

$from = new DateTime('2030-01-01T02:00:00.123456+02:00');
$to = new DateTimeImmutable('2030-01-02T00:00:00.123456Z');
$range = new JobU\TimeRange($from, $to);
$from->modify('+2 days');
$check($range->getFrom()->format('Y-m-d H:i:s.u e') === '2030-01-01 00:00:00.123456 UTC', 'copied UTC input');
$check($range->getFrom() !== $range->getFrom(), 'fresh date output');
$check((new JobU\TimeRange)->getTo() === null, 'unbounded range');
$reject(ValueError::class, fn() => new JobU\TimeRange($to, $to));
$reject(ValueError::class, fn() => new JobU\TimeRange($from, $to));
$reject(TypeError::class, fn() => new JobU\TimeRange('2030-01-01'));
$reject(ValueError::class, fn() => new JobU\TimeRange(new DateTimeImmutable('9999-01-01')));
$badDate = new class extends DateTimeImmutable { public function __construct() {} };
$reject(ValueError::class, fn() => new JobU\TimeRange($badDate));

$queue = '019c0000-0000-7000-8000-000000000001';
$states = [JobU\RunState::Running, JobU\RunState::Failed];
$query = new JobU\RunQuery(queueId: $queue, states: $states, origin: JobU\RunOrigin::Manual,
    type: JobU\JobType::Cli, planned: $range, limit: 200);
$states[0] = JobU\RunState::Scheduled;
$check($query->getQueueId() === $queue && $query->getJobId() === null, 'UUID filters');
$check($query->getStates() === [JobU\RunState::Running, JobU\RunState::Failed], 'copied state list');
$check($query->getState() === null && $query->getOrigin() === JobU\RunOrigin::Manual && $query->getType() === JobU\JobType::Cli, 'typed filters');
$check($query->getPlanned() == $range && $query->getLimit() === 200, 'range and limit');
$check($query->getStarted() == new JobU\TimeRange && $query->getCompleted()->getTo() === null, 'unrestricted ranges');
$check(new JobU\RunQuery === new JobU\RunQuery ? false : true, 'identity comparison');
$check(new JobU\RunQuery == new JobU\RunQuery(planned: new JobU\TimeRange), 'normalized unbounded range');
$check(new JobU\RunQuery(limit: 1) != new JobU\RunQuery(limit: 2), 'native content comparison');
foreach ([0, 201, -1, PHP_INT_MAX] as $limit) {
    $reject(ValueError::class, fn() => new JobU\RunQuery(limit: $limit));
    $reject(ValueError::class, fn() => new JobU\StatisticsQuery(limit: $limit));
}
foreach ([[], [JobU\RunState::Running, JobU\RunState::Running], [1 => JobU\RunState::Running]] as $invalid) {
    $reject(ValueError::class, fn() => new JobU\RunQuery(states: $invalid));
}
$reject(TypeError::class, fn() => new JobU\RunQuery(states: ['running']));
$reject(TypeError::class, fn() => new JobU\RunQuery(state: JobU\JobState::Active));
$reject(ValueError::class, fn() => new JobU\RunQuery(state: JobU\RunState::Running, states: [JobU\RunState::Failed]));
foreach (['bad', strtoupper($queue), '00000000-0000-0000-0000-000000000000'] as $id) {
    $reject(ValueError::class, fn() => new JobU\RunQuery(queueId: $id));
}
$stats = new JobU\StatisticsQuery(queueId: $queue, jobId: $queue, type: JobU\JobType::Http,
    origin: JobU\RunOrigin::Scheduled, planned: $range, groupBy: JobU\StatisticsGroupBy::State, limit: 1);
$check($stats->getQueueId() === $queue && $stats->getJobId() === $queue && $stats->getType() === JobU\JobType::Http, 'statistics identifiers');
$check($stats->getOrigin() === JobU\RunOrigin::Scheduled && $stats->getGroupBy() === JobU\StatisticsGroupBy::State && $stats->getLimit() === 1, 'statistics selections');
$check($stats->getPlanned() == $range, 'statistics range');
$check((new JobU\StatisticsQuery)->getPlanned()->getFrom() === null && (new JobU\StatisticsQuery)->getGroupBy() === JobU\StatisticsGroupBy::None, 'server defaults unresolved');
$start = new DateTimeImmutable('2030-01-01Z');
new JobU\StatisticsQuery(planned: new JobU\TimeRange($start, $start->modify('+31 days')));
$reject(ValueError::class, fn() => new JobU\StatisticsQuery(planned: new JobU\TimeRange($start, $start->modify('+31 days +1 microsecond'))));
$reject(TypeError::class, fn() => new JobU\StatisticsQuery(groupBy: null));

foreach ([$range, $query, $stats] as $object) {
    $reject(Error::class, fn() => clone $object);
    $reject(Exception::class, fn() => serialize($object));
    $reject(Error::class, function () use ($object) { $object->extra = 1; });
    $reject(Exception::class, fn() => $object->__construct());
    $check($object == $object, 'self comparison');
}
foreach (['TimeRange', 'RunQuery', 'StatisticsQuery', 'SystemInfo', 'SecretMetadata', 'CliExecutionResult', 'HttpExecutionResult', 'OutputChunk'] as $class) {
    $reflection = new ReflectionClass('JobU\\' . $class);
    $check($reflection->isFinal() && $reflection->isInternal(), "$class final native class");
}
foreach (['SystemInfo','SecretMetadata','CliExecutionResult','HttpExecutionResult','OutputChunk'] as $class) {
    $class = 'JobU\\' . $class;
    $reject(Error::class, fn() => new $class);
}
$check((new ReflectionClass(JobU\ExecutionResult::class))->isAbstract(), 'abstract result base');
$check((string)(new ReflectionMethod(JobU\OutputChunk::class, 'getData'))->getReturnType() === 'string', 'binary output signature');
$check((string)(new ReflectionMethod(JobU\HttpExecutionResult::class, 'getDuration'))->getReturnType() === '?JobU\\Duration', 'duration signature');
echo "queries ok\n";
