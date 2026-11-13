<?php
declare(strict_types=1);

function adminCheck(bool $condition, string $message): void
{
    if (!$condition) {
        throw new RuntimeException($message);
    }
}

function adminThrows(string $class, callable $call): Throwable
{
    try {
        $call();
    } catch (Throwable $error) {
        adminCheck($error instanceof $class, "expected $class, got " . get_class($error));
        return $error;
    }
    throw new RuntimeException("expected $class");
}

$socket = defined('ADMIN_SOCKET') ? ADMIN_SOCKET : $argv[1];
$c = new JobU\Client($socket);
// Leave every secret cleaned up so a second request can reuse the same daemon.
$empty = $c->listSecrets();
adminCheck(!$empty->hasNext() && $empty->items() === [], 'empty initial page');
adminThrows(OutOfBoundsException::class, fn() => $empty->next());
// Direct extension failures must redact the sensitive value in PHP 8.2+ traces.
ini_set('zend.exception_ignore_args', '0');
try {
    $c->setSecret('INVALID', 'trace-private-marker');
} catch (ValueError $error) {
    adminCheck(!str_contains((string)$error, 'trace-private-marker'), 'redacted exception trace');
    adminCheck($error->getTrace()[0]['args'][1] instanceof SensitiveParameterValue, 'sensitive trace value');
}
$marker = "stage53-private-marker\0\xff";
$a = $c->setSecret(name: 'admin.a', value: $marker);
adminCheck($a->getName() === 'admin.a', 'metadata name');
$created = $a->getCreatedAt();
$b = $c->setSecret('admin.a', 'rotated');
adminCheck($b->getCreatedAt() == $created && $b->getUpdatedAt() >= $a->getUpdatedAt(), 'rotation timestamps');
adminCheck($a !== $b && $a->getCreatedAt() !== $created, 'detached metadata and dates');
adminCheck($created->getTimezone()->getName() === 'UTC', 'metadata UTC');
$c->setSecret('admin.b', '');
$c->setSecret(str_repeat('z', 128), str_repeat("\0\xff", 32768));

$p = $c->listSecrets(limit: 1);
adminCheck($p->items() === [] && $p->hasNext(), 'prefetched before-first');
$p->next();
adminCheck($p->items()[0]->getName() === 'admin.a', 'ascending names');
$c->deleteSecret('admin.a');
$p->next();
adminCheck($p->items()[0]->getName() === 'admin.b', 'deleted exclusive boundary');
$p->next();
adminCheck($p->items()[0]->getName() === str_repeat('z', 128) && !$p->hasNext(), 'final page');
$c->deleteSecret(str_repeat('z', 128));
$c->deleteSecret('admin.b');
$missing = adminThrows(JobU\NotFoundException::class, fn() => $c->deleteSecret('admin.missing'));
adminCheck($missing->getErrorCode() === 'jobu.secret.not_found', 'missing secret code');

// References remain names even when the backing value exists or rotates.
$c->setSecret('admin.used', $marker);
$ref = new JobU\SecretReference('admin.used');
$payload = new JobU\CliPayload('/bin/true', [$ref]);
$payload->setEnvironment('TOKEN', $ref);
$q = $c->createQueue('admin-' . bin2hex(random_bytes(6)));
$job = $q->createCliJob($payload, new JobU\OnceSchedule(new DateTimeImmutable('2100-01-01Z')));
$inUse = adminThrows(JobU\Exception::class, fn() => $c->deleteSecret('admin.used'));
adminCheck($inUse->getErrorCode() === 'jobu.secret.in_use', 'definition reference protects secret');
adminCheck($job->getPayload()->getArguments()[0]->getName() === 'admin.used', 'symbolic argument');
adminCheck($job->getPayload()->getEnvironment()['TOKEN']->getName() === 'admin.used', 'symbolic environment');
$http = new JobU\HttpPayload('https://example.invalid/', 'POST');
$http->addHeader('Authorization', $ref);
$http->setBody($ref);
adminCheck($http->getHeaders()[0]->getValue()->getName() === 'admin.used' && $http->getBody()->getName() === 'admin.used', 'symbolic HTTP');
$job->suspend();
$job->delete();
$q->suspend();
$q->delete();
$c->deleteSecret('admin.used');
adminCheck(!method_exists($c, 'getSecret') && !method_exists($a, 'getValue'), 'no secret read API');
ob_start();
var_dump($a, $ref, $payload, $http, $c);
$dump = ob_get_clean();
adminCheck(!str_contains($dump, 'stage53-private-marker'), 'native debug output excludes bytes');
adminCheck(!str_contains($inUse->getMessage(), 'stage53-private-marker'), 'generated exception excludes bytes');

