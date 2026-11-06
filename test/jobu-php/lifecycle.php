<?php
function check(bool $condition, string $message): void
{
    if (!$condition) {
        throw new RuntimeException($message);
    }
}

check(!class_exists('JobU\\Client', false), 'public Client remains deferred');
check(JobUTestClient::live() === 0, 'request starts without native clients');
check(!isset($GLOBALS['previous_request']), 'request globals do not leak');

$first = new JobUTestClient(JOBU_TEST_SOCKET);
$second = new JobUTestClient(JOBU_TEST_SOCKET);
check($first->ping() && $second->ping(), 'two independent connected clients');
$first->pending();
$first->close();
$first->close();
check(!$first->ping() && $second->ping(), 'closing one leaves the other usable');
unset($first);

$weak = WeakReference::create($second);
$holder = new JobUTestHolder($second);
unset($second);
check($weak->get() !== null && $holder->client()->ping(), 'holder strongly retains client');
$holder->client()->close();
check(!$holder->client()->ping(), 'retained client honors explicit close');
unset($holder);
check($weak->get() === null && JobUTestClient::live() === 0, 'holder release frees client');

$cycle = new JobUTestClient(JOBU_TEST_SOCKET);
$holder = new JobUTestHolder($cycle);
$cycle->reference = $holder;
$weak = WeakReference::create($cycle);
unset($cycle, $holder);
gc_collect_cycles();
check($weak->get() === null && JobUTestClient::live() === 0, 'GC sees native holder reference');

foreach ([false, true] as $afterConnect) {
    try {
        new JobUTestClient($afterConnect ? JOBU_TEST_SOCKET : JOBU_TEST_SOCKET . '.missing', true);
        throw new LogicException('expected construction failure');
    } catch (Exception $error) {
        check(str_contains($error->getMessage(), 'Fixture'), 'partial construction diagnostic');
    }
    check(JobUTestClient::live() === 0, 'partial construction cleanup');
}

$client = new JobUTestClient(JOBU_TEST_SOCKET);
$holder = new JobUTestHolder($client);
foreach ([$client, $holder] as $object) {
    try { $copy = clone $object; throw new LogicException('clone accepted'); }
    catch (Error $error) {}
    try { serialize($object); throw new LogicException('serialization accepted'); }
    catch (Exception $error) { check(!$error instanceof LogicException, 'serialization rejected'); }
    $serialized = 'O:' . strlen(get_class($object)) . ':"' . get_class($object) . '":0:{}';
    try { unserialize($serialized); throw new LogicException('unserialization accepted'); }
    catch (Exception $error) { check(!$error instanceof LogicException, 'unserialization rejected'); }
}
unset($object, $holder, $client);
check(JobUTestClient::live() === 0, 'policy checks release clients');

// Keep a holder/client cycle and accepted native work alive until engine teardown.
// RSHUTDOWN must close sessions before Zend frees these native wrappers.
$client = new JobUTestClient(JOBU_TEST_SOCKET);
$holder = new JobUTestHolder($client);
$client->reference = $holder;
$client->pending();
$GLOBALS['previous_request'] = $holder;
unset($client, $holder);
return true;
