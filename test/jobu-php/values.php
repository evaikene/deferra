<?php
declare(strict_types=1);

// Closures keep this scenario reusable in every request of the Embed lifecycle test.
$check = static function (bool $condition, string $message): void {
    if (!$condition) {
        throw new RuntimeException($message);
    }
};
$reject = static function (string $type, callable $operation) use ($check): void {
    try {
        $operation();
    } catch (Throwable $error) {
        $check($error instanceof $type, "expected $type, got " . $error::class . ': ' . $error->getMessage());
        return;
    }
    throw new RuntimeException("expected $type");
};

foreach (['milliseconds' => 1, 'seconds' => 1000, 'minutes' => 60000, 'hours' => 3600000, 'days' => 86400000] as $factory => $scale) {
    foreach ([0, 1, -1, intdiv(PHP_INT_MAX, $scale), intdiv(PHP_INT_MIN, $scale)] as $value) {
        $check(JobU\Duration::$factory($value)->getMilliseconds() === $value * $scale, "$factory exact conversion");
    }
    if ($scale > 1) {
        $reject(ValueError::class, fn() => JobU\Duration::$factory(intdiv(PHP_INT_MAX, $scale) + 1));
        $reject(ValueError::class, fn() => JobU\Duration::$factory(intdiv(PHP_INT_MIN, $scale) - 1));
    }
    $reject(TypeError::class, fn() => JobU\Duration::$factory(1.5));
}

foreach (['service.token', 'a', str_repeat('a', 128), 'a_1.b2'] as $name) {
    $check((new JobU\SecretReference(name: $name))->getName() === $name, 'secret name preserved');
}
foreach (['', '.a', 'a.', 'a..b', 'A', 'a-b', '1a', 'a b', "a\0b", 'é', str_repeat('a', 129)] as $name) {
    $reject(ValueError::class, fn() => new JobU\SecretReference($name));
}
$reject(TypeError::class, fn() => new JobU\SecretReference([]));

$now = JobU\OnceSchedule::now();
$check($now->isNow() && $now->getAt() === null, 'symbolic now has no prematurely resolved date');
date_default_timezone_set('Pacific/Auckland');
foreach (['1969-12-31T23:59:59.999999Z', '1970-01-01T00:00:00.000001Z', '2030-10-27T03:30:00.123456+02:00'] as $text) {
    $date = new DateTime($text);
    $expected = $date->format('U.u');
    $once = new JobU\OnceSchedule(at: $date);
    $date->modify('+1 day');
    $result = $once->getAt();
    $check(!$once->isNow() && $result instanceof DateTimeImmutable, 'absolute once');
    $check($result->format('U.u') === $expected && $result->getTimezone()->getName() === 'UTC', 'exact UTC copied instant');
    $check($once->getAt() !== $result, 'date getters return fresh values');
}
$immutable = new DateTimeImmutable('2001-02-03T04:05:06.123456Z');
$check((new JobU\OnceSchedule($immutable))->getAt()->format('U.u') === $immutable->format('U.u'), 'immutable date input');
// Overrides must never substitute data for the native date or timezone state.
$date = new class('2001-02-03T04:05:06.123456Z') extends DateTimeImmutable {
    public function getTimestamp(): int { throw new RuntimeException('override called'); }
};
$check((new JobU\OnceSchedule($date))->getAt()->format('u') === '123456', 'native date extraction');
$badDate = new class extends DateTimeImmutable { public function __construct() {} };
$reject(ValueError::class, fn() => new JobU\OnceSchedule($badDate));
$reject(TypeError::class, fn() => new JobU\OnceSchedule('now'));
$reject(ValueError::class, fn() => new JobU\OnceSchedule(new DateTimeImmutable('@9223372036854775807')));

