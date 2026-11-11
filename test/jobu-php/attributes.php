<?php
declare(strict_types=1);

function check(bool $ok, string $message): void {
    if (!$ok) { throw new RuntimeException($message); }
}
function rejects(string $class, callable $operation): void {
    try { $operation(); } catch (Throwable $error) {
        check($error instanceof $class, 'Expected ' . $class . ', got ' . get_class($error));
        return;
    }
    throw new RuntimeException('Expected ' . $class);
}

$cases = [
    [JobU\JobAttributes::class, 'Timeout', 'job.timeout', JobU\Duration::seconds(12)],
    [JobU\JobAttributes::class, 'OutputCapture', 'output.capture', 'always'],
    [JobU\JobAttributes::class, 'RetryInitialDelay', 'retry.initial_delay', JobU\Duration::milliseconds(25)],
    [JobU\JobAttributes::class, 'RetryMaxDelay', 'retry.max_delay', JobU\Duration::seconds(5)],
    [JobU\JobAttributes::class, 'RetryMaxAttempts', 'retry.max_attempts', 3],
    [JobU\JobAttributes::class, 'RetryJitter', 'retry.jitter', 0.2],
    [JobU\JobAttributes::class, 'RetryMultiplier', 'retry.multiplier', 2.5],
    [JobU\JobAttributes::class, 'RetryMode', 'retry.mode', JobU\RetryMode::Blocking],
    [JobU\JobAttributes::class, 'RetryStrategy', 'retry.strategy', 'exponential'],
    [JobU\CliJobAttributes::class, 'RetryExitCodes', 'cli.retry_exit_codes', ['1', '10-20']],
    [JobU\CliJobAttributes::class, 'TerminationGrace', 'cli.termination_grace', JobU\Duration::seconds(2)],
    [JobU\CliJobAttributes::class, 'StdoutLimit', 'output.stdout_limit', 100],
    [JobU\CliJobAttributes::class, 'StderrLimit', 'output.stderr_limit', 200],
    [JobU\HttpJobAttributes::class, 'FollowRedirects', 'http.follow_redirects', true],
    [JobU\HttpJobAttributes::class, 'IdempotencyKey', 'http.idempotency_key', true],
    [JobU\HttpJobAttributes::class, 'TlsVerify', 'http.tls_verify', false],
    [JobU\HttpJobAttributes::class, 'MaxRedirects', 'http.max_redirects', 3],
    [JobU\HttpJobAttributes::class, 'HttpBodyLimit', 'output.http_body_limit', 300],
    [JobU\HttpJobAttributes::class, 'HttpHeadersLimit', 'output.http_headers_limit', 400],
    [JobU\HttpJobAttributes::class, 'RetryErrors', 'http.retry_errors', ['connect', 'timeout']],
    [JobU\HttpJobAttributes::class, 'RetryStatuses', 'http.retry_statuses', ['429', '500-599']],
];
foreach ($cases as [$class, $suffix, $name, $value]) {
    $object = new $class;
    $get = 'get' . $suffix;
    $set = 'set' . $suffix;
    check($object->$get() === null && !$object->has($name), $name . ' absent');
    $object->$set($value);
    check($object->$get() == $value && $object->has($name), $name . ' typed roundtrip');
    $generic = $value instanceof JobU\RetryMode ? $value->value : $value;
    check($object->get($name) == $generic, $name . ' generic representation');
    $copy = clone $object;
    check($copy !== $object && $copy == $object, $name . ' clone equality');
    $copy->set($name, $generic);
    $method = new ReflectionMethod($class, $set);
    check($method->getNumberOfRequiredParameters() === 1 && (string)$method->getReturnType() === 'void', 'setter signature');
    rejects(TypeError::class, fn() => $object->set($name, null));
    check($copy == $object, 'failed mutation is atomic');
}

$q = new JobU\QueueDefaults;
$cli = $q->cli();
$http = $q->http();
check($q->cli() === $cli && $q->http() === $http, 'stable views');
$cli->setTimeout(JobU\Duration::seconds(4));
check($http->getTimeout()->getMilliseconds() === 4000, 'common backing');
$cli->setStdoutLimit(10);
$http->setHttpBodyLimit(20);
rejects(ValueError::class, fn() => $cli->get('http.tls_verify'));
rejects(ValueError::class, fn() => $http->has('cli.termination_grace'));
rejects(ValueError::class, fn() => $cli->set('http.tls_verify', false));
$qc = clone $q;
check($qc == $q && $qc->cli() !== $cli, 'clone owns views');
$qc->cli()->setTimeout(JobU\Duration::seconds(8));
check($q->get('job.timeout')->getMilliseconds() === 4000 && $qc != $q, 'clone isolation');
$cc = clone $cli;
$cc->setStdoutLimit(90);
check($cli->getStdoutLimit() === 10, 'view clone isolation');
$q->clear('job.timeout');
check($cli->getTimeout() === null && $http->getTimeout() === null, 'clear shared');
$q->clear('job.timeout');
foreach (['bogus', 'JOB.timeout', 'job.timeout' . "\0"] as $name) {
    rejects(ValueError::class, fn() => $q->set($name, 1));
    rejects(ValueError::class, fn() => $q->get($name));
    rejects(ValueError::class, fn() => $q->has($name));
    rejects(ValueError::class, fn() => $q->clear($name));
}

