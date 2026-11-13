<?php
declare(strict_types=1);

// Shared by production CLI and the repeated-request Embed/proxy fixture.
if (!defined('HISTORY_SOCKET')) {
    define('HISTORY_SOCKET', $argv[1]);
    define('HISTORY_PREFIX', 'history-cli:');
}
function historyCheck(bool $condition, string $message): void
{
    if (!$condition) {
        throw new RuntimeException($message);
    }
}
function historyThrows(string $class, callable $call): Throwable
{
    try {
        $call();
    } catch (Throwable $error) {
        historyCheck($error instanceof $class, 'expected ' . $class . ', got ' . $error::class);
        return $error;
    }
    throw new RuntimeException('expected ' . $class);
}
function historyItems(JobU\Page $page): array
{
    $items = [];
    while ($page->hasNext()) {
        $page->next();
        array_push($items, ...$page->items());
    }
    return $items;
}
/** Synchronize with observed durable state, bounded by a deadline; no fixed sleep. */
function historyWait(JobU\Run $run, JobU\RunState $state): void
{
    $deadline = hrtime(true) + 5_000_000_000;
    do {
        $run->refresh();
        if ($run->getState() === $state) {
            return;
        }
    } while (hrtime(true) < $deadline);
    throw new RuntimeException('run did not reach ' . $state->value . ': ' . $run->getState()->value);
}

$c = new JobU\Client(HISTORY_SOCKET);
$q = $c->createQueue(HISTORY_PREFIX . 'primary', historyRetention: JobU\Duration::seconds(0));
$future = new JobU\OnceSchedule(new DateTimeImmutable('2040-01-02T03:04:05.123456Z'));
$j = $q->createCliJob(new JobU\CliPayload('/bin/true'), $future, name: 'history');
$initial = historyItems($j->listRuns());
historyCheck(count($initial) === 1 && $initial[0] instanceof JobU\RunSummary, 'typed summary');
$s = $initial[0];
$r = $s->load();
foreach (['getId', 'getJobId', 'getQueueId', 'getJobRevision', 'getPriority', 'getOrigin', 'getType', 'getState',
          'isScheduleOwned', 'getPlannedAt', 'getRunnableAt', 'getStartedAt', 'getCompletedAt'] as $getter) {
    historyCheck($s->$getter() == $r->$getter(), $getter . ' complete summary fields');
}
historyCheck($s->getPlannedAt()->format('u') === '123456', 'exact UTC microseconds');
historyCheck($r->getResult() === null && $r->getPayload() instanceof JobU\CliPayload, 'full run details');
historyCheck($r->getAttributes() instanceof JobU\CliJobAttributes, 'typed run attributes');
historyCheck(!$r->listAttempts()->hasNext(), 'zero attempts');
historyCheck($j->getRun(id: $r->getId())->getId() === $r->getId(), 'explicit lookup');
historyCheck($q->getJob(id: $j->getId())->getId() === $j->getId(), 'queue job lookup');
// Manual occurrences provide real terminal results and retained attempt details.
$runs = [];
for ($i = 0; $i < 3; ++$i) {
    $runs[] = $j->runNow();
    historyWait($runs[$i], JobU\RunState::Succeeded);
}
$done = $runs[2];
historyCheck($done->getResult() instanceof JobU\CliExecutionResult && $done->getResult()->getExitCode() === 0, 'CLI result');
$attempts = historyItems($done->listAttempts(limit: 1));
historyCheck(count($attempts) === 1 && $attempts[0] instanceof JobU\AttemptSummary, 'attempt summaries');
$a = $attempts[0]->load();
foreach (['getRunId', 'getNumber', 'getDueAt', 'getStartedAt', 'getCompletedAt', 'getState', 'getOutcome'] as $getter) {
    historyCheck($a->$getter() == $attempts[0]->$getter(), $getter . ' attempt fields');
}
historyCheck($a->getState() === JobU\AttemptState::Completed && $a->getOutcome() === JobU\AttemptOutcome::Succeeded, 'attempt terminal fields');
historyCheck($a->getResult() instanceof JobU\CliExecutionResult, 'attempt typed result');
historyCheck($done->getAttempt(number: 1)->getResult()->getExitCode() === 0, 'direct attempt load');
$a->refresh();

