<?php
declare(strict_types=1);

function check(bool $ok, string $message): void {
    if (!$ok) { throw new RuntimeException($message); }
}
function rejects(string $class, callable $operation): void {
    try { $operation(); } catch (Throwable $error) {
        check($error instanceof $class, 'Expected ' . $class . ', got ' . get_class($error));
        check(!str_contains($error->getMessage(), 'private-token'), 'Error disclosed submitted text');
        return;
    }
    throw new RuntimeException('Expected ' . $class);
}

$secret = new JobU\SecretReference('service.token');
$p = new JobU\CliPayload('"/opt/My Tools/worker" --verbose ""', arguments: ['two words', $secret]);
check($p->getExecutable() === '/opt/My Tools/worker', 'Quoted executable');
check($p->getArguments() == ['--verbose', '', 'two words', $secret], 'Appended literal arguments');
check($p->getEnvironment() === [] && $p->getWorkingDirectory() === '/' && $p->getExpectedExitCodes() === [0], 'Defaults');
check(!method_exists($p, 'getCommand'), 'No lossy command getter');
$p->setWorkingDirectory('/tmp');
$p->setExpectedExitCodes([255, 0, 7]);
$p->setEnvironment('TOKEN', $secret);
$p->setEnvironment('EMPTY', '');
$p->unsetEnvironment('HOME');
$p->setEnvironment('PATH', '/usr/bin:/bin');
$p->clearEnvironment('PATH');
$env = $p->getEnvironment();
check($env['TOKEN'] == $secret && $env['EMPTY'] === '' && array_key_exists('HOME', $env) && $env['HOME'] === null && !array_key_exists('PATH', $env), 'Environment states');
$p->setCommand('/bin/echo $HOME *.txt | > ~ `id` $(id)', ['literal " quote']);
check($p->getArguments() === ['$HOME', '*.txt', '|', '>', '~', '`id`', '$(id)', 'literal " quote'], 'No shell evaluation');
check($p->getEnvironment() == $env && $p->getWorkingDirectory() === '/tmp' && $p->getExpectedExitCodes() === [0, 7, 255], 'setCommand preserves other fields');
$p->setExecutable('/bin/printf');
check($p->getArguments()[0] === '$HOME', 'setExecutable preserves argv');
$p->setArguments(['hello']);
$p->addArgument($secret);
check($p->getArguments() == ['hello', $secret], 'Per-field arguments');
$p->setCommand('/opt/My\ Tools/worker "a\\qb"');
check($p->getExecutable() === '/opt/My Tools/worker' && $p->getArguments() === ['a\qb'], 'Escapes');

// Snapshot comparison makes failed mutations prove whole-object atomicity.
$before = clone $p;
foreach (['', "''", '/bin/echo "private-token', '/bin/echo private-token\\', "/bin/echo private-token\0", "/bin/echo \xff"] as $bad) {
    rejects(ValueError::class, fn() => $p->setCommand($bad));
    check($p == $before, 'Failed command changed payload');
}
rejects(TypeError::class, fn() => $p->setCommand('/bin/echo changed', ['private-token', 1]));
check($p == $before, 'Failed appended argument changed command');
foreach (['relative/path', '', str_repeat('x', 4097), "private-token\0", "\xff"] as $bad) {
    rejects(ValueError::class, fn() => $p->setExecutable($bad));
}
foreach (['relative', '', str_repeat('/', 4097), "/private-token\0", "/\xff"] as $bad) {
    rejects(ValueError::class, fn() => $p->setWorkingDirectory($bad));
}
foreach (['', '1BAD', 'A=B', 'JOBU_JOB_ID', 'JOBU_RUN_ID', 'JOBU_ATTEMPT'] as $bad) {
    rejects(ValueError::class, fn() => $p->setEnvironment($bad, 'private-token'));
    rejects(ValueError::class, fn() => $p->unsetEnvironment($bad));
    rejects(ValueError::class, fn() => $p->clearEnvironment($bad));
}
foreach (['PATH', 'JOBU_CUSTOM'] as $name) {
    rejects(ValueError::class, fn() => $p->setEnvironment($name, $secret));
}
rejects(TypeError::class, fn() => $p->setEnvironment('A', null));
rejects(ValueError::class, fn() => $p->setEnvironment('A', "private-token\0"));
rejects(ValueError::class, fn() => $p->addArgument("private-token\xff"));
foreach ([[0, 0], [-1], [256], ['code' => 1]] as $bad) {
    rejects(ValueError::class, fn() => $p->setExpectedExitCodes($bad));
}
foreach ([['0'], [0.0], [false], [null]] as $bad) {
    rejects(TypeError::class, fn() => $p->setExpectedExitCodes($bad));
}
rejects(ValueError::class, fn() => $p->setArguments(['key' => 'value']));
rejects(ValueError::class, fn() => $p->setArguments(array_fill(0, 1025, 'x')));
check($p == $before, 'Failed field edits changed payload');

