<?php
declare(strict_types=1);
require __DIR__ . '/common.php';

$client = exampleClient($argv, 0, 'reconnect.php SOCKET');
try {
    $client->getSystemInfo();
    echo "Connected. Restart the test daemon on the same socket, then press Enter.\n";
    if (fgets(STDIN) === false) {
        throw new RuntimeException('Input ended before the reconnect step.');
    }

    // Reuse the same Client. This read may reconnect and repeat the handshake
    // within its deadline. There is no application loop replaying mutations.
    $client->getSystemInfo(timeout: JobU\Duration::seconds(5));
    echo "Read succeeded on the existing Client.\n";
} catch (JobU\ConnectionException | JobU\TimeoutException $error) {
    fwrite(STDERR, "Recovery failed. Create a new Client for a later operation.\n");
    throw $error;
} finally {
    $client->close();
}
