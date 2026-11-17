<?php
declare(strict_types=1);
require __DIR__ . '/common.php';

$client = exampleClient($argv, 1, 'enqueue-http.php SOCKET TEST_HTTP_URL');
try {
    // Use only a disposable HTTP endpoint. This deliberately synthetic value
    // demonstrates a reference; real credentials should never be pasted here.
    $secretName = 'example.token_' . bin2hex(random_bytes(6));
    $client->setSecret($secretName, 'synthetic-demo-token');

    $payload = new JobU\HttpPayload($argv[2], method: 'POST');
    $payload->addHeader('Authorization', new JobU\SecretReference($secretName));
    $payload->addHeader('X-Tag', 'first');
    $payload->addHeader('X-Tag', 'second');
    $payload->setJsonBody(['event' => 'php-example']);

    $attributes = new JobU\HttpJobAttributes();
    $attributes->setTimeout(JobU\Duration::seconds(10));
    $attributes->setOutputCapture('always');
    $queue = $client->createQueue(exampleName('http'));
    $job = $queue->createHttpJob(
        $payload, JobU\OnceSchedule::now(), name: 'post-event', attributes: $attributes,
    );
    printExampleJob($queue, $job);
    echo 'Secret name: ', $secretName, "\n";
    echo "Only the symbolic reference is stored in the payload.\n";
} finally {
    $client->close();
}