$query = new JobU\RunQuery(jobId: $j->getId(), states: [JobU\RunState::Succeeded, JobU\RunState::Scheduled], limit: 1);
historyCheck(count(historyItems($c->listRuns($query))) === 4, 'state union and multiple cursor pages');
historyCheck(count(historyItems($j->listRuns(new JobU\RunQuery(origin: JobU\RunOrigin::Manual, type: JobU\JobType::Cli)))) === 3, 'combined filters');
historyCheck(count(historyItems($j->listRuns(new JobU\RunQuery(completed: new JobU\TimeRange(to: new DateTimeImmutable('2000-01-01Z')))))) === 0, 'empty range selection');
$j->setName('pending edit');
historyCheck(count(historyItems($j->listRuns())) === 4 && $j->isDirty() && $j->getName() === 'pending edit', 'dirty owner reads');
historyCheck($j->getRun($done->getId())->getResult()->getExitCode() === 0 && $j->isDirty(), 'dirty job lookup');
$j->discardChanges();
$q->setWeight(7);
$query = new JobU\RunQuery(jobId: $j->getId(), type: JobU\JobType::Cli, limit: 1);
historyCheck(count(historyItems($q->listRuns($query))) === 4 && $query->getQueueId() === null, 'queue scope copies query');
historyCheck($q->getJob($j->getId())->getId() === $j->getId() && $q->isDirty() && $q->getWeight() === 7, 'dirty queue lookup');
$q->discardChanges();
$other = $c->createQueue(HISTORY_PREFIX . 'other');
$j->suspend();
$j->move($other);
historyThrows(JobU\NotFoundException::class, fn() => $q->getJob($j->getId()));
historyCheck($other->getJob($j->getId())->getQueueId() === $other->getId(), 'lookup follows current membership');
historyCheck(count(historyItems($q->listRuns($query))) === 3, 'queue lists retained terminal runs');
historyCheck(count(historyItems($other->listRuns($query))) === 1, 'queue lists moved nonterminal run');
historyCheck(count(historyItems($j->listRuns(new JobU\RunQuery(queueId: $q->getId())))) === 3, 'terminal history retains captured queue');
historyCheck(count(historyItems($j->listRuns(new JobU\RunQuery(queueId: $other->getId())))) === 1, 'nonterminal run follows native move');
$r->refresh();
historyCheck($r->getQueueId() === $other->getId() && $r->getJobRevision() === $j->getRevision(), 'refresh adopts moved run');
historyCheck($r->cancel() === JobU\RunCancellationDisposition::Completed, 'pending cancellation completed');
historyCheck($r->getState() === JobU\RunState::Cancelled && $s->getState() === JobU\RunState::Scheduled, 'cache and immutable summary');
historyCheck($s->load()->getState() === JobU\RunState::Cancelled, 'load reads current detail');
historyThrows(JobU\Exception::class, fn() => $r->cancel());

$j->refresh();
$j->delete();
historyThrows(JobU\NotFoundException::class, fn() => $other->getJob($j->getId()));
$q->suspend(); $q->delete();
historyCheck(count(historyItems($q->listRuns($query))) === 3, 'deleted queue history remains readable');
historyCheck($j->getRun($done->getId())->getId() === $done->getId(), 'deleted job detail lookup');
historyCheck(count(historyItems($j->listRuns())) === 4, 'locally deleted owner history');
historyCheck($s->load()->getId() === $r->getId() && $a->getNumber() === 1, 'retained detail after owner deletion');
$a->refresh();

// Deleted definitions remain accessible through the existing explicit list filter.
$archive = $c->createQueue(HISTORY_PREFIX . 'archive', historyRetention: JobU\Duration::seconds(0));
$archivedJob = $archive->createCliJob(new JobU\CliPayload('/bin/true'), $future);
$archive->suspend();
$archive->delete();
historyThrows(JobU\NotFoundException::class, fn() => $archive->getJob($archivedJob->getId()));
historyCheck(historyItems($archive->listJobs(includeDeleted: true))[0]->isDeleted(), 'retained deleted child list');

// Active cancellation acknowledges the executor request, not terminal completion.
$slowAttrs = new JobU\CliJobAttributes();
$slowAttrs->setTerminationGrace(JobU\Duration::milliseconds(50));
$slow = $other->createCliJob(new JobU\CliPayload('/bin/sleep', ['20']), $future, attributes: $slowAttrs);
$active = $slow->runNow();
historyWait($active, JobU\RunState::Running);
historyThrows(JobU\NotFoundException::class, fn() => $j->getRun($active->getId()));
historyThrows(JobU\NotFoundException::class, fn() => $q->getJob($slow->getId()));
historyCheck($active->cancel() === JobU\RunCancellationDisposition::Requested, 'active cancellation requested');
historyCheck($active->getState() === JobU\RunState::Running, 'requested snapshot still running');
historyWait($active, JobU\RunState::Cancelled);