// Input references and returned arrays cannot mutate native storage.
$text = 'first';
$args = [&$text];
$p->setArguments($args);
$text = 'second';
$args[] = 'third';
$output = $p->getArguments();
$output[0] = 'changed';
$env['TOKEN'] = 'changed';
check($p->getArguments() === ['first'] && $p->getEnvironment()['TOKEN'] == $secret, 'Copy isolation');
$copy = clone $p;
check($copy == $p && $copy !== $p, 'Detached clone equality');
$copy->addArgument('different');
check($copy != $p && $p->getArguments() === ['first'], 'Clone mutation isolation');
$a = new JobU\CliPayload('/bin/echo');
$b = new JobU\CliPayload('/bin/echo');
$a->setEnvironment('B', '2'); $a->setEnvironment('A', '1');
$b->setEnvironment('A', '1'); $b->setEnvironment('B', '2');
$a->setExpectedExitCodes([2, 1]); $b->setExpectedExitCodes([1, 2]);
check($a == $b, 'Map and exit set comparison');
$a->setExpectedExitCodes([]);
check($a->getExpectedExitCodes() === [] && $a != $b, 'Empty exit set');
$a->setArguments(array_fill(0, 1024, ''));
rejects(ValueError::class, fn() => $a->addArgument('overflow'));
rejects(ValueError::class, fn() => $a->setCommand('/bin/echo x', array_fill(0, 1024, '')));
check(count($a->getArguments()) === 1024, 'Combined argv limit atomicity');
$c = new JobU\CliPayload('worker');
check($c->getEnvironment() === [], 'No inherited PATH during local edits');
for ($i = 0; $i < 256; ++$i) { $c->setEnvironment('E' . $i, ''); }
$c->setEnvironment('E0', 'updated');
rejects(ValueError::class, fn() => $c->unsetEnvironment('OVERFLOW'));
$c->clearEnvironment('E0'); $c->unsetEnvironment('E256');
check(count($c->getEnvironment()) === 256, 'Environment entry limit');
rejects(Exception::class, fn() => $p->__construct('/bin/echo'));
rejects(Error::class, function () use ($p) { $p->extra = 1; });
rejects(Exception::class, fn() => serialize($p));
rejects(Exception::class, fn() => unserialize('O:15:"JobU\\CliPayload":0:{}'));

$r = new ReflectionClass(JobU\CliPayload::class);
check($r->isFinal(), 'Final payload class');
check(count($r->getMethods()) === 15, 'Exact method surface');
foreach (['__construct', 'setCommand'] as $method) {
    $params = $r->getMethod($method)->getParameters();
    check($params[0]->getName() === 'command' && $params[1]->getName() === 'arguments' && $params[1]->getDefaultValue() === [], 'Named/default arguments');
}
require __DIR__ . "/cli-payload-coercion.php";
echo "CLI payload checks passed\n";
