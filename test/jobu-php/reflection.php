<?php
declare(strict_types=1);

function check(bool $condition, string $message): void
{
    if (!$condition) {
        throw new RuntimeException($message);
    }
}

$extension = new ReflectionExtension('jobu');
check($extension->getVersion() !== false && $extension->getVersion() !== '', 'extension version');
check($extension->getFunctions() === [], 'no premature RPC functions');

$enums = [
    'QueueState' => ['Active' => 'active', 'Suspending' => 'suspending', 'Suspended' => 'suspended', 'Deleted' => 'deleted'],
    'JobState' => ['Active' => 'active', 'Suspending' => 'suspending', 'Suspended' => 'suspended', 'Deleted' => 'deleted',
        'Succeeded' => 'succeeded', 'Failed' => 'failed', 'Cancelled' => 'cancelled'],
    'RunState' => ['Scheduled' => 'scheduled', 'Running' => 'running', 'RetryWait' => 'retry_wait',
        'Succeeded' => 'succeeded', 'Failed' => 'failed', 'Interrupted' => 'interrupted', 'Cancelled' => 'cancelled'],
    'AttemptState' => ['Pending' => 'pending', 'Running' => 'running', 'Completed' => 'completed'],
    'AttemptOutcome' => ['Succeeded' => 'succeeded', 'Failed' => 'failed', 'Interrupted' => 'interrupted', 'Cancelled' => 'cancelled'],
    'RetryMode' => ['Blocking' => 'blocking', 'Reschedule' => 'reschedule'],
    'RecoveryPolicy' => ['FailInterrupted' => 'fail_interrupted', 'RetryInterrupted' => 'retry_interrupted'],
    'JobType' => ['Cli' => 'cli', 'Http' => 'http'],
    'OutputChannel' => ['Stdout' => 'stdout', 'Stderr' => 'stderr', 'Body' => 'body', 'Headers' => 'headers'],
];

foreach ($enums as $name => $expected) {
    $class = 'JobU\\' . $name;
    $reflection = new ReflectionEnum($class);
    check($reflection->isInternal() && $reflection->getExtensionName() === 'jobu', "$class ownership");
    check($reflection->isBacked() && (string) $reflection->getBackingType() === 'string', "$class backing type");
    $actual = [];
    foreach ($class::cases() as $case) {
        $actual[$case->name] = $case->value;
        check($class::from($case->value) === $case, "$class from");
        check($class::tryFrom($case->value) === $case, "$class tryFrom");
        check(unserialize(serialize($case)) === $case, "$class native serialization");
    }
    check($actual === $expected, "$class case names and wire values");
    check($class::tryFrom('not-a-case') === null, "$class unknown case");
    try {
        $class::from('not-a-case');
        throw new RuntimeException("$class accepted unknown case");
    } catch (ValueError) {
    }
}

$exceptions = ['Exception', 'ConnectionException', 'TimeoutException', 'NotFoundException',
    'RevisionConflictException', 'InvalidCursorException', 'UnsavedChangesException',
    'OutcomeUnknownException', 'UnsupportedFeatureException'];
foreach ($exceptions as $name) {
    $reflection = new ReflectionClass('JobU\\' . $name);
    check($reflection->isInternal() && $reflection->getExtensionName() === 'jobu', "$name ownership");
    check($reflection->getParentClass()->getName() === ($name === 'Exception' ? 'Exception' : 'JobU\\Exception'), "$name parent");
    check($reflection->getConstructor()->getDeclaringClass()->getName() === 'Exception', "$name inherited constructor");
    foreach (['getErrorCode', 'getCategory'] as $methodName) {
        $method = $reflection->getMethod($methodName);
        check($method->isPublic() && !$method->isStatic(), "$name::$methodName visibility");
        check($method->getNumberOfParameters() === 0, "$name::$methodName parameters");
        check((string) $method->getReturnType() === '?string', "$name::$methodName return type");
    }
}

$expectedClasses = array_map(fn(string $name): string => 'JobU\\' . $name, [...array_keys($enums), ...$exceptions]);
$actualClasses = $extension->getClassNames();
sort($expectedClasses);
sort($actualClasses);
check($actualClasses === $expectedClasses, 'exact implemented class surface');
echo "reflection ok\n";