foreach ([new JobU\CronSchedule('@daily'), new JobU\CronSchedule('@daily', null)] as $cron) {
    $check($cron->getExpression() === '@daily' && $cron->getTimezone() === null, 'omitted timezone');
}
foreach (['UTC', 'Europe/Tallinn', 'America/New_York'] as $name) {
    $zone = new DateTimeZone($name);
    $cron = new JobU\CronSchedule(timezone: $zone, expression: '30 3 * * *');
    $zone->__construct('Pacific/Auckland');
    $result = $cron->getTimezone();
    $check($result->getName() === $name && $cron->getExpression() === '30 3 * * *', 'timezone copied');
    $result->__construct('Pacific/Auckland');
    $check($cron->getTimezone()->getName() === $name, 'timezone getter detached');
}
$zone = new class('UTC') extends DateTimeZone {
    public function getName(): string { throw new RuntimeException('override called'); }
};
$check((new JobU\CronSchedule('@weekly', $zone))->getTimezone()->getName() === 'UTC', 'native timezone extraction');
$badZone = new class extends DateTimeZone { public function __construct() {} };
$reject(ValueError::class, fn() => new JobU\CronSchedule('@daily', $badZone));
foreach (['+00:00', '+02:00', 'EST'] as $zone) {
    $reject(ValueError::class, fn() => new JobU\CronSchedule('@daily', new DateTimeZone($zone)));
}
foreach (['', "* *\0* * *", "\xff"] as $expression) {
    $reject(ValueError::class, fn() => new JobU\CronSchedule($expression));
}
$check((new JobU\CronSchedule('daemon validates this'))->getExpression() === 'daemon validates this', 'no local cron grammar');
$reject(TypeError::class, fn() => new JobU\CronSchedule('@daily', 'UTC'));

$secret = new JobU\SecretReference('service.token');
$once = new JobU\OnceSchedule($immutable);
$cron = new JobU\CronSchedule('@daily');
foreach ([JobU\Duration::seconds(1), $secret, $once, $now, $cron] as $value) {
    $reject(Error::class, static fn() => clone $value);
    $reject(Exception::class, static fn() => serialize($value));
    $reject(Error::class, static function () use ($value): void { $value->extra = 1; });
    $class = $value::class;
    $reject(Exception::class, static fn() => unserialize('O:' . strlen($class) . ':"' . $class . '":0:{}'));
    $check((new ReflectionClass($value))->isFinal(), 'concrete values final');
}
$reject(Exception::class, fn() => $secret->__construct('replacement'));
$reject(Exception::class, fn() => $once->__construct(new DateTimeImmutable('now')));
$reject(Exception::class, fn() => $cron->__construct('@weekly'));
$check($secret->getName() === 'service.token' && $cron->getExpression() === '@daily', 'reinitialization does not mutate');
$check((new ReflectionClass(JobU\Schedule::class))->isAbstract(), 'schedule abstract');

$methods = [
    [JobU\Duration::class, 'getMilliseconds', 'int', []],
    [JobU\SecretReference::class, '__construct', '', ['name' => 'string']],
    [JobU\SecretReference::class, 'getName', 'string', []],
    [JobU\OnceSchedule::class, '__construct', '', ['at' => 'DateTimeInterface']],
    [JobU\OnceSchedule::class, 'now', 'JobU\\OnceSchedule', []],
    [JobU\OnceSchedule::class, 'isNow', 'bool', []],
    [JobU\OnceSchedule::class, 'getAt', '?DateTimeImmutable', []],
    [JobU\CronSchedule::class, '__construct', '', ['expression' => 'string', 'timezone' => '?DateTimeZone']],
    [JobU\CronSchedule::class, 'getExpression', 'string', []],
    [JobU\CronSchedule::class, 'getTimezone', '?DateTimeZone', []],
];
foreach ($methods as [$class, $name, $return, $parameters]) {
    $method = new ReflectionMethod($class, $name);
    $check($method->isPublic() && $method->isStatic() === ($name === 'now'), 'method visibility/static');
    $check((string) $method->getReturnType() === $return, 'return type');
    $actual = [];
    foreach ($method->getParameters() as $parameter) {
        $actual[$parameter->getName()] = (string) $parameter->getType();
    }
    $check($actual === $parameters, 'named parameters and types');
}
$timezoneParameter = (new ReflectionMethod(JobU\CronSchedule::class, '__construct'))->getParameters()[1];
$check($timezoneParameter->isDefaultValueAvailable() && $timezoneParameter->getDefaultValue() === null, 'timezone default');

