<?php
declare(strict_types=1);

function check(bool $condition): void {
    if (!$condition) {
        throw new RuntimeException('php-cxx consumer assertion failed');
    }
}

function fails(string $class, Closure $call): void {
    try {
        $call();
    } catch (Throwable $error) {
        check($error instanceof $class);
        return;
    }
    throw new RuntimeException('Expected PHP exception was not raised');
}

check(phpcxx_probe('integer', PHP_INT_MAX) === PHP_INT_MAX);
check(phpcxx_probe('float', 42) === 42.0);
check(phpcxx_probe('boolean', false) === false);
check(phpcxx_probe('string', "a\0b") === "a\0b");
check(phpcxx_probe('array', [1, 2]) === 2);
check(phpcxx_probe('object', new DateTime()) === true);
check(phpcxx_probe('optional') === 'missing');
check(phpcxx_probe(value: null, operation: 'optional') === 'null');
check(phpcxx_probe(value: 19, operation: 'integer') === 19);
check(phpcxx_probe('range', 255) === 255);

fails(TypeError::class, fn() => phpcxx_probe('integer', '42'));
fails(TypeError::class, fn() => phpcxx_probe('boolean', 1));
fails(TypeError::class, fn() => phpcxx_probe('string', null));
fails(TypeError::class, fn() => phpcxx_probe('object', new stdClass()));
fails(ValueError::class, fn() => phpcxx_probe('range', -1));
fails(ArgumentCountError::class, fn() => phpcxx_probe('integer'));
fails(ArgumentCountError::class, fn() => phpcxx_probe('integer', 1, 2));

// Native state must win over overridden userland date accessors.
class ConsumerDate extends DateTime {
    public function getTimestamp(): int { throw new LogicException('override must not run'); }
}
date_default_timezone_set('Pacific/Auckland');
$date = phpcxx_probe('date', new ConsumerDate('1969-12-31 23:59:59.500001 UTC'));
check($date instanceof DateTimeImmutable);
check($date->format('Y-m-d H:i:s.u e') === '1969-12-31 23:59:59.500001 UTC');

// eval without a strict declaration deliberately provides a weak PHP caller.
check(eval('return phpcxx_probe("integer", "42");') === 42);
check(eval('return phpcxx_probe("boolean", "0");') === false);
check(eval('return phpcxx_probe("float", "1.5");') === 1.5);
check(eval('return phpcxx_probe("string", 42);') === '42');
echo "php-cxx conversion consumer passed\n";
