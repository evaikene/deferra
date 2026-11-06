<?php
/**
 * @generate-class-entries
 * @generate-legacy-arginfo 80100
 */

final class JobUTestClient
{
    public mixed $reference = null;
    public function __construct(string $socket, bool $fail = false) {}
    public function close(): void {}
    public function ping(): bool {}
    public function pending(): void {}
    public static function live(): int {}
}

final class JobUTestHolder
{
    public function __construct(JobUTestClient $client) {}
    public function client(): JobUTestClient {}
}

/** Test-only access to the production bridge; never registered by jobu.so. */
final class JobUTestBridge
{
    public static function ping(JobU\Client $client, ?JobU\Duration $timeout = null): bool {}
    public static function createQueue(JobU\Client $client, string $name, ?JobU\Duration $timeout = null, ?string $key = null): string {}
    public static function failure(string $code, string $category, bool $remote, bool $unknown = false, ?string $key = null, bool $pending = false): void {}
    public static function timeout(JobU\Client $client): int {}
}