// An HTTP execution failure still has an HTTP result, never a guessed CLI result.
$attrs = new JobU\HttpJobAttributes();
$attrs->setRetryMaxAttempts(1);
$attrs->setTimeout(JobU\Duration::seconds(1));
$http = $other->createHttpJob(new JobU\HttpPayload('http://127.0.0.1:1/'), $future, attributes: $attrs);
$hr = $http->runNow();
historyWait($hr, JobU\RunState::Failed);
historyCheck($hr->getResult() instanceof JobU\HttpExecutionResult, 'HTTP run result');
historyCheck(historyItems($hr->listAttempts())[0]->load()->getResult() instanceof JobU\HttpExecutionResult, 'HTTP scoped result');
historyCheck($hr->getAttempt(1)->getResult() instanceof JobU\HttpExecutionResult, 'HTTP direct result');

// Retries establish ordering across multiple pages; the highest number is latest.
$retry = new JobU\CliJobAttributes();
$retry->setRetryMaxAttempts(3);
$retry->setRetryInitialDelay(JobU\Duration::milliseconds(1));
$retry->setRetryExitCodes(['1']);
$retryJob = $other->createCliJob(new JobU\CliPayload('/bin/false'), $future, attributes: $retry);
$retryRun = $retryJob->runNow();
historyWait($retryRun, JobU\RunState::Failed);
$retryAttempts = historyItems($retryRun->listAttempts(limit: 1));
historyCheck(array_map(fn($item) => $item->getNumber(), $retryAttempts) === [3, 2, 1], 'descending number across cursors');
historyCheck($retryAttempts[0]->load()->getNumber() === 3, 'latest attempt by number');

foreach ([$s, $r, $a, $attempts[0]] as $object) {
    historyThrows(Error::class, fn() => clone $object);
    historyThrows(Exception::class, fn() => serialize($object));
    historyThrows(Error::class, function () use ($object) { $object->extra = 1; });
}
$weak = WeakReference::create($c);
$GLOBALS['historyClient'] = $c;
$GLOBALS['historyJob'] = $slow;
$GLOBALS['historyRetainedJob'] = $j;
$GLOBALS['historyOldQueue'] = $q;
$GLOBALS['historyRun'] = $done;
$GLOBALS['historySummary'] = $s;
$GLOBALS['historyAttempt'] = $a;
$GLOBALS['historyHttpRun'] = $hr;
$GLOBALS['historyRetryRun'] = $retryRun;
$GLOBALS['historyQueue'] = $other;

$owner = new JobU\Client(HISTORY_SOCKET);
$parentQueue = $owner->getQueueById($other->getId());
$parentJob = $parentQueue->getJob($retryJob->getId());
$parentRun = $parentJob->getRun($retryRun->getId());
$heldAttempt = $parentRun->getAttempt(1);
$weakParents = array_map(fn($o) => WeakReference::create($o), [$parentQueue, $parentJob, $parentRun]);
$weakClient = WeakReference::create($owner);
unset($owner, $parentQueue, $parentJob, $parentRun); gc_collect_cycles();
historyCheck(array_map(fn($w) => $w->get(), $weakParents) === [null, null, null], 'child does not retain parent chain');
historyCheck($weakClient->get() !== null, 'attempt retains connection owner');
$heldAttempt->refresh();
$weakClient->get()->close();
historyCheck($heldAttempt->getResult()->getExitCode() === 1, 'attempt cached after close');
historyThrows(JobU\ConnectionException::class, fn() => $heldAttempt->refresh());
unset($heldAttempt); gc_collect_cycles();
historyCheck($weakClient->get() === null, 'attempt releases connection owner');

// A summary alone keeps its Client alive; explicit close leaves cached reads usable.
$owner = new JobU\Client(HISTORY_SOCKET);
$weakOwner = WeakReference::create($owner);
$held = historyItems($owner->listRuns(new JobU\RunQuery(jobId: $j->getId())))[0];
unset($owner); gc_collect_cycles();
historyCheck($weakOwner->get() !== null, 'summary retains Client');
$weakOwner->get()->close();
historyCheck($held->getId() !== '', 'cached summary after close');
historyThrows(JobU\ConnectionException::class, fn() => $held->load());
unset($held); gc_collect_cycles();
historyCheck($weakOwner->get() === null, 'summary releases Client');
return true;
