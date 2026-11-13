<?php
declare(strict_types=1);

function check(bool $condition, string $message): void
{
    if (!$condition) {
        throw new RuntimeException($message);
    }
}

$extension = new ReflectionExtension('jobu');
check($extension->getVersion() !== false && $extension->getVersion() !== '', 'extension version');
check($extension->getFunctions() === [], 'no premature RPC functions');

$enums = [
    'RunCancellationDisposition' => ['Completed' => 'completed', 'Requested' => 'requested'],
    'QueueState' => ['Active' => 'active', 'Suspending' => 'suspending', 'Suspended' => 'suspended', 'Deleted' => 'deleted'],
    'JobState' => ['Active' => 'active', 'Suspending' => 'suspending', 'Suspended' => 'suspended', 'Deleted' => 'deleted',
        'Succeeded' => 'succeeded', 'Failed' => 'failed', 'Cancelled' => 'cancelled'],
    'RunState' => ['Scheduled' => 'scheduled', 'Running' => 'running', 'RetryWait' => 'retry_wait',
        'Succeeded' => 'succeeded', 'Failed' => 'failed', 'Interrupted' => 'interrupted', 'Cancelled' => 'cancelled'],
    'AttemptState' => ['Pending' => 'pending', 'Running' => 'running', 'Completed' => 'completed'],
    'AttemptOutcome' => ['Succeeded' => 'succeeded', 'Failed' => 'failed', 'Interrupted' => 'interrupted', 'Cancelled' => 'cancelled'],
    'RetryMode' => ['Blocking' => 'blocking', 'Reschedule' => 'reschedule'],
    'RecoveryPolicy' => ['FailInterrupted' => 'fail_interrupted', 'RetryInterrupted' => 'retry_interrupted'],
    'JobType' => ['Cli' => 'cli', 'Http' => 'http'],
    'RunOrigin' => ['Scheduled' => 'scheduled', 'Manual' => 'manual'],
    'StatisticsGroupBy' => ['None' => 'none', 'Queue' => 'queue', 'Job' => 'job', 'Type' => 'type', 'Origin' => 'origin', 'State' => 'state'],
    'OutputStatus' => ['Available' => 'available', 'Pending' => 'pending', 'NotCaptured' => 'not_captured', 'Lost' => 'lost'],
    'OutputChannel' => ['Stdout' => 'stdout', 'Stderr' => 'stderr', 'Body' => 'body', 'Headers' => 'headers'],
];

foreach ($enums as $name => $expected) {
    $class = 'JobU\\' . $name;
    $reflection = new ReflectionEnum($class);
    check($reflection->isInternal() && $reflection->getExtensionName() === 'jobu', "$class ownership");
    check($reflection->isBacked() && (string) $reflection->getBackingType() === 'string', "$class backing type");
    $actual = [];
    foreach ($class::cases() as $case) {
        $actual[$case->name] = $case->value;
        check($class::from($case->value) === $case, "$class from");
        check($class::tryFrom($case->value) === $case, "$class tryFrom");
        check(unserialize(serialize($case)) === $case, "$class native serialization");
    }
    check($actual === $expected, "$class case names and wire values");
    check($class::tryFrom('not-a-case') === null, "$class unknown case");
    try {
        $class::from('not-a-case');
        throw new RuntimeException("$class accepted unknown case");
    } catch (ValueError) {
    }
}

$exceptions = ['Exception', 'ConnectionException', 'TimeoutException', 'NotFoundException',
    'RevisionConflictException', 'InvalidCursorException', 'UnsavedChangesException',
    'OutcomeUnknownException', 'UnsupportedFeatureException'];
foreach ($exceptions as $name) {
    $reflection = new ReflectionClass('JobU\\' . $name);
    check($reflection->isInternal() && $reflection->getExtensionName() === 'jobu', "$name ownership");
    check($reflection->getParentClass()->getName() === ($name === 'Exception' ? 'Exception' : 'JobU\\Exception'), "$name parent");
    check($reflection->getConstructor()->getDeclaringClass()->getName() === 'Exception', "$name inherited constructor");
    foreach (['getErrorCode', 'getCategory'] as $methodName) {
        $method = $reflection->getMethod($methodName);
        check($method->isPublic() && !$method->isStatic(), "$name::$methodName visibility");
        check($method->getNumberOfParameters() === 0, "$name::$methodName parameters");
        check((string) $method->getReturnType() === '?string', "$name::$methodName return type");
    }
}

