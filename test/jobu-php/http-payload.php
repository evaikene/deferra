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

$p = new JobU\HttpPayload(url: 'https://example.test/path', method: 'pOsT');
check($p->getMethod() === 'POST' && $p->getUrl() === 'https://example.test/path', 'Constructor');
check($p->getHeaders() === [] && $p->getBody() === null && $p->getExpectedStatuses() === ['200-299'], 'Defaults');
check(!method_exists($p, 'clearHeaders'), 'Singular clearHeader API');
$p->addHeader('X-Test', 'one');
$p->addHeader('X-Other', 'middle');
$p->addHeader('x-test', 'two', sensitive: true);
$headers = $p->getHeaders();
check(array_map(fn($h) => $h->getName(), $headers) === ['X-Test', 'X-Other', 'x-test'], 'Header order/spelling');
check(array_map(fn($h) => $h->getValue(), $headers) === ['one', 'middle', 'two'], 'Header values');
check(!$headers[0]->isSensitive() && $headers[2]->isSensitive(), 'Per-entry sensitivity');
$p->clearHeader('X-TEST');
check(count($p->getHeaders()) === 1 && $p->getHeaders()[0]->getName() === 'X-Other', 'Clear all matches');
check(count($headers) === 3 && $headers[0]->getValue() === 'one', 'Detached snapshots');
rejects(Error::class, fn() => clone $headers[0]);
rejects(Error::class, fn() => new JobU\HttpHeader());
rejects(Exception::class, fn() => serialize($headers[0]));

$secret = new JobU\SecretReference('service.token');
foreach (['Authorization', 'cookie', 'PROXY-AUTHORIZATION'] as $name) {
    $p->addHeader($name, 'private-token');
    check(array_slice($p->getHeaders(), -1)[0]->isSensitive(), 'Forced credential sensitivity');
}
$p->addHeader('X-Secret', $secret);
$h = array_slice($p->getHeaders(), -1)[0];
check($h->isSensitive() && $h->getValue() == $secret && $h->getValue() !== $secret, 'Symbolic fresh reference');
ob_start(); var_dump($p, $h); $debug = ob_get_clean();
check(!str_contains($debug, 'private-token'), 'Native contents are not exposed in debug output');

foreach (["", "nul\0bytes", "\xff\xfe", 'UTF-8: õ'] as $body) {
    $p->setBody($body);
    check($p->getBody() === $body, 'Binary-safe body');
}
$p->setBody($secret);
check($p->getBody() == $secret && $p->getBody() !== $secret, 'Symbolic body copy');
$before = clone $p;
rejects(ValueError::class, fn() => $p->setMethod('HEAD'));
check($p == $before, 'Method/body rejection atomic');
$p->clearBody();
$p->setMethod('head');
foreach (['', 'body', $secret] as $body) {
    rejects(ValueError::class, fn() => $p->setBody($body));
    check($p->getBody() === null, 'HEAD body rejected');
}
rejects(ValueError::class, fn() => $p->setJsonBody(null));
$p->setMethod('GET');
$p->setBody('GET body');
$p->setExpectedStatuses(['201', '400-499', '201']);
check($p->getExpectedStatuses() === ['201', '400-499', '201'], 'Status order and duplicates');

$before = clone $p;
foreach (['OPTIONS', 'CUSTOM', ' GET', '', "HEAD\0"] as $method) {
    rejects(ValueError::class, fn() => $p->setMethod($method));
    check($p == $before, 'Failed method atomic');
}
foreach (['ftp://host/', 'https://private-token@host/', 'https://host/#private-token', 'https://host/%xx', 'https://', "https://host/\0", 'https://[::1', 'https://host:bad'] as $url) {
    rejects(ValueError::class, fn() => $p->setUrl($url));
    check($p == $before, 'Failed URL atomic');
}
foreach (['Host', 'content-length', 'Transfer-Encoding', 'Connection', 'Proxy-Connection', 'TE', 'Trailer', 'Upgrade', 'X-JobU-Test', 'Idempotency-Key', '', 'Bad Name'] as $name) {
    rejects(ValueError::class, fn() => $p->addHeader($name, 'private-token'));
    check($p == $before, 'Failed header name atomic');
}
foreach (["private-token\r", "private-token\n", "private-token\0", "\xff"] as $value) {
    rejects(ValueError::class, fn() => $p->addHeader('X-Test', $value));
    check($p == $before, 'Failed header bytes atomic');
}
foreach ([[], ['600'], ['099'], ['201-200'], ['200 ', '201'], [1 => '200'], array_fill(0, 65, '200')] as $statuses) {
    rejects(ValueError::class, fn() => $p->setExpectedStatuses($statuses));
    check($p == $before, 'Failed statuses atomic');
}
rejects(TypeError::class, fn() => $p->setExpectedStatuses([200]));
rejects(TypeError::class, fn() => $p->setBody([]));
rejects(TypeError::class, fn() => $p->addHeader('X', new stdClass()));
rejects(ValueError::class, fn() => $p->setBody(str_repeat('x', 262145)));
check($p == $before, 'Invalid inputs preserve state');

