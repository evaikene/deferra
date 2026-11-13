<?php
// Invoked in each isolated Embed request against a real daemon and recording proxy.
use JobU\Duration;
use JobU\QueueState;
use JobU\RecoveryPolicy;

function queueCheck(bool $ok, string $message): void {
    if (!$ok) throw new RuntimeException($message);
}
function queueThrows(string $class, callable $call): void {
    try { $call(); } catch (Throwable $error) {
        queueCheck($error instanceof $class, "expected $class, got " . get_class($error));
        return;
    }
    throw new RuntimeException("missing $class");
}

$c = new JobU\Client(QUEUE_SOCKET);
$d = new JobU\QueueDefaults();
$d->cli()->setTimeout(Duration::seconds(3));
$q = $c->createQueue(QUEUE_PREFIX . 'first', defaults: $d, historyRetention: Duration::seconds(0),
    runnableWaitWarning: Duration::milliseconds(0), recoveryPolicy: RecoveryPolicy::RetryInterrupted);
$GLOBALS['queueClient'] = $c;
$GLOBALS['queueResource'] = $q;
$id = $q->getId();
queueCheck($q->getRevision() === 1 && !$q->isDirty() && !$q->isDeleted(), 'initial state');
queueCheck($q->getState() === QueueState::Active && $q->getDeletedAt() === null, 'active fields');
queueCheck($q->getWeight() === 1 && $q->getConcurrencyLimit() === 1, 'default capacity');
queueCheck($q->getRecoveryPolicy() === RecoveryPolicy::RetryInterrupted, 'policy');
queueCheck($q->getHistoryRetention() == Duration::seconds(0), 'zero retention');
queueCheck($q->getRunnableWaitWarning() == Duration::milliseconds(0), 'zero warning');
queueCheck($q->getCreatedAt() instanceof DateTimeImmutable && $q->getUpdatedAt() >= $q->getCreatedAt(), 'dates');
queueCheck($q->getCreatedAt()->getTimezone()->getName() === 'UTC', 'UTC');
queueCheck($c->getQueue($q->getName())->getId() === $id, 'literal lookup');
queueCheck($c->getQueueById($id)->getId() === $id, 'id lookup');
$lookalike = $c->createQueue($id);
queueCheck($lookalike->getHistoryRetention() === null && $lookalike->getRunnableWaitWarning() == Duration::seconds(10), 'creation duration defaults');
queueCheck($c->getQueue($id)->getId() === $lookalike->getId(), 'UUID-looking literal name');
foreach (['id:literal', 'name:literal'] as $suffix) {
    $literal = $c->createQueue(QUEUE_PREFIX . $suffix);
    queueCheck($c->getQueue($literal->getName())->getId() === $literal->getId(), 'prefix literal');
}
queueThrows(ValueError::class, fn() => $c->getQueueById(strtoupper($id)));
queueThrows(ValueError::class, fn() => $c->getQueueById('not-a-uuid'));
$root = $q->defaults(); $cli = $root->cli(); $http = $root->http();
$d->cli()->setTimeout(Duration::seconds(9));
queueCheck($cli->getTimeout() == Duration::seconds(3), 'create copied defaults');
$copy = $q->getDefaults(); $copy->cli()->setTimeout(Duration::seconds(8));
queueCheck($cli->getTimeout() == Duration::seconds(3) && !$q->isDirty(), 'detached defaults');

// Local edits form one checked update; returning a field to baseline removes it.
$q->setWeight(4); $q->setWeight(1);
queueCheck(!$q->isDirty(), 'edit back');
$q->setName(QUEUE_PREFIX . 'renamed');
$q->setWeight(3); $q->setConcurrencyLimit(4);
$q->setRecoveryPolicy(RecoveryPolicy::FailInterrupted);
$q->setHistoryRetention(null); $q->setRunnableWaitWarning(Duration::milliseconds(7));
$q->setDefaults($copy);
foreach (['refresh', 'suspend', 'resume', 'delete'] as $method) {
    queueThrows(JobU\UnsavedChangesException::class, fn() => $q->$method());
}
$q->save();
queueCheck($q->getId() === $id && $q->getRevision() === 2 && !$q->isDirty(), 'checked save');
queueCheck($q->defaults() === $root && $root->cli() === $cli && $root->http() === $http, 'save identity');
queueCheck($q->getHistoryRetention() === null && $q->getWeight() === 3 && $q->getConcurrencyLimit() === 4, 'saved fields');
queueCheck($q->getRecoveryPolicy() === RecoveryPolicy::FailInterrupted && $q->getRunnableWaitWarning() == Duration::milliseconds(7), 'saved policy and warning');
$q->save();
queueThrows(JobU\NotFoundException::class, fn() => $c->getQueue(QUEUE_PREFIX . 'first'));
queueCheck($c->getQueueById($id)->getName() === QUEUE_PREFIX . 'renamed', 'rename identity');

