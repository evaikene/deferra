<?php
// Repeated by the real-daemon Embed harness; queues stay suspended to avoid races.
$c = new JobU\Client(QUEUE_SOCKET);
$q = $c->createQueue(QUEUE_PREFIX . 'jobs');
$q->suspend();
$when = new JobU\OnceSchedule(new DateTimeImmutable('+1 day'));
$p = new JobU\CliPayload('/bin/true');
$q->defaults()->cli()->setTimeout(JobU\Duration::seconds(17));
queueThrows(JobU\UnsavedChangesException::class, fn() => $q->createCliJob($p, $when));
$q->save();
$j = $q->createCliJob(payload: $p, schedule: $when, name: 'example', priority: -2);
queueCheck($j->getType() === JobU\JobType::Cli && $j->getQueueId() === $q->getId(), 'job identity');
queueCheck($j->getRevision() === 1 && $j->getName() === 'example' && $j->getPriority() === -2, 'job fields');
queueCheck($j->getAttributes()->getTimeout() == JobU\Duration::seconds(17), 'materialized defaults');
queueCheck($j->getCreatedAt()->getTimezone()->getName() === 'UTC' && $j->getDeletedAt() === null, 'job dates');
$p->setExecutable('/bin/false');
queueCheck($j->payload()->getExecutable() === '/bin/true', 'creation payload copied');
$a = $j->attributes(); $payload = $j->payload();
$payload->setExecutable('/bin/false'); $payload->setExecutable('/bin/true');
queueCheck(!$j->isDirty(), 'payload net edits');
$copy = $j->getPayload(); $copy->setExecutable('/bin/false');
queueCheck(!$j->isDirty(), 'detached payload');
$j->setPayload($copy);
queueCheck($j->payload() === $payload && $payload->getExecutable() === '/bin/false', 'payload replacement');
queueThrows(TypeError::class, fn() => $j->setPayload(new JobU\HttpPayload('http://localhost/')));
$j->discardChanges();
queueCheck($payload->getExecutable() === '/bin/true' && !$j->isDirty(), 'discard payload');
$partial = new JobU\JobAttributes(); $partial->setTimeout(JobU\Duration::seconds(21));
$j->setAttributes($partial);
queueCheck($a->getTimeout() == JobU\Duration::seconds(21) && $a->getRetryMaxAttempts() !== null, 'merge preserves omitted');
$j->setName(null); $j->setPriority(10);
foreach (['refresh', 'suspend', 'resume', 'delete', 'runNow'] as $method) {
    queueThrows(JobU\UnsavedChangesException::class, fn() => $j->$method());
}
$j->save();
queueCheck($j->getName() === null && $j->getPriority() === 10 && !$j->isDirty(), 'saved scalar fields');
queueCheck($j->attributes() === $a && $j->payload() === $payload, 'save child identity');
$stale = $q->getJob($j->getId());
$j->setPriority(11); $j->save();
$stale->setName('stale');
queueThrows(JobU\RevisionConflictException::class, fn() => $stale->save());
queueCheck($stale->isDirty() && $stale->getName() === 'stale', 'conflict preserves edits');
$stale->discardChanges(); $stale->refresh();
queueCheck($stale->getPriority() === 11, 'explicit refresh');
queueThrows(ValueError::class, fn() => $j->setSchedule(JobU\OnceSchedule::now()));
queueThrows(ValueError::class, fn() => $j->setPriority(2147483648));
$j->setSchedule(new JobU\CronSchedule('* * * * *'));
queueCheck($j->getSchedule()->getTimezone() === null, 'pending timezone omitted');
$j->discardChanges();
$j->suspend();
$run = $j->runNow(idempotencyKey: QUEUE_PREFIX . 'run');
queueCheck($run instanceof JobU\Run && $run->getJobId() === $j->getId(), 'manual run identity');
queueCheck(!$run->isScheduleOwned() && $run->getOrigin() === JobU\RunOrigin::Manual, 'manual origin');
queueCheck($run->getResult() === null && $run->getStartedAt() === null, 'pending run fields');
queueCheck($run->getAttributes() instanceof JobU\CliJobAttributes, 'typed run attributes');
$rp = $run->getPayload(); $rp->setExecutable('/bin/false');
queueCheck($run->getPayload()->getExecutable() === '/bin/true', 'run copy isolation');
$again = $j->runNow(idempotencyKey: QUEUE_PREFIX . 'run');
queueCheck($again->getId() === $run->getId(), 'manual replay');

