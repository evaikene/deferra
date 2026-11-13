<?php
declare(strict_types=1);

function pageCheck(bool $condition, string $message): void
{
    if (!$condition) {
        throw new RuntimeException($message);
    }
}

function pageThrows(string $class, callable $operation): Throwable
{
    try {
        $operation();
    } catch (Throwable $error) {
        pageCheck($error instanceof $class, 'expected ' . $class . ', got ' . $error::class);
        return $error;
    }
    throw new RuntimeException('expected ' . $class);
}

$client = new JobU\Client(PAGE_SOCKET);
$queue = $client->createQueue('page-primary');
$client->createQueue('page-second');
$client->createQueue('page-third');
for ($i = 0; $i < 3; ++$i) {
    $queue->createCliJob(new JobU\CliPayload('/bin/true'), new JobU\OnceSchedule(new DateTimeImmutable('+1 hour')), name: 'page-job-' . $i);
}
$emptyQueue = $client->createQueue('page-empty');
$page = JobUTestPages::create($client, 'jobs', 1, $emptyQueue->getId());
pageCheck(!$page->hasNext() && $page->items() === [], 'empty first result');
pageThrows(OutOfBoundsException::class, fn() => $page->next());

// Returned arrays are independent; their resource elements keep their identity.
$page = JobUTestPages::create($client, 'jobs', 2, $queue->getId());
pageCheck($page->hasNext() && $page->items() === [], 'before first');
pageThrows(ValueError::class, fn() => $page->next(JobU\Duration::milliseconds(0)));
pageCheck($page->items() === [], 'invalid override preserves position');
$page->next();
$first = $page->items();
pageCheck(count($first) === 2 && $first[0] instanceof JobU\Job, 'typed job items');
pageCheck($first[0] === $page->items()[0], 'stable item identity');
$copy = $first;
unset($copy[0]);
$copy[] = 'local';
pageCheck($page->items() === $first, 'array edits isolated');
$first[0]->setName('local unsaved name');
$page->next();
pageCheck(count($page->items()) === 1 && !$page->hasNext(), 'short terminal page');
pageCheck($first[0]->isDirty() && $first[0]->getName() === 'local unsaved name', 'older resources survive advancement');
$last = $page->items();
pageThrows(OutOfBoundsException::class, fn() => $page->next());
pageCheck($page->items() === $last, 'bounds failure preserves last page');
pageThrows(Error::class, fn() => clone $page);
pageThrows(Exception::class, fn() => serialize($page));
pageThrows(Exception::class, fn() => unserialize('O:9:"JobU\\Page":0:{}'));

// Page owns Client even before first next(). Cached operations survive close.
$retained = new JobU\Client(PAGE_SOCKET);
$weak = WeakReference::create($retained);
$buffered = JobUTestPages::create($retained, 'jobs', 1, $queue->getId());
$retained->close();
unset($retained);
pageCheck($weak->get() !== null && $buffered->hasNext(), 'retained closed Client');
$buffered->next();
$loaded = $buffered->items();
pageThrows(JobU\ConnectionException::class, fn() => $buffered->next());
pageCheck($buffered->items() === $loaded && $buffered->hasNext(), 'closed fetch preserves state');
unset($buffered, $loaded);
gc_collect_cycles();
pageCheck($weak->get() === null, 'Page and item ownership released');

$GLOBALS['pageClient'] = $client;
$GLOBALS['pageQueue'] = $queue;
$GLOBALS['pageQueueId'] = $queue->getId();
return true;