$j = new JobU\HttpPayload('https://example.test/', 'POST');
foreach ([null, true, 1, 1.0, 'õ /', ['x' => [1, 2]], (object)['value' => 'text']] as $value) {
    $j->setJsonBody($value);
    check($j->getBody() === json_encode($value, JSON_THROW_ON_ERROR, 512), 'PHP JSON encoding parity');
    check(count($j->getHeaders()) === 1 && $j->getHeaders()[0]->getValue() === 'application/json', 'Single content type');
}
$j->clearHeader('CONTENT-TYPE');
$j->addHeader('content-TYPE', 'custom');
$j->addHeader('Content-Type', 'second');
$j->setJsonBody(['x']);
check(array_map(fn($h) => $h->getValue(), $j->getHeaders()) === ['custom', 'second'], 'Preserve existing content types');
$cycle = []; $cycle[] =& $cycle;
$deep = null; for ($i = 0; $i < 514; ++$i) { $deep = [$deep]; }
$stream = fopen('php://memory', 'r+');
foreach (["\xff", NAN, INF, $cycle, $deep, $stream] as $bad) {
    $before = clone $j;
    rejects(JsonException::class, fn() => $j->setJsonBody($bad));
    check($j == $before, 'JSON failure atomic');
}
fclose($stream);
// JSON callbacks own their edits even if the outer operation later fails.
$j->clearHeader('content-type');
$value = new class($j) implements JsonSerializable {
    public function __construct(private JobU\HttpPayload $payload) {}
    public function jsonSerialize(): mixed {
        $this->payload->setUrl('https://nested.test/');
        $this->payload->addHeader('Content-Type', 'callback');
        return ['ok' => true];
    }
};
$j->setJsonBody($value);
check($j->getUrl() === 'https://nested.test/' && $j->getBody() === '{"ok":true}', 'Nested edits retained');
check(count($j->getHeaders()) === 1 && $j->getHeaders()[0]->getValue() === 'callback', 'Check current content type');
$value = new class($j) implements JsonSerializable {
    public function __construct(private JobU\HttpPayload $payload) {}
    public function jsonSerialize(): mixed {
        $this->payload->clearBody();
        $this->payload->setMethod('HEAD');
        return ['blocked'];
    }
};
rejects(ValueError::class, fn() => $j->setJsonBody($value));
check($j->getBody() === null && $j->getMethod() === 'HEAD', 'Recheck current method after callback');
$exception = new LogicException('callback sentinel');
$value = new class($j, $exception) implements JsonSerializable {
    public function __construct(private JobU\HttpPayload $payload, private Throwable $error) {}
    public function jsonSerialize(): mixed {
        $this->payload->setUrl('https://error.test/');
        throw $this->error;
    }
};
try { $j->setJsonBody($value); throw new RuntimeException('Expected callback failure'); }
catch (LogicException $error) { check($error === $exception, 'Preserve exception identity'); }
check($j->getUrl() === 'https://error.test/' && $j->getBody() === null, 'Failed outer edit preserves nested edits');

$limit = new JobU\HttpPayload('https://example.test/');
for ($i = 0; $i < 124; ++$i) { $limit->addHeader('X', ''); }
$before = clone $limit;
rejects(ValueError::class, fn() => $limit->addHeader('X', ''));
rejects(ValueError::class, fn() => $limit->setJsonBody(1));
check($limit == $before, 'JSON body/content-type commit atomic at header capacity');
$bytes = new JobU\HttpPayload('https://example.test/');
$bytes->addHeader('X', str_repeat('x', 65536 - 183 - 1));
rejects(ValueError::class, fn() => $bytes->addHeader('Y', ''));

$a = new JobU\HttpPayload('https://example.test/');
$b = clone $a;
check($a == $b && $a !== $b, 'Detached clone value comparison');
$b->setBody('');
check($a != $b && !in_array($b, [$a]), 'Absent differs from empty');
$b->clearBody(); $b->addHeader('X', 'a'); $a->addHeader('X', 'a');
check($a == $b && $a->getHeaders()[0] == $b->getHeaders()[0], 'Header equality by native value');
$b->addHeader('x', 'b');
check($a != $b && $a->getHeaders()[0] != $b->getHeaders()[1], 'Header inequality');
rejects(Exception::class, fn() => serialize($a));
rejects(Error::class, function () use ($a) { $a->extra = true; });
rejects(Exception::class, fn() => $a->__construct('https://replacement.test/'));

// Equality must inspect every native field, including flags and list order.
$base = new JobU\HttpPayload('https://example.test/');
foreach ([
    fn($p) => $p->setUrl('https://other.test/'),
    fn($p) => $p->setMethod('POST'),
    fn($p) => $p->setExpectedStatuses(['201']),
    fn($p) => $p->setBody(new JobU\SecretReference('body.bytes')),
] as $change) {
    $copy = clone $base; $change($copy);
    check($copy != $base, 'Native field changes equality');
}
$left = clone $base; $right = clone $base;
$left->addHeader('X', 'same'); $right->addHeader('X', 'same', true);
check($left != $right && $left->getHeaders()[0] != $right->getHeaders()[0], 'Sensitivity affects equality');
$left->setExpectedStatuses(['200', '201']); $right->setExpectedStatuses(['201', '200']);
check($left->getExpectedStatuses() !== $right->getExpectedStatuses(), 'Statuses retain order');
json_decode('{invalid');
$errorBefore = json_last_error();
$base->setJsonBody(['ok']);
check(json_last_error() === $errorBefore, 'Exception-mode JSON preserves global last error');
rejects(JsonException::class, fn() => $base->setJsonBody(INF));
check(json_last_error() === $errorBefore, 'JSON failure preserves global last error');

$reflection = new ReflectionClass(JobU\HttpPayload::class);
check($reflection->isFinal() && count($reflection->getMethods()) === 14, 'Payload method surface');
check((string)$reflection->getMethod('getBody')->getReturnType() === 'JobU\\SecretReference|string|null', 'Body return type');
check(array_map(fn($p) => $p->getName(), $reflection->getMethod('addHeader')->getParameters()) === ['name', 'value', 'sensitive'], 'Named header arguments');
check((new ReflectionClass(JobU\HttpHeader::class))->isFinal(), 'Immutable final header');
require __DIR__ . '/http-payload-coercion.php';
echo "HTTP payload checks passed\n";
