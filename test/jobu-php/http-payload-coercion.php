<?php
// This file deliberately uses weak typing so string conversion can execute PHP.
$p = new JobU\HttpPayload('https://example.test/');
$url = new class($p) {
    public function __construct(private JobU\HttpPayload $p) {}
    public function __toString(): string {
        $this->p->addHeader('X-Nested', 'retained');
        return 'https://coerced.test/';
    }
};
$p->setUrl($url);
check($p->getUrl() === 'https://coerced.test/' && count($p->getHeaders()) === 1, 'URL coercion preserves nested edits');
$name = new class($p) {
    public function __construct(private JobU\HttpPayload $p) {}
    public function __toString(): string {
        $this->p->setBody('nested');
        return 'Bad Name';
    }
};
rejects(ValueError::class, fn() => $p->addHeader($name, 'value'));
check($p->getBody() === 'nested' && count($p->getHeaders()) === 1, 'Failed coercing setter preserves nested changes');
$method = new class($p) {
    public function __construct(private JobU\HttpPayload $p) {}
    public function __toString(): string {
        $this->p->clearBody();
        return 'HEAD';
    }
};
$p->setMethod($method);
check($p->getMethod() === 'HEAD' && $p->getBody() === null, 'Method validates current body');