$expectedClasses = array_map(fn(string $name): string => 'JobU\\' . $name, [...array_keys($enums), ...$exceptions, 'Duration', 'Client', 'SecretReference', 'Schedule', 'OnceSchedule', 'CronSchedule', 'JobAttributes', 'CliJobAttributes', 'HttpJobAttributes', 'QueueDefaults', 'CliPayload', 'HttpPayload', 'HttpHeader', 'TimeRange', 'RunQuery', 'StatisticsQuery', 'SystemInfo', 'SecretMetadata', 'ExecutionResult', 'CliExecutionResult', 'HttpExecutionResult', 'OutputChunk', 'Queue', 'Job', 'Run', 'RunSummary', 'Attempt', 'AttemptSummary', 'Page']);
$actualClasses = $extension->getClassNames();
sort($expectedClasses);
sort($actualClasses);
check($actualClasses === $expectedClasses, 'exact implemented class surface');
$queue = new ReflectionClass(JobU\Queue::class);
check($queue->isFinal() && $queue->getConstructor()->isPrivate(), 'queue construction');
$creation = new ReflectionMethod(JobU\Client::class, 'createQueue');
check(array_map(fn($p) => $p->getName(), $creation->getParameters()) ===
    ['name', 'weight', 'concurrencyLimit', 'recoveryPolicy', 'defaults', 'historyRetention',
     'runnableWaitWarning', 'idempotencyKey', 'timeout'], 'queue creation named parameters');
check((string)$creation->getReturnType() === JobU\Queue::class, 'queue creation return');
check((string)$queue->getMethod('getHistoryRetention')->getReturnType() === '?JobU\\Duration', 'nullable retention');
check((string)$queue->getMethod('getDeletedAt')->getReturnType() === '?DateTimeImmutable', 'nullable deletion date');
check(!$queue->hasMethod('statistics') && $queue->hasMethod('listJobs'), 'stage boundary');
$page = new ReflectionClass(JobU\Page::class);
check($page->isFinal() && $page->getConstructor()->isPrivate(), 'Page construction');
check(array_map(fn($m) => $m->getName(), $page->getMethods()) === ['__construct', 'hasNext', 'next', 'items'], 'Page methods');
check((string)$page->getMethod('hasNext')->getReturnType() === 'bool', 'Page hasNext return');
check((string)$page->getMethod('items')->getReturnType() === 'array', 'Page items return');
$next = $page->getMethod('next');
check((string)$next->getReturnType() === 'void', 'Page next return');
check($next->getNumberOfRequiredParameters() === 0 && $next->getNumberOfParameters() === 1, 'Page next arity');
$timeout = $next->getParameters()[0];
check($timeout->getName() === 'timeout' && (string)$timeout->getType() === '?JobU\\Duration' && $timeout->getDefaultValue() === null, 'Page timeout');
check(!class_exists('JobUTestPages', false), 'fixture factories excluded');
check((new ReflectionClass(JobU\Client::class))->hasMethod('listQueues'), 'public definition lists');

foreach ([
    [JobU\Client::class, 'listQueues', ['state', 'includeDeleted', 'limit', 'timeout'], ['?JobU\QueueState', 'bool', 'int', '?JobU\Duration'], [null, false, 100, null]],
    [JobU\Client::class, 'listJobs', ['queueName', 'queueId', 'state', 'type', 'includeDeleted', 'limit', 'timeout'], ['?string', '?string', '?JobU\JobState', '?JobU\JobType', 'bool', 'int', '?JobU\Duration'], [null, null, null, null, false, 100, null]],
    [JobU\Queue::class, 'listJobs', ['state', 'type', 'includeDeleted', 'limit', 'timeout'], ['?JobU\JobState', '?JobU\JobType', 'bool', 'int', '?JobU\Duration'], [null, null, false, 100, null]],
] as [$class, $name, $names, $types, $defaults]) {
    $method = new ReflectionMethod($class, $name);
    check((string)$method->getReturnType() === JobU\Page::class, "$class::$name return type");
    check(array_map(fn($p) => $p->getName(), $method->getParameters()) === $names, "$class::$name parameter names");
    check(array_map(fn($p) => (string)$p->getType(), $method->getParameters()) === $types, "$class::$name parameter types");
    check(array_map(fn($p) => $p->getDefaultValue(), $method->getParameters()) === $defaults, "$class::$name parameter defaults");
}