$input = ['1'];
$cli->setRetryExitCodes($input);
$input[0] = '2';
$output = $cli->getRetryExitCodes();
$output[0] = '3';
check($cli->getRetryExitCodes() === ['1'], 'list isolation');
$referenced = '4';
$cli->setRetryExitCodes([&$referenced]);
$referenced = '5';
check($cli->getRetryExitCodes() === ['4'], 'reference isolation');
foreach ([[1], [['1']], ['a' => '1'], ['1-3', '2-4']] as $bad) {
    rejects(is_int($bad[0] ?? null) || is_array($bad[0] ?? null) ? TypeError::class : ValueError::class,
        fn() => $cli->setRetryExitCodes($bad));
}
rejects(ValueError::class, fn() => $cli->setTimeout(JobU\Duration::milliseconds(PHP_INT_MAX)));
rejects(ValueError::class, fn() => $cli->setTimeout(JobU\Duration::milliseconds(PHP_INT_MIN)));
rejects(ValueError::class, fn() => $cli->setTimeout(JobU\Duration::milliseconds(0)));
rejects(ValueError::class, fn() => $cli->setRetryJitter(NAN));
rejects(ValueError::class, fn() => $cli->setRetryMultiplier(INF));
rejects(ValueError::class, fn() => $cli->setStdoutLimit(-1));
rejects(ValueError::class, fn() => $http->setMaxRedirects(21));
rejects(ValueError::class, fn() => $http->setRetryStatuses(['999']));

// Partial edits may temporarily violate cross-field rules; export checks them.
$cli->setRetryInitialDelay(JobU\Duration::seconds(10));
$cli->setRetryMaxDelay(JobU\Duration::seconds(1));
$http->setMaxRedirects(0);
$http->setFollowRedirects(true);
check($cli->getRetryMaxDelay()->getMilliseconds() === 1000, 'cross-field checks deferred');

$a = new JobU\JobAttributes;
$b = new JobU\JobAttributes;
$a->setRetryMaxAttempts(2);
$b->setRetryMaxAttempts(3);
check($a != $b && $a < $b && !in_array($a, [$b]), 'native comparison');
$b->setRetryMaxAttempts(2);
check($a == $b && $a !== $b && array_search($a, [$b]) === 0, 'value equality');
$a->setOutputCapture('none');
$b->setOutputCapture('none');
check($a == $b, 'ordered backing equality');

foreach ([$a, $cli, $http, $q] as $object) {
    rejects(Exception::class, fn() => serialize($object));
    rejects(Exception::class, fn() => $object->__construct());
    rejects(Error::class, function() use ($object) { $object->extra = 1; });
}
check(!method_exists($a, 'clear') && !method_exists($cli, 'clear'), 'no job removal');

// A live view retains its parent; the cached-view cycle is collectible afterwards.
$parent = new JobU\QueueDefaults;
$weak = WeakReference::create($parent);
$view = $parent->cli();
unset($parent);
gc_collect_cycles();
check($weak->get() !== null, 'view retains parent');
$view->setStdoutLimit(7);
$detached = clone $view;
unset($view);
gc_collect_cycles();
check($weak->get() === null && $detached->getStdoutLimit() === 7, 'clone drops parent and cycle collected');
rejects(TypeError::class, fn() => $a->setRetryMaxAttempts('3'));
rejects(TypeError::class, fn() => $a->set('retry.max_attempts', '3'));
$a->setRetryMultiplier(3);
check($a->getRetryMultiplier() === 3.0, 'typed integer widening');
rejects(ArgumentCountError::class, fn() => $a->getTimeout(1));
rejects(ArgumentCountError::class, fn() => $q->cli(1));
// A separately compiled weak-mode call uses Zend scalar coercion.
$weakSetter = eval('return function($value) { $a = new JobU\\JobAttributes; $a->setRetryMaxAttempts($value); return $a->getRetryMaxAttempts(); };');
check($weakSetter('3') === 3, 'weak scalar parsing');
echo "attributes ok\\n";