// Native value comparison must distinguish contents while === keeps identity.
$equalPairs = [
    [JobU\Duration::seconds(1), JobU\Duration::milliseconds(1000)],
    [JobU\Duration::milliseconds(PHP_INT_MIN), JobU\Duration::milliseconds(PHP_INT_MIN)],
    [new JobU\SecretReference('service.token'), new JobU\SecretReference('service.token')],
    [JobU\OnceSchedule::now(), JobU\OnceSchedule::now()],
    [new JobU\OnceSchedule(new DateTimeImmutable('2000-01-01T00:00:00.123456Z')),
        new JobU\OnceSchedule(new DateTimeImmutable('2000-01-01T02:00:00.123456+02:00'))],
    [new JobU\CronSchedule('@daily'), new JobU\CronSchedule('@daily', null)],
    [new JobU\CronSchedule('@daily', new DateTimeZone('UTC')),
        new JobU\CronSchedule('@daily', new DateTimeZone('UTC'))],
];
foreach ($equalPairs as [$left, $right]) {
    $check($left == $right && $right == $left && !($left != $right), 'native equality is symmetric');
    $check(($left <=> $right) === 0 && ($right <=> $left) === 0, 'equal ordering');
    $check($left !== $right && $left === $left, 'strict comparison remains identity');
    $check(in_array($left, [$right]) && array_search($left, [$right]) === 0, 'loose search finds equal content');
    $check(!in_array($left, [$right], true), 'strict search keeps identity');
}

$orderedValues = [
    [JobU\Duration::milliseconds(PHP_INT_MIN), JobU\Duration::milliseconds(-1),
        JobU\Duration::milliseconds(0), JobU\Duration::milliseconds(1), JobU\Duration::milliseconds(PHP_INT_MAX)],
    [new JobU\SecretReference('db.password'), new JobU\SecretReference('service.token')],
    [JobU\OnceSchedule::now(), new JobU\OnceSchedule(new DateTimeImmutable('1969-12-31T23:59:59.999999Z')),
        new JobU\OnceSchedule(new DateTimeImmutable('1970-01-01T00:00:00Z')),
        new JobU\OnceSchedule(new DateTimeImmutable('1970-01-01T00:00:00.000001Z'))],
    [new JobU\CronSchedule('@daily'), new JobU\CronSchedule('@daily', new DateTimeZone('Europe/Tallinn')),
        new JobU\CronSchedule('@daily', new DateTimeZone('UTC')), new JobU\CronSchedule('@weekly')],
];
foreach ($orderedValues as $values) {
    foreach ($values as $index => $left) {
        foreach (array_slice($values, $index + 1) as $right) {
            $check($left != $right && $right != $left, 'different native values are unequal');
            $check($left < $right && $right > $left && !($right < $left) && !($left > $right), 'native ordering');
            $check(($left <=> $right) === -1 && ($right <=> $left) === 1, 'normalized symmetric ordering');
            $check(!in_array($left, [$right]) && array_search($left, [$right]) === false, 'loose search rejects different content');
        }
    }
    $reversed = array_reverse($values);
    sort($reversed);
    $check($reversed === $values, 'sorting uses native values');
}

$differentClasses = [JobU\Duration::seconds(1), new JobU\SecretReference('a'),
    JobU\OnceSchedule::now(), new JobU\CronSchedule('@daily'), new stdClass()];
foreach ($differentClasses as $index => $left) {
    foreach (array_slice($differentClasses, $index + 1) as $right) {
        $check($left != $right && $right != $left, 'different classes never compare as equal native values');
    }
}

return true;
