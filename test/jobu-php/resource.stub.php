<?php
/**
 * @generate-class-entries
 * @generate-legacy-arginfo 80100
 */

/** Test-only resource adapter; never registered in jobu.so. */
final class JobUTestResource
{
    public mixed $reference = null;
    public function __construct(JobU\Client $client, string $id) {}
    public function name(): string {}
    public function revision(): int {}
    public function dirty(): bool {}
    public function defaults(): JobU\QueueDefaults {}
    public function notifications(): int {}
    public function setName(string $name): void {}
    public function save(?JobU\Duration $timeout = null): void {}
    public function refresh(?JobU\Duration $timeout = null): void {}
    public function discardChanges(): void {}
    public function lifecycle(): void {}
    public function markDeleted(): void {}
    public function rejectNextSnapshot(): void {}
}
