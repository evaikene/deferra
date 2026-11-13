<?php
declare(strict_types=1);

function clientCheck(bool $condition, string $message): void
{
    if (!$condition) {
        throw new RuntimeException($message);
    }
}

function clientThrows(string $type, callable $call): Throwable
{
    try {
        $call();
    } catch (Throwable $error) {
        clientCheck($error instanceof $type, "Expected $type, got " . get_class($error));
        return $error;
    }
    throw new RuntimeException("Expected $type");
}

$clientClass = new ReflectionClass(JobU\Client::class);
clientCheck($clientClass->isFinal(), 'Client is final');
clientCheck(count($clientClass->getMethods()) === 8, 'connection, queue entry points and cross-queue lists');
$parameters = $clientClass->getConstructor()->getParameters();
clientCheck(count($parameters) === 2, 'constructor parameter count');
clientCheck($parameters[0]->getName() === 'socketPath' && (string)$parameters[0]->getType() === 'string', 'socket parameter');
clientCheck($parameters[1]->getName() === 'timeout' && (string)$parameters[1]->getType() === '?JobU\Duration', 'timeout parameter');
clientCheck($parameters[1]->getDefaultValue() === null, 'default timeout');
clientCheck((string)$clientClass->getMethod('close')->getReturnType() === 'void', 'close return type');
clientCheck((new ReflectionClass(JobU\Duration::class))->isFinal(), 'Duration is final');

foreach (['milliseconds' => 1, 'seconds' => 1000, 'minutes' => 60000, 'hours' => 3600000, 'days' => 86400000] as $factory => $scale) {
    foreach ([0, 1, -1, intdiv(PHP_INT_MAX, $scale), intdiv(PHP_INT_MIN, $scale)] as $value) {
        $duration = JobU\Duration::$factory($value);
        clientCheck($duration instanceof JobU\Duration, "$factory exact integer range");
        clientThrows(Exception::class, fn() => serialize($duration));
        clientThrows(Error::class, fn() => clone $duration);
    }
    if ($scale > 1) {
        clientThrows(ValueError::class, fn() => JobU\Duration::$factory(intdiv(PHP_INT_MAX, $scale) + 1));
        clientThrows(ValueError::class, fn() => JobU\Duration::$factory(intdiv(PHP_INT_MIN, $scale) - 1));
    }
    clientThrows(TypeError::class, fn() => JobU\Duration::$factory(1.5));
    clientThrows(ArgumentCountError::class, fn() => JobU\Duration::$factory());
    clientThrows(ArgumentCountError::class, fn() => JobU\Duration::$factory(1, 2));
}
clientThrows(Error::class, fn() => new JobU\Duration());
clientThrows(ValueError::class, fn() => new JobU\Client(''));
clientThrows(ValueError::class, fn() => new JobU\Client("socket\0suffix"));
clientThrows(TypeError::class, fn() => new JobU\Client([], null));
clientThrows(TypeError::class, fn() => new JobU\Client('/unused', 1));
clientThrows(ArgumentCountError::class, fn() => new JobU\Client());
clientThrows(ArgumentCountError::class, fn() => new JobU\Client('/unused', null, 1));
foreach ([0, -1, PHP_INT_MAX] as $timeout) {
    clientThrows(ValueError::class, fn() => new JobU\Client('/unused', JobU\Duration::milliseconds($timeout)));
}
$path = sys_get_temp_dir() . '/jobu-absent-' . bin2hex(random_bytes(12)) . '/private.sock';
$error = clientThrows(JobU\ConnectionException::class, fn() => new JobU\Client($path));
clientCheck($error->getErrorCode() === 'php.client.connection_failed', 'connection code');
clientCheck($error->getCategory() === 'unavailable', 'connection category');
clientCheck($error->getCode() === 0, 'standard integer exception code');
clientCheck(!str_contains($error->getMessage(), $path), 'socket path not disclosed');
if (isset($argv[1])) {
    $first = new JobU\Client($argv[1]);
    $second = new JobU\Client(socketPath: $argv[1], timeout: JobU\Duration::seconds(2));
    $first->close();
    $first->close();
    clientThrows(Exception::class, fn() => serialize($second));
    clientThrows(Error::class, fn() => clone $second);
    $second->close();
    // A successfully initialized live client is also released during CLI shutdown.
    $remaining = new JobU\Client($argv[1]);
    define('QUEUE_SOCKET', $argv[1]);
    define('QUEUE_PREFIX', 'cli:');
    require __DIR__ . '/queue.php';
}
echo "client contracts ok\n";