$utc = new JobU\CronSchedule('@daily', new DateTimeZone('UTC'));
$implicit = new JobU\CronSchedule('@daily');
$after = new DateTime('2030-01-01T00:00:00.123456+02:00');
$original = clone $after;
adminCheck($c->validateSchedule($utc) === null, 'validation returns void');
$times = $c->nextScheduleOccurrences(schedule: $utc, after: $after);
adminCheck(count($times) === 5 && $after == $original, 'default count, unchanged mutable date');
$previous = $after;
foreach ($times as $time) {
    adminCheck($time instanceof DateTimeImmutable && $time->getTimezone()->getName() === 'UTC', 'UTC immutable');
    adminCheck($time > $previous, 'strict occurrence order');
    $previous = $time;
}
adminCheck($times[0]->format('Y-m-d H:i:s.u') === '2030-01-01 00:00:00.000000', 'UTC conversion');
$exact = new DateTimeImmutable('2030-01-01T00:00:00Z');
adminCheck($c->nextScheduleOccurrences($utc, $exact, 1)[0]->format('Y-m-d') === '2030-01-02', 'strictly after exact occurrence');
adminCheck(count($c->nextScheduleOccurrences(new JobU\CronSchedule('@hourly'), $exact, 200)) === 200, 'maximum count');
foreach (['@hourly', '@daily', '@weekly', '0 9 * * FRI-MON'] as $expression) {
    $c->validateSchedule(new JobU\CronSchedule($expression));
}
foreach (['bad cron', '@monthly', str_repeat('*', 513)] as $expression) {
    $error = adminThrows(JobU\Exception::class, fn() => $c->validateSchedule(new JobU\CronSchedule($expression)));
    adminCheck($error->getErrorCode() === 'jobu.schedule.invalid_expression', 'remote grammar code');
}
$error = adminThrows(JobU\Exception::class, fn() => $c->nextScheduleOccurrences(new JobU\CronSchedule('0 0 31 FEB *'), $exact));
adminCheck($error->getErrorCode() === 'jobu.schedule.no_future_occurrence', 'impossible date');
$error = adminThrows(JobU\Exception::class, fn() => $c->nextScheduleOccurrences($utc, new DateTimeImmutable('2262-04-11Z')));
adminCheck($error->getErrorCode() === 'jobu.schedule.out_of_range', 'daemon occurrence range');

// The daemon fixture uses Europe/Tallinn, deliberately different from explicit UTC.
$default = $c->nextScheduleOccurrences($implicit, $exact, 1)[0];
adminCheck($default->format('Y-m-d H:i') === '2030-01-01 22:00', 'daemon timezone default');
adminCheck($implicit->getTimezone() === null && $utc->getTimezone()->getName() === 'UTC', 'schedule inputs unchanged');
foreach ([['30 3 31 MAR *', '2024-03-30Z', '2024-03-31 01:30'],
          ['30 3 27 OCT *', '2024-10-26Z', '2024-10-27 00:30']] as [$expression, $start, $expected]) {
    $cron = new JobU\CronSchedule($expression, new DateTimeZone('Europe/Tallinn'));
    $times = $c->nextScheduleOccurrences($cron, new DateTimeImmutable($start), 2);
    adminCheck($times[0]->format('Y-m-d H:i') === $expected, 'DST gap/overlap');
    adminCheck($times[1]->format('Y') === '2025', 'overlap occurs only once');
}

// A Page owns its Client; metadata owns neither Client nor secret bytes.
$owner = new JobU\Client($socket);
$owner->setSecret('admin.owner', 'x');
$weak = WeakReference::create($owner);
$page = $owner->listSecrets();
unset($owner);
adminCheck($weak->get() !== null, 'page retains client');
$page->next();
$metadata = $page->items()[0];
unset($page);
gc_collect_cycles();
adminCheck($weak->get() === null && $metadata->getName() === 'admin.owner', 'metadata detached');
$c->deleteSecret('admin.owner');
$c->close();
foreach ([fn() => $c->setSecret('admin.closed', 'x'), fn() => $c->listSecrets(),
          fn() => $c->deleteSecret('admin.closed'), fn() => $c->validateSchedule($utc),
          fn() => $c->nextScheduleOccurrences($utc, $exact)] as $call) {
    adminThrows(JobU\ConnectionException::class, $call);
}
return true;
