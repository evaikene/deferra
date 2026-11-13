<?php
declare(strict_types=1);

// Shared by the production module and repeated-request Embed/proxy fixture.
if (!defined('OUTPUT_SOCKET')) {
    define('OUTPUT_SOCKET', $argv[1]);
    define('OUTPUT_PREFIX', 'output-cli:');
    define('OUTPUT_PHP_BINARY', PHP_BINARY);
}
function outputCheck(bool $condition, string $message): void
{
    if (!$condition) {
        throw new RuntimeException($message);
    }
}
function outputThrows(string $class, callable $call): Throwable
{
    try {
        $call();
    } catch (Throwable $error) {
        outputCheck($error instanceof $class, 'expected ' . $class . ', got ' . $error::class);
        return $error;
    }
    throw new RuntimeException('expected ' . $class);
}
/** Synchronize with durable completion, without assuming a fixed execution time. */
function outputWait(JobU\Run $run, JobU\RunState $state): void
{
    $deadline = hrtime(true) + 5_000_000_000;
    do {
        $run->refresh();
        if ($run->getState() === $state) {
            return;
        }
    } while (hrtime(true) < $deadline);
    throw new RuntimeException('run did not reach ' . $state->value);
}
function outputRun(JobU\Queue $queue, string $script, ?JobU\CliJobAttributes $attributes = null): JobU\Run
{
    $attributes ??= new JobU\CliJobAttributes();
    $attributes->setOutputCapture('always');
    $job = $queue->createCliJob(new JobU\CliPayload('/bin/sh', ['-c', $script]),
        new JobU\OnceSchedule(new DateTimeImmutable('2040-01-01Z')), attributes: $attributes);
    $run = $job->runNow();
    outputWait($run, JobU\RunState::Succeeded);
    return $run;
}
$c = new JobU\Client(OUTPUT_SOCKET);
$q = $c->createQueue(OUTPUT_PREFIX . 'queue', historyRetention: JobU\Duration::seconds(0));
$future = new JobU\OnceSchedule(new DateTimeImmutable('2040-01-01Z'));
$emptyJob = $q->createCliJob(new JobU\CliPayload('/bin/true'), $future);
$page = $emptyJob->listRuns(); $page->next(); $zero = $page->items()[0]->load();
outputCheck($zero->getOutput() === null, 'no attempts');

$run = outputRun($q, "printf 'out'; printf 'err\\000é' >&2");
$attempt = $run->getAttempt(1);
$expected = "err\0é";
outputCheck($attempt->getOutput() === $expected && $run->getOutput() === $expected, 'stderr preference including binary NUL');
outputCheck($attempt->getOutput(maxBytes: 1, timeout: JobU\Duration::seconds(2)) === substr($expected, -1), 'tail can split UTF-8');
$chunk = $attempt->readOutput(channel: JobU\OutputChannel::Stderr);
outputCheck($chunk->getData() === $expected && $chunk->getOffset() === 0, 'default slice');
outputCheck($chunk->getStatus() === JobU\OutputStatus::Available && !$chunk->isCaptureLost(), 'available capture');
outputCheck($chunk->getRetainedBytes() === strlen($expected) && $chunk->getNextOffset() === null, 'metadata');
$first = $attempt->readOutput(JobU\OutputChannel::Stderr, length: 5);
$last = $attempt->readOutput(JobU\OutputChannel::Stderr, offset: $first->getNextOffset(), length: 1);
outputCheck($first->getData() . $last->getData() === $expected, 'concatenate decoded chunks across UTF-8 boundary');
outputCheck($last->getData() === "\xa9", 'base64 chunk returns raw bytes');
outputCheck($attempt->readOutput(JobU\OutputChannel::Stderr, offset: strlen($expected))->getData() === '', 'EOF offset');
outputThrows(JobU\Exception::class, fn() => $attempt->readOutput(JobU\OutputChannel::Stderr, offset: strlen($expected) + 1));

$stdout = outputRun($q, "printf 'stdout'");
outputCheck($stdout->getOutput() === 'stdout', 'empty stderr falls back to stdout');
$empty = outputRun($q, ':');
outputCheck($empty->getOutput() === '' && $empty->getAttempt(1)->getOutput() === '', 'captured empty');
$absent = $emptyJob->runNow(); outputWait($absent, JobU\RunState::Succeeded);
outputCheck($absent->getOutput() === null, 'on_error does not capture successful output');
outputCheck($absent->getAttempt(1)->readOutput(JobU\OutputChannel::Stdout)->getStatus() === JobU\OutputStatus::NotCaptured, 'not captured status');

$large = outputRun($q, 'head -c 70000 /dev/zero; printf END');
outputCheck($large->getOutput() === str_repeat("\0", 4093) . 'END', 'default 4096 byte tail');
outputCheck($large->getOutput(65536) === str_repeat("\0", 65533) . 'END', 'maximum tail');
outputCheck($large->getAttempt(1)->readOutput(JobU\OutputChannel::Stdout, length: 65536)->getBytesReturned() === 65536, 'maximum chunk');
$limited = new JobU\CliJobAttributes(); $limited->setStdoutLimit(8192);
$truncated = outputRun($q, 'head -c 20000 /dev/zero; printf END', $limited);
$t = $truncated->getAttempt(1)->readOutput(JobU\OutputChannel::Stdout);
outputCheck($t->isTruncated() && $t->getRetainedBytes() === 8192 && $t->getOmittedBytes() === 11811, 'truncated retained accounting');
outputCheck($truncated->getOutput() === substr($t->getData(), -4096), 'tail addresses retained bytes, excluding omitted middle');