foreach ([-1, 0, 4294967296] as $bad) {
    queueThrows(ValueError::class, fn() => $q->setWeight($bad));
    queueThrows(ValueError::class, fn() => $q->setConcurrencyLimit($bad));
}
foreach (['', str_repeat('a', 129), "bad\n", "bad\xFF", 'reserved-deleted#' . $id] as $bad) {
    queueThrows(ValueError::class, fn() => $q->setName($bad));
}
queueThrows(ValueError::class, fn() => $q->setHistoryRetention(Duration::milliseconds(1)));
queueThrows(ValueError::class, fn() => $q->setHistoryRetention(Duration::seconds(-1)));
queueThrows(ValueError::class, fn() => $q->setRunnableWaitWarning(Duration::milliseconds(-1)));
queueThrows(TypeError::class, fn() => $q->setRunnableWaitWarning(null));
queueThrows(TypeError::class, fn() => $q->setRecoveryPolicy(QueueState::Active));
queueCheck(!$q->isDirty(), 'invalid setters atomic');
$q->setHistoryRetention(Duration::seconds(30));
queueCheck($q->getHistoryRetention() == Duration::seconds(30), 'positive retention');
$q->discardChanges();
foreach (['', str_repeat('k', 129), "bad\xFF"] as $badKey) {
    queueThrows(ValueError::class, fn() => $c->createQueue(QUEUE_PREFIX . 'bad-key', idempotencyKey: $badKey));
}
queueThrows(ValueError::class, fn() => $c->createQueue(QUEUE_PREFIX . 'bad-weight', weight: 0));
queueThrows(TypeError::class, fn() => $c->createQueue(QUEUE_PREFIX . 'bad-defaults', defaults: new JobU\CliJobAttributes()));
queueThrows(ValueError::class, fn() => $c->createQueue(QUEUE_PREFIX . 'bad-time', historyRetention: Duration::milliseconds(1)));
queueThrows(ValueError::class, fn() => $c->getQueueById($id, timeout: Duration::milliseconds(0)));
queueThrows(ValueError::class, fn() => $q->refresh(timeout: Duration::milliseconds(0)));
queueCheck(!$q->isDirty(), 'invalid calls preserve state');
$q->setWeight(4294967295); $q->discardChanges();
$q->setDefaults(new JobU\QueueDefaults()); $q->save();
queueCheck(!$cli->has('job.timeout') && $q->defaults() === $root, 'clear defaults');
$cli->setTimeout(Duration::seconds(11)); $q->discardChanges();
queueCheck(!$cli->has('job.timeout') && !$q->isDirty(), 'discard in place');
$cli->setTimeout(Duration::seconds(12)); $q->save();
$root->clear('job.timeout'); $q->save();
queueCheck(!$cli->has('job.timeout'), 'clear one override');

$peer = $c->getQueueById($id);
$peer->setWeight(6); $peer->save();
$revision = $q->getRevision();
$q->setWeight(5);
queueThrows(JobU\RevisionConflictException::class, fn() => $q->save());
queueCheck($q->getWeight() === 5 && $q->getRevision() === $revision && $q->isDirty(), 'conflict preserves edits');
$q->discardChanges(); $q->refresh();
queueCheck($q->getWeight() === 6 && $q->defaults() === $root, 'refresh adopts');

// Coercion may edit another field; the outer setter must preserve that change.
$name = new class($q) {
    public function __construct(private JobU\Queue $queue) {}
    public function __toString(): string { $this->queue->setWeight(7); return QUEUE_PREFIX . 'coerced'; }
};
$q->setName($name);
queueCheck($q->getWeight() === 7 && $q->getName() === QUEUE_PREFIX . 'coerced', 'reentrant setter');
$q->discardChanges();
queueThrows(Error::class, fn() => clone $q);
queueThrows(Exception::class, fn() => serialize($q));
queueThrows(Error::class, fn() => new JobU\Queue());
queueThrows(ArgumentCountError::class, fn() => $q->getName(1));
queueThrows(ArgumentCountError::class, fn() => $q->setName());

$q->suspend();
queueCheck($q->getState() === QueueState::Suspended, 'idle suspend');
$q->resume(); queueCheck($q->getState() === QueueState::Active, 'resume');
queueThrows(JobU\Exception::class, fn() => $q->delete());
queueCheck(!$q->isDeleted(), 'failed delete');
return true;
