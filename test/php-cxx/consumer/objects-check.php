<?php

// Explicit checks remain enabled with php -n, regardless of zend.assertions.
set_error_handler(static function (int $severity, string $message, string $file, int $line): void {
    throw new ErrorException($message, 0, $severity, $file, $line);
});
$check = static function (bool $condition, string $scenario): void {
    if (!$condition) {
        throw new RuntimeException($scenario);
    }
};
$fails = static function (callable $action, string $type) use ($check): void {
    try {
        $action();
    } catch (Throwable $error) {
        $check(get_class($error) === $type, "Expected $type, got " . get_class($error));
        return;
    }
    throw new RuntimeException("Expected $type");
};

$baseline = PhpCxxNode::counts();
$node = new PhpCxxNode(7);
$check($node->number() === 7 && $node->reference() === null, 'native construction');
$check(PhpCxxNode::accepts($node), 'binding identity');
$check(!PhpCxxNode::accepts(new stdClass()), 'ordinary object rejected');
$locked = new PhpCxxLocked();
$check(!PhpCxxNode::accepts($locked), 'same native type with different policy rejected');
$fails(static fn() => clone $locked, Error::class);
$fails(static fn() => $node->__construct(), Exception::class);
$check($node->number() === 7, 'reinitialization preserves original native state');
$node->setNumber(11);
$check($node->number() === 11, 'native mutation');

// A PHP child that skips its parent constructor still owns a valid empty Zend object.
class PhpCxxUninitialized extends PhpCxxNode {
    public function __construct() {}
}
$empty = new PhpCxxUninitialized();
$check(!PhpCxxNode::accepts($empty), 'uninitialized native storage rejected');
$fails(static fn() => $empty->number(), Exception::class);
$fails(static fn() => clone $empty, Exception::class);
unset($empty);

$before = PhpCxxNode::counts();
$fails(static fn() => new PhpCxxNode(-2), ValueError::class);
$check(PhpCxxNode::counts() === $before, 'failure before native construction');
$fails(static fn() => new PhpCxxNode(1, new stdClass(), true), Exception::class);
$after = PhpCxxNode::counts();
$check($after['live'] === $before['live'] &&
    $after['constructed'] === $before['constructed'] + 1 &&
    $after['destroyed'] === $before['destroyed'] + 1, 'failure after construction destroys once');

class PhpCxxChild extends PhpCxxNode {
    public string $extra = 'child';
    public function __clone() { $this->setNumber($this->number() + 1); }
}
$child = new PhpCxxChild(20, $node);
$child->property = ['a' => 1];
$copy = clone $child;
$check($copy instanceof PhpCxxChild && $copy->extra === 'child', 'subclass layout and clone type');
$check($copy->number() === 21 && $child->number() === 20, '__clone sees independent initialized native state');
$check($copy->reference() === $node, 'fixture clone policy explicitly retains reference identity');
$copy->property['a'] = 2;
$check($child->property === ['a' => 1], 'standard property copy-on-write');
$child->setNumber(-1);
$before = PhpCxxNode::counts()['live'];
$fails(static fn() => clone $child, ValueError::class);
$check(PhpCxxNode::counts()['live'] === $before, 'native clone failure cleans target');
$child->setNumber(-3);
try {
    $unexpected = clone $child;
    throw new LogicException('Pending clone exception was lost');
} catch (Exception $error) {
    $check($error->getMessage() === 'original clone exception', 'pending clone exception preserved');
}
$check(PhpCxxNode::counts()['live'] === $before, 'pending exception cleans native clone temporary');

class PhpCxxThrowingClone extends PhpCxxNode {
    public function __clone() { throw new RuntimeException('clone failed'); }
}
$throwing = new PhpCxxThrowingClone();
$before = PhpCxxNode::counts()['live'];
$fails(static fn() => clone $throwing, RuntimeException::class);
$check(PhpCxxNode::counts()['live'] === $before, 'PHP clone failure cleans native state');

foreach ([$node, $child, $locked] as $object) {
    $fails(static fn() => serialize($object), Exception::class);
    $class = get_class($object);
    $serialized = 'O:' . strlen($class) . ':"' . $class . '":0:{}';
    $fails(static fn() => unserialize($serialized), Exception::class);
}
unset($object, $copy, $child, $throwing, $locked, $node);

// Failed clones must not run userland destructors before native state exists.
// Successfully initialized targets must retain normal PHP destructor behavior.
class PhpCxxCloneDestructor extends PhpCxxNode {
    public static array $destroyed = [];
    public static int $destructorCalls = 0;
    public bool $failPhpClone = false;

    public function __destruct() {
        ++self::$destructorCalls;
        self::$destroyed[] = $this->number();
    }

    public function __clone() {
        if ($this->failPhpClone) {
            throw new RuntimeException('PHP clone failed after native initialization');
        }
    }
}
class PhpCxxEmptyCloneDestructor extends PhpCxxCloneDestructor {
    public function __construct() {}
    public function initialize(): void { parent::__construct(); }
}

$checkCloneFailure = static function (PhpCxxNode $source, string $type, string $message) use ($check): void {
    $before = PhpCxxNode::counts();
    $destructorCalls = PhpCxxCloneDestructor::$destructorCalls;
    try {
        $unexpected = clone $source;
        throw new LogicException('Expected clone failure');
    } catch (Throwable $error) {
        $check(get_class($error) === $type, 'original clone exception class preserved');
        $check($error->getMessage() === $message && $error->getPrevious() === null,
            'original clone error preserved without destructor exception chaining');
    }
    $after = PhpCxxNode::counts();
    $check(PhpCxxCloneDestructor::$destructorCalls === $destructorCalls,
        'uninitialized failed clone never invokes PHP destructor');
    $check($after['live'] === $before['live'] &&
        $after['constructed'] - $before['constructed'] === $after['destroyed'] - $before['destroyed'],
        'failed clone and native temporaries are cleaned up');
};

