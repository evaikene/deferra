<?php
// Deliberately weak typing: a string parameter can execute __toString().
// This file is included by cli-payload.php after its assertion helpers.

function payloadString(callable $convert): Stringable {
    return new class($convert) {
        public function __construct(private $convert) {}
        public function __toString(): string { return ($this->convert)(); }
    };
}

$p = new JobU\CliPayload('/bin/echo old');
$p->setCommand(payloadString(function () use ($p): string {
    $p->setWorkingDirectory('/tmp');
    $p->setExpectedExitCodes([7]);
    $p->setEnvironment('KEEP', 'yes');
    return '/bin/printf new';
}));
check($p->getExecutable() === '/bin/printf' && $p->getArguments() === ['new'], 'Coerced command pair');
check($p->getWorkingDirectory() === '/tmp' && $p->getExpectedExitCodes() === [7] && $p->getEnvironment() === ['KEEP' => 'yes'], 'Command preserves nested field edits');

$p->setExecutable(payloadString(function () use ($p): string {
    $p->setArguments(['nested']);
    return '/bin/echo';
}));
check($p->getExecutable() === '/bin/echo' && $p->getArguments() === ['nested'], 'Executable preserves nested argv');
$p->setWorkingDirectory(payloadString(function () use ($p): string {
    $p->setCommand('/bin/printf nested');
    return '/var/tmp';
}));
check($p->getWorkingDirectory() === '/var/tmp' && $p->getExecutable() === '/bin/printf' && $p->getArguments() === ['nested'], 'Directory preserves nested command');

foreach (['setEnvironment', 'unsetEnvironment', 'clearEnvironment'] as $method) {
    $p = new JobU\CliPayload('/bin/echo');
    $p->setEnvironment('TARGET', 'old');
    $name = payloadString(function () use ($p): string {
        $p->setEnvironment('KEEP', 'nested');
        $p->setWorkingDirectory('/tmp');
        return 'TARGET';
    });
    if ($method === 'setEnvironment') {
        $p->$method($name, 'new');
    } else {
        $p->$method($name);
    }
    $env = $p->getEnvironment();
    check($env['KEEP'] === 'nested' && $p->getWorkingDirectory() === '/tmp', $method . ' preserves nested edits');
    if ($method === 'clearEnvironment') {
        check(!array_key_exists('TARGET', $env), 'Clear owns one entry');
    } else {
        check($env['TARGET'] === ($method === 'setEnvironment' ? 'new' : null), 'Set/unset owns one entry');
    }
}

// A rejected outer edit must not roll back a successful setter invoked by PHP
// coercion. It also must not partially change either half of the command pair.
$p = new JobU\CliPayload('/bin/echo old');
rejects(TypeError::class, function () use ($p): void {
    $p->setCommand(payloadString(function () use ($p): string {
        $p->setEnvironment('KEEP', 'yes');
        return '/bin/printf new';
    }), [123]);
});
check($p->getExecutable() === '/bin/echo' && $p->getArguments() === ['old'] && $p->getEnvironment() === ['KEEP' => 'yes'], 'Invalid argv leaves command atomic and nested edit intact');
rejects(ValueError::class, function () use ($p): void {
    $p->setCommand(payloadString(function () use ($p): string {
        $p->setExpectedExitCodes([9]);
        return '/bin/printf "';
    }));
});
check($p->getExecutable() === '/bin/echo' && $p->getArguments() === ['old'] && $p->getExpectedExitCodes() === [9], 'Invalid parsed command preserves nested edit');
rejects(RuntimeException::class, function () use ($p): void {
    $p->setWorkingDirectory(payloadString(function () use ($p): string {
        $p->setArguments(['survives']);
        throw new RuntimeException('Conversion failed');
    }));
});
check($p->getWorkingDirectory() === '/' && $p->getArguments() === ['survives'], 'Throwing coercion preserves independent setter');

// Limits must use the state after coercion, including a nested map insertion.
$p = new JobU\CliPayload('/bin/echo');
for ($i = 0; $i < 255; ++$i) { $p->setEnvironment('E' . $i, ''); }
rejects(ValueError::class, function () use ($p): void {
    $p->setEnvironment(payloadString(function () use ($p): string {
        $p->setEnvironment('LAST', 'nested');
        return 'OVERFLOW';
    }), 'outer');
});
check(count($p->getEnvironment()) === 256 && $p->getEnvironment()['LAST'] === 'nested' && !array_key_exists('OVERFLOW', $p->getEnvironment()), 'Environment limit uses current state');
