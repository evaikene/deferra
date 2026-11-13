<?php
declare(strict_types=1);

// The same scenarios run through the production CLI module and Embed harness.
if (!defined('LIST_SOCKET')) {
    define('LIST_SOCKET', $argv[1]);
    define('LIST_PREFIX', 'lists-cli:');
}

function listCheck(bool $condition, string $message): void
{
    if (!$condition) {
        throw new RuntimeException($message);
    }
}

function listThrows(string $class, callable $operation): Throwable
{
    try {
        $operation();
    } catch (Throwable $error) {
        listCheck($error instanceof $class, 'expected ' . $class . ', got ' . $error::class);
        return $error;
    }
    throw new RuntimeException('expected ' . $class);
}

/** Consume every page without assuming a short page is terminal. */
function listItems(JobU\Page $page): array
{
    $items = [];
    while ($page->hasNext()) {
        $page->next();
        array_push($items, ...$page->items());
    }
    return $items;
}

function listIds(JobU\Page $page): array
{
    return array_map(fn($item) => $item->getId(), listItems($page));
}

$client = new JobU\Client(LIST_SOCKET);
$queue = $client->createQueue(LIST_PREFIX . 'primary', historyRetention: JobU\Duration::seconds(0));
$empty = $client->createQueue(LIST_PREFIX . 'empty');
$schedule = new JobU\OnceSchedule(new DateTimeImmutable('+1 day'));
$jobs = [];
for ($i = 0; $i < 3; ++$i) {
    $jobs[] = $queue->createCliJob(new JobU\CliPayload('/bin/true'), $schedule, name: 'cli-' . $i);
}
$jobs[1]->suspend();
$http = $queue->createHttpJob(new JobU\HttpPayload('https://example.invalid/'), $schedule, name: 'http');
$deleted = $queue->createCliJob(new JobU\CliPayload('/bin/true'), $schedule, name: 'deleted');
$deleted->suspend();
$deleted->delete();

$ids = array_map(fn($job) => $job->getId(), [...$jobs, $http]);
sort($ids);
listCheck(listIds($client->listJobs(queueId: $queue->getId())) === $ids, 'raw default includes suspended jobs');
listCheck(listIds($client->listJobs(queueName: $queue->getName())) === $ids, 'literal name selection');
listCheck(listIds($queue->listJobs()) === $ids, 'queue-scoped selection');
listCheck(count(listItems($client->listJobs())) >= 4, 'unscoped default');
listCheck(listIds($queue->listJobs(state: JobU\JobState::Suspended, type: JobU\JobType::Cli)) === [$jobs[1]->getId()], 'state and runner intersect');
listCheck(listIds($queue->listJobs(type: JobU\JobType::Http)) === [$http->getId()], 'HTTP filter');
listCheck(listIds($queue->listJobs(state: JobU\JobState::Deleted)) === [], 'state does not enable deleted visibility');
listCheck(listIds($queue->listJobs(state: JobU\JobState::Deleted, includeDeleted: true)) === [$deleted->getId()], 'deleted definition visibility');
listCheck(count(listItems($queue->listJobs(includeDeleted: true))) === 5, 'all retained definitions');

foreach ([1, 2, 4, 200] as $limit) {
    listCheck(listIds($queue->listJobs(limit: $limit)) === $ids, 'keyset ordering and complete coverage');
}
$page = $empty->listJobs();
listCheck(!$page->hasNext() && $page->items() === [], 'empty first result');
listThrows(OutOfBoundsException::class, fn() => $page->next());

// Listing reads persisted definitions and leaves the Queue's pending edits intact.
$queue->setName(LIST_PREFIX . 'unsaved');
$queue->defaults()->cli()->setTerminationGrace(JobU\Duration::seconds(3));
$defaults = $queue->defaults();
listCheck(listIds($queue->listJobs()) === $ids, 'dirty queue uses UUID');
listCheck($queue->isDirty() && $queue->getName() === LIST_PREFIX . 'unsaved' && $queue->defaults() === $defaults, 'dirty state and attachment unchanged');
$queue->discardChanges();
$queue->setName(LIST_PREFIX . 'renamed');
$queue->save();
listCheck(listIds($queue->listJobs()) === $ids, 'renamed queue uses UUID');
listThrows(JobU\NotFoundException::class, fn() => $client->listJobs(queueName: LIST_PREFIX . 'primary'));