echo "reflection ok\n";

foreach ([
    [JobU\Queue::class, 'getJob', 'JobU\Job', ['id', 'timeout'], ['string', '?JobU\Duration'], [null]],
    [JobU\Queue::class, 'listRuns', 'JobU\Page', ['query', 'timeout'], ['?JobU\RunQuery', '?JobU\Duration'], [null, null]],
    [JobU\Job::class, 'getRun', 'JobU\Run', ['id', 'timeout'], ['string', '?JobU\Duration'], [null]],
    [JobU\Client::class, 'listRuns', 'JobU\Page', ['query', 'timeout'], ['?JobU\RunQuery', '?JobU\Duration'], [null, null]],
    [JobU\Run::class, 'getAttempt', 'JobU\Attempt', ['number', 'timeout'], ['int', '?JobU\Duration'], [null]],
    [JobU\Job::class, 'listRuns', 'JobU\Page', ['query', 'timeout'], ['?JobU\RunQuery', '?JobU\Duration'], [null, null]],
    [JobU\RunSummary::class, 'load', 'JobU\Run', ['timeout'], ['?JobU\Duration'], [null]],
    [JobU\Run::class, 'refresh', 'void', ['timeout'], ['?JobU\Duration'], [null]],
    [JobU\Run::class, 'cancel', 'JobU\RunCancellationDisposition', ['timeout'], ['?JobU\Duration'], [null]],
    [JobU\Run::class, 'listAttempts', 'JobU\Page', ['limit', 'timeout'], ['int', '?JobU\Duration'], [100, null]],
    [JobU\AttemptSummary::class, 'load', 'JobU\Attempt', ['timeout'], ['?JobU\Duration'], [null]],
    [JobU\Attempt::class, 'refresh', 'void', ['timeout'], ['?JobU\Duration'], [null]],
    [JobU\Run::class, 'getOutput', '?string', ['maxBytes', 'timeout'], ['int', '?JobU\Duration'], [4096, null]],
    [JobU\Attempt::class, 'getOutput', '?string', ['maxBytes', 'timeout'], ['int', '?JobU\Duration'], [4096, null]],
    [JobU\Attempt::class, 'readOutput', 'JobU\OutputChunk', ['channel', 'offset', 'length', 'timeout'], ['JobU\OutputChannel', 'int', 'int', '?JobU\Duration'], [0, 16384, null]],
    [JobU\Attempt::class, 'getResult', '?JobU\ExecutionResult', [], [], []],
] as [$class, $name, $return, $names, $types, $defaults]) {
    $m = new ReflectionMethod($class, $name);
    $p = $m->getParameters();
    check((string)$m->getReturnType() === $return, "$class::$name return");
    check(array_map(fn($v) => $v->getName(), $p) === $names, "$class::$name names");
    check(array_map(fn($v) => (string)$v->getType(), $p) === $types, "$class::$name types");
    $optional = array_filter($p, fn($v) => $v->isOptional());
    check(array_values(array_map(fn($v) => $v->getDefaultValue(), $optional)) === $defaults, "$class::$name defaults");
}
foreach ([JobU\RunSummary::class, JobU\AttemptSummary::class, JobU\Attempt::class] as $name) {
    $c = new ReflectionClass($name);
    check($c->isFinal() && $c->getConstructor()->isPrivate(), "$name construction");
    if ($name !== JobU\Attempt::class) {
        check(!$c->hasMethod('getOutput') && !$c->hasMethod('readOutput'), "$name omits output");
    }
}
foreach (['getAttributes', 'getPayload', 'getResult'] as $method) {
    check(!(new ReflectionClass(JobU\RunSummary::class))->hasMethod($method), "RunSummary omits $method");
}
check(!(new ReflectionClass(JobU\AttemptSummary::class))->hasMethod('getResult'), 'AttemptSummary omits result');

foreach (['getJob', 'getRun', 'getAttempt', 'listAttempts', 'getOutput', 'readOutput'] as $method) {
    check(!(new ReflectionClass(JobU\Client::class))->hasMethod($method), "Client omits $method");
}