// Separate suspended job has no manual barrier and can move/delete deterministically.
$moving = $q->createCliJob(new JobU\CliPayload('/bin/true'), $when);
$moving->suspend();
$destination = $c->createQueue(QUEUE_PREFIX . 'destination'); $destination->suspend();
$destination->setWeight(2);
queueThrows(JobU\UnsavedChangesException::class, fn() => $moving->move($destination));
$destination->discardChanges(); $moving->move($destination);
queueCheck($moving->getQueueId() === $destination->getId(), 'moved queue UUID');
$held = $moving->payload(); $revision = $moving->getRevision();
$moving->delete();
queueCheck($moving->isDeleted() && $moving->getRevision() === $revision, 'null deletion response');
queueThrows(JobU\NotFoundException::class, fn() => $held->setExecutable('/bin/false'));
queueThrows(JobU\NotFoundException::class, fn() => $moving->refresh());

$hp = new JobU\HttpPayload('http://localhost/example', 'POST');
$hp->addHeader('X-Repeat', 'one'); $hp->addHeader('X-Repeat', 'two'); $hp->setBody("\xff\0");
$http = $q->createHttpJob($hp, $when);
queueCheck($http->attributes() instanceof JobU\HttpJobAttributes && $http->getPayload()->getBody() === "\xff\0", 'HTTP binary snapshot');
queueCheck(count($http->payload()->getHeaders()) === 2, 'duplicate headers');
$http->payload()->setBody('changed'); $http->save(); $http->refresh();
queueCheck($http->getPayload()->getBody() === 'changed', 'HTTP save/refresh');

// Scalar coercion may edit a different field; preserve that nested edit.
$coercion = new class($http) {
    public function __construct(private JobU\Job $job) {}
    public function __toString(): string { $this->job->setPriority(42); return 'coerced'; }
};
$http->setName($coercion);
queueCheck($http->getPriority() === 42 && $http->getName() === 'coerced', 'job reentrant setter');
$http->discardChanges();
$reference = $when;
$http->setSchedule($reference);
$reference = JobU\OnceSchedule::now();
queueCheck(!$http->getSchedule()->isNow(), 'schedule input detached from variable');
$http->discardChanges();
queueThrows(Error::class, fn() => clone $http);
queueThrows(Exception::class, fn() => serialize($run));
$otherClient = new JobU\Client(QUEUE_SOCKET);
$otherQueue = $otherClient->getQueueById($q->getId());
queueThrows(ValueError::class, fn() => $http->move($otherQueue));
$otherClient->close();

$c->close();
$j->setName('local after close'); $j->discardChanges();
queueCheck($j->getPriority() === 11 && $run->getId() === $again->getId(), 'cached getters after close');
queueThrows(JobU\ConnectionException::class, fn() => $j->refresh());

// An attached child owns Job and Client; releasing it lets Zend collect the cycle.
$owners = (function () use ($when): array {
    $client = new JobU\Client(QUEUE_SOCKET);
    $queue = $client->getQueue(QUEUE_PREFIX . 'jobs');
    $job = $queue->createCliJob(new JobU\CliPayload('/bin/true'), $when);
    return [WeakReference::create($client), WeakReference::create($job), $job->payload()];
})();
gc_collect_cycles();
queueCheck($owners[0]->get() !== null && $owners[1]->get() !== null, 'job child retains owners');
unset($owners[2]); gc_collect_cycles();
queueCheck($owners[0]->get() === null && $owners[1]->get() === null, 'job cycle collected');
return true;