// Names that resemble selectors or UUIDs are literal names, never CLI syntax.
$named = $client->createQueue($queue->getId());
listCheck(listIds($client->listJobs(queueName: $queue->getId())) === [], 'UUID-shaped name');
listCheck(listIds($client->listJobs(queueId: $queue->getId())) === $ids, 'UUID selection remains explicit');
$literal = $client->createQueue('id:' . $named->getId());
listCheck(listIds($client->listJobs(queueName: $literal->getName())) === [], 'id prefix is literal');
listThrows(JobU\NotFoundException::class, fn() => $client->listJobs(queueId: '019a0000-0000-7000-8000-000000000001'));

// A moved definition follows its current queue; retained item objects remain cached.
$jobs[1]->move($empty);
listCheck(listIds($empty->listJobs()) === [$jobs[1]->getId()], 'moved definition destination');
listCheck(!in_array($jobs[1]->getId(), listIds($queue->listJobs()), true), 'moved definition leaves source');
$jobs[1]->move($queue);

// A name selector is deliberately resolved again on continuation. The Queue
// resource keeps the original UUID after rename and is unaffected by name reuse.
$live = $client->createQueue(LIST_PREFIX . 'live');
for ($i = 0; $i < 2; ++$i) {
    $live->createCliJob(new JobU\CliPayload('/bin/true'), $schedule);
}
$byName = $client->listJobs(queueName: $live->getName(), limit: 1);
$byId = $live->listJobs(limit: 1);
$byName->next();
$byId->next();
$live->setName(LIST_PREFIX . 'live-renamed');
$live->save();
$client->createQueue(LIST_PREFIX . 'live');
$byName->next();
$byId->next();
listCheck($byName->items() === [] && !$byName->hasNext(), 'name continuation observes name reuse');
listCheck(count($byId->items()) === 1 && !$byId->hasNext(), 'UUID continuation keeps original queue');

// Full resource projections expose their normal attached configurations and guards.
$loaded = listItems($queue->listJobs(type: JobU\JobType::Http))[0];
listCheck($loaded->payload() instanceof JobU\HttpPayload && $loaded->attributes() instanceof JobU\HttpJobAttributes, 'typed HTTP configuration');
$loaded = listItems($queue->listJobs(type: JobU\JobType::Cli))[0];
listCheck($loaded->payload() instanceof JobU\CliPayload && $loaded->attributes() instanceof JobU\CliJobAttributes, 'typed CLI configuration');
$loaded->setName('saved listed job');
$loaded->save();
$loaded->refresh();
listCheck($loaded->getName() === 'saved listed job', 'listed job is an active resource');
$historicalJob = listItems($queue->listJobs(state: JobU\JobState::Deleted, includeDeleted: true))[0];
listCheck($historicalJob->isDeleted(), 'listed deleted job guard');
listThrows(JobU\NotFoundException::class, fn() => $historicalJob->setName('forbidden'));
listThrows(JobU\NotFoundException::class, fn() => $historicalJob->payload()->setExecutable('/bin/false'));