$source = new PhpCxxCloneDestructor(-1);
$checkCloneFailure($source, ValueError::class, 'Fixture rejects negative native clones');
$source->setNumber(-3);
$checkCloneFailure($source, Exception::class, 'original clone exception');
unset($source);

$source = new PhpCxxEmptyCloneDestructor();
try {
    $checkCloneFailure($source, Exception::class, 'Native object is not initialized or is being destroyed');
} finally {
    // The source still owns its normal destructor; initialize it before releasing it.
    $source->initialize();
    unset($source);
}

PhpCxxCloneDestructor::$destroyed = [];
$source = new PhpCxxCloneDestructor(42);
$copy = clone $source;
unset($copy);
$check(PhpCxxCloneDestructor::$destroyed === [42], 'successful clone retains its PHP destructor');
$source->failPhpClone = true;
$before = PhpCxxNode::counts()['live'];
$fails(static fn() => clone $source, RuntimeException::class);
$check(PhpCxxCloneDestructor::$destroyed === [42, 42],
    'PHP clone-hook failure after native initialization retains normal destructor behavior');
$check(PhpCxxNode::counts()['live'] === $before, 'initialized failed clone releases native state');
unset($source, $checkCloneFailure);
$check(PhpCxxCloneDestructor::$destroyed === [42, 42, 42], 'source destructor still runs once');

// Both standard property representations must be traced alongside native edges.
foreach ([false, true] as $materialize) {
    $a = new PhpCxxNode();
    $b = new PhpCxxNode();
    $weakA = WeakReference::create($a);
    $weakB = WeakReference::create($b);
    $a->setReference($b);
    $a->property = $b; // Two owned references to the same object must both be reported.
    $b->setReference([$a]);
    if ($materialize) {
        $properties = get_object_vars($a);
        unset($properties);
    }
    unset($a, $b);
    gc_collect_cycles();
    $check($weakA->get() === null && $weakB->get() === null, 'native/array/property cycle collected');
}

$a = new PhpCxxNode();
$weak = WeakReference::create($a);
$a->setReference($a);
unset($a);
gc_collect_cycles();
$check($weak->get() === null, 'native self-cycle collected');

// Native ownership keeps a PHP object alive until the reference is replaced.
$a = new PhpCxxNode();
$referent = new stdClass();
$weak = WeakReference::create($referent);
$a->setReference($referent);
unset($referent);
$check($weak->get() !== null, 'native strong reference retains referent');
$a->setReference(null);
$check($weak->get() === null, 'replacement releases referent');
unset($a);

class PhpCxxCleanupProbe {
    public function __construct(public string $name) {}
    public function __destruct() { $GLOBALS['phpcxx_cleanup_order'][] = $this->name; }
}
$GLOBALS['phpcxx_cleanup_order'] = [];
$a = new PhpCxxNode(0, new PhpCxxCleanupProbe('native'));
$a->property = new PhpCxxCleanupProbe('property');
unset($a);
$check($GLOBALS['phpcxx_cleanup_order'] === ['native', 'property'], 'native cleanup precedes standard properties');
unset($GLOBALS['phpcxx_cleanup_order']);

class PhpCxxDestructor extends PhpCxxNode {
    public function __destruct() { $GLOBALS['phpcxx_destructor_number'] = $this->number(); }
}
$a = new PhpCxxDestructor(123);
unset($a);
$check($GLOBALS['phpcxx_destructor_number'] === 123, 'PHP destructor runs before native teardown');
unset($GLOBALS['phpcxx_destructor_number']);

class PhpCxxSerializationHooks extends PhpCxxNode {
    public function __serialize(): array { return []; }
    public function __unserialize(array $data): void {}
}
$a = new PhpCxxSerializationHooks();
$fails(static fn() => serialize($a), Exception::class);
$serialized = 'O:24:"PhpCxxSerializationHooks":0:{}';
$fails(static fn() => unserialize($serialized), Exception::class);
unset($a);

if (PHP_VERSION_ID >= 80500) {
    // Keep newer syntax out of the PHP 8.2 parser. A copied standard handler
    // would accidentally allocate an object without native storage here.
    $node = new PhpCxxNode();
    $fails(static function () use ($node) {
        return eval('return clone($node, ["property" => 1]);');
    }, Error::class);
    unset($node);
}

gc_collect_cycles();
$after = PhpCxxNode::counts();
$check($after['live'] === $baseline['live'] &&
    $after['constructed'] - $baseline['constructed'] === $after['destroyed'] - $baseline['destroyed'],
    'all completed constructions destroyed exactly once');

// Deliberately leave both reachable objects and a cycle for normal request shutdown.
// The Embed harness checks native counts after php_embed_shutdown(), not RSHUTDOWN.
$GLOBALS['phpcxx_shutdown_objects'] = [new PhpCxxNode(), new PhpCxxNode(), new PhpCxxLocked()];
$GLOBALS['phpcxx_shutdown_objects'][0]->setReference($GLOBALS['phpcxx_shutdown_objects'][1]);
$GLOBALS['phpcxx_shutdown_objects'][1]->property = $GLOBALS['phpcxx_shutdown_objects'][0];
restore_error_handler();
return true;
