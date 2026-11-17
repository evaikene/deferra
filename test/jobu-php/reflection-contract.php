<?php
declare(strict_types=1);

// Reflect the same declarations in separate processes: loading the stub beside
// the extension would redeclare its classes. No stub parser or generator is used.
$stub = $argv[1] ?? null;
if ($stub !== null) {
    require $stub;
}

function contractType(?ReflectionType $type): ?string
{
    if ($type instanceof ReflectionUnionType || $type instanceof ReflectionIntersectionType) {
        $parts = array_map(contractType(...), $type->getTypes());
        sort($parts);
        return implode($type instanceof ReflectionUnionType ? '|' : '&', $parts);
    }
    return $type === null ? null : (string) $type;
}

function contractValue(mixed $value): mixed
{
    return $value instanceof UnitEnum ? [$value::class, $value->name] : $value;
}

function contractAttributes(Reflector $reflection): array
{
    $attributes = [];
    foreach ($reflection->getAttributes() as $attribute) {
        $attributes[$attribute->getName()] = $attribute->getArguments();
    }
    ksort($attributes);
    return $attributes;
}

$classes = [];
foreach (get_declared_classes() as $name) {
    if (!str_starts_with($name, 'JobU\\')) {
        continue;
    }
    $class = new ReflectionClass($name);
    if ($stub === null && (!$class->isInternal() || $class->getExtensionName() !== 'jobu')) {
        throw new RuntimeException("$name is not owned by jobu");
    }
    $methods = [];
    foreach ($class->getMethods() as $method) {
        if ($method->getDeclaringClass()->getName() !== $name) {
            continue;
        }
        $parameters = [];
        foreach ($method->getParameters() as $parameter) {
            $parameters[] = [
                'name' => $parameter->getName(),
                'type' => contractType($parameter->getType()),
                'optional' => $parameter->isOptional(),
                'variadic' => $parameter->isVariadic(),
                'reference' => $parameter->isPassedByReference(),
                'hasDefault' => $parameter->isDefaultValueAvailable(),
                'default' => $parameter->isDefaultValueAvailable() ? contractValue($parameter->getDefaultValue()) : null,
                'attributes' => contractAttributes($parameter),
            ];
        }
        $methods[$method->getName()] = [
            'modifiers' => $method->getModifiers(),
            'reference' => $method->returnsReference(),
            'return' => contractType($method->getReturnType()),
            'parameters' => $parameters,
            'attributes' => contractAttributes($method),
        ];
    }
    ksort($methods);
    $cases = null;
    if ($class->isEnum()) {
        $enum = new ReflectionEnum($name);
        $cases = [];
        foreach ($enum->getCases() as $case) {
            $cases[$case->getName()] = $case->getBackingValue();
        }
    }
    $interfaces = $class->getInterfaceNames();
    sort($interfaces);
    $classes[$name] = [
        // zend_register_internal_enum does not set the userland final bit.
        // PHP still forbids enum inheritance; the driver checks that behavior
        // in isolated children before normalizing this one representation gap.
        'final' => $class->isEnum() || $class->isFinal(),
        'abstract' => $class->isAbstract(),
        'parent' => $class->getParentClass() ? $class->getParentClass()->getName() : null,
        'interfaces' => $interfaces,
        'enum' => $cases,
        'methods' => $methods,
    ];
}
ksort($classes);
echo json_encode($classes, JSON_PRETTY_PRINT | JSON_THROW_ON_ERROR), "\n";