$archive = $client->createQueue(LIST_PREFIX . 'archive', historyRetention: JobU\Duration::seconds(0));
$archivedJob = $archive->createCliJob(new JobU\CliPayload('/bin/true'), $schedule);
$archive->suspend();
$archive->delete();
listThrows(JobU\NotFoundException::class, fn() => $archive->listJobs());
listThrows(JobU\NotFoundException::class, fn() => $client->listJobs(queueId: $archive->getId()));
listCheck(listIds($archive->listJobs(includeDeleted: true)) === [$archivedJob->getId()], 'locally deleted queue history');
listCheck(listIds($client->listJobs(queueName: LIST_PREFIX . 'archive', includeDeleted: true)) === [$archivedJob->getId()], 'historical name selector');
$historicalQueues = listItems($client->listQueues(state: JobU\QueueState::Deleted, includeDeleted: true, limit: 1));
$historicalQueue = array_values(array_filter($historicalQueues, fn($q) => $q->getId() === $archive->getId()))[0];
listCheck($historicalQueue->isDeleted() && $historicalQueue->getDeletedAt() !== null, 'historical queue snapshot');
listCheck(listIds($historicalQueue->listJobs(includeDeleted: true)) === [$archivedJob->getId()], 'historically loaded queue history');
listThrows(JobU\NotFoundException::class, fn() => $historicalQueue->defaults()->cli()->setTerminationGrace(JobU\Duration::seconds(3)));
listCheck(listIds($client->listQueues(state: JobU\QueueState::Deleted)) === [], 'queue Deleted state still needs flag');
listCheck(!in_array($archive->getId(), listIds($client->listQueues()), true), 'default queues exclude deleted');

// Reusing a historical name leaves native ambiguity visible instead of guessing.
$replacement = $client->createQueue(LIST_PREFIX . 'archive', historyRetention: JobU\Duration::seconds(0));
$replacement->createCliJob(new JobU\CliPayload('/bin/true'), $schedule);
$replacement->suspend();
$replacement->delete();
listThrows(JobU\Exception::class, fn() => $client->listJobs(queueName: LIST_PREFIX . 'archive', includeDeleted: true));
listCheck(listIds($client->listJobs(queueId: $archive->getId(), includeDeleted: true)) === [$archivedJob->getId()], 'UUID disambiguates retained names');

// Pages and retained items each own Client. Array edits do not replace Page's items.
$ownedClient = new JobU\Client(LIST_SOCKET);
$weakClient = WeakReference::create($ownedClient);
$ownedQueue = $ownedClient->getQueueById($queue->getId());
$page = $ownedQueue->listJobs(limit: 1);
$weakQueue = WeakReference::create($ownedQueue);
unset($ownedQueue, $ownedClient);
gc_collect_cycles();
listCheck($weakQueue->get() === null && $weakClient->get() !== null, 'Page retains Client independently of Queue');
$page->next();
$items = $page->items();
$copy = $items;
unset($copy[0]);
listCheck($page->items() === $items, 'array copy isolation');
$items[0]->setName('retained local edit');
$page->next();
listCheck($items[0]->isDirty(), 'old items survive advancement');
unset($page, $copy);
gc_collect_cycles();
listCheck($weakClient->get() !== null, 'retained Job owns Client');
unset($items);
gc_collect_cycles();
listCheck($weakClient->get() === null, 'Client released with last owner');

$closed = new JobU\Client(LIST_SOCKET);
$page = $closed->listJobs(queueId: $queue->getId(), limit: 1);
$closed->close();
$page->next();
$items = $page->items();
listThrows(JobU\ConnectionException::class, fn() => $page->next());
listCheck($page->items() === $items && $page->hasNext(), 'closed continuation preserves items');
listCheck($items[0]->getId() !== '', 'cached getters after close');
listThrows(JobU\ConnectionException::class, fn() => $closed->listQueues());
listThrows(JobU\ConnectionException::class, fn() => $closed->listJobs());

// Listed Queues retain Client too, including through attached default views.
$owner = new JobU\Client(LIST_SOCKET);
$weakOwner = WeakReference::create($owner);
$queues = $owner->listQueues(limit: 1);
$queues->next();
$view = $queues->items()[0]->defaults()->cli();
unset($queues, $owner);
gc_collect_cycles();
listCheck($weakOwner->get() !== null, 'listed Queue attachment retains Client');
unset($view);
gc_collect_cycles();
listCheck($weakOwner->get() === null, 'listed Queue cycle is collectable');

// Export only fixture roots needed by the proxy-level request/failure checks.
$GLOBALS['listClient'] = $client;
$GLOBALS['listQueue'] = $queue;
$GLOBALS['listQueueId'] = $queue->getId();
return true;