$retry = new JobU\CliJobAttributes();
$retry->setRetryMaxAttempts(3); $retry->setRetryInitialDelay(JobU\Duration::milliseconds(1));
$retry->setRetryExitCodes(['1']); $retry->setOutputCapture('always');
$retryJob = $q->createCliJob(new JobU\CliPayload('/bin/sh', ['-c', "printf 'retry'; exit 1"]), $future, attributes: $retry);
$retried = $retryJob->runNow(); outputWait($retried, JobU\RunState::Failed);
outputCheck($retried->getOutput() === 'retry' && $retried->getAttempt(1)->getOutput() === 'retry', 'latest and explicit attempt output');

$slowAttributes = new JobU\CliJobAttributes(); $slowAttributes->setOutputCapture('always');
$slowAttributes->setTerminationGrace(JobU\Duration::milliseconds(10));
$slowJob = $q->createCliJob(new JobU\CliPayload('/bin/sleep', ['20']), $future, attributes: $slowAttributes);
$running = $slowJob->runNow(); outputWait($running, JobU\RunState::Running);
$runningAttempt = $running->getAttempt(1);
outputCheck($runningAttempt->readOutput(JobU\OutputChannel::Stdout)->getStatus() === JobU\OutputStatus::Pending, 'pending without live stream');
outputCheck($running->getOutput() === null && $runningAttempt->getOutput() === null, 'no wait for running work');
$running->cancel(); outputWait($running, JobU\RunState::Cancelled);
outputCheck($runningAttempt->getState() === JobU\AttemptState::Running, 'output leaves cache unchanged');

// A separate matching PHP CLI serves one real HTTP response. No Zend object
// crosses a thread, and the ephemeral port is published only after listening.
$serverCode = <<<'SERVER'
$s = stream_socket_server('tcp://127.0.0.1:0', $code, $error);
if (!$s) { exit(2); }
echo stream_socket_get_name($s, false), "\n"; flush();
$c = stream_socket_accept($s, 5);
if (!$c) { exit(3); }
stream_set_timeout($c, 5);
while (($line = fgets($c)) !== false && $line !== "\r\n") {}
$body = str_repeat('h', 5000) . "\0é";
$response = "HTTP/1.1 200 OK\r\nContent-Length: " . strlen($body) . "\r\nConnection: close\r\n\r\n" . $body;
while ($response !== '') { $n = fwrite($c, $response); if (!$n) { exit(4); } $response = substr($response, $n); }
fclose($c); fclose($s);
SERVER;
$server = proc_open([OUTPUT_PHP_BINARY, '-n', '-r', $serverCode], [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['pipe', 'w']], $pipes);
outputCheck(is_resource($server), 'start HTTP fixture');
try {
    stream_set_timeout($pipes[1], 5);
    $address = trim(fgets($pipes[1]));
    outputCheck($address !== '', 'HTTP fixture address');
    $httpAttributes = new JobU\HttpJobAttributes(); $httpAttributes->setOutputCapture('always');
    $httpJob = $q->createHttpJob(new JobU\HttpPayload('http://' . $address . '/output'), $future, attributes: $httpAttributes);
    $http = $httpJob->runNow(); outputWait($http, JobU\RunState::Succeeded);
    $httpAttempt = $http->getAttempt(1);
    outputCheck($http->getOutput() === str_repeat('h', 4093) . "\0é", 'HTTP body tail');
    outputCheck($httpAttempt->getOutput(1) === "\xa9", 'HTTP binary tail');
    outputCheck(str_contains($httpAttempt->readOutput(JobU\OutputChannel::Headers)->getData(), '200 OK'), 'explicit HTTP headers');
} finally {
    foreach ($pipes as $pipe) { fclose($pipe); }
    $status = proc_close($server);
}
outputCheck($status === 0, 'HTTP fixture completed');

// Retained output remains readable after the owning job and queue are deleted.
$q->suspend(); $q->delete();
outputCheck($run->getOutput() === $expected && $attempt->getOutput() === $expected, 'deleted owner history');
$GLOBALS['outputClient'] = $c;
$GLOBALS['outputRun'] = $run;
$GLOBALS['outputAttempt'] = $attempt;
$GLOBALS['outputZero'] = $zero;
$GLOBALS['outputRetry'] = $retried;
$GLOBALS['outputHttp'] = $httpAttempt;
$GLOBALS['outputEmpty'] = $empty->getAttempt(1);

$owner = new JobU\Client(OUTPUT_SOCKET);
$p = $owner->listRuns(new JobU\RunQuery(jobId: $run->getJobId(), states: [JobU\RunState::Succeeded])); $p->next();
$held = $p->items()[0]->load()->getAttempt(1);
$weak = WeakReference::create($owner);
unset($p, $owner); gc_collect_cycles();
outputCheck($held->getOutput() === $expected && $weak->get() !== null, 'attempt owns Client');
$weak->get()->close();
outputThrows(JobU\ConnectionException::class, fn() => $held->getOutput());
outputThrows(JobU\ConnectionException::class, fn() => $held->readOutput(JobU\OutputChannel::Stderr));
outputCheck($chunk->getData() === $expected, 'detached chunk survives closure');
unset($held); gc_collect_cycles();
outputCheck($weak->get() === null, 'attempt releases Client');
return true;
