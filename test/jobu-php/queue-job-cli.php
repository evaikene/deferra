<?php
// Run the existing queue/job behavior assertions through the production module.
define('QUEUE_SOCKET', $argv[1]);
define('QUEUE_PREFIX', 'queue-job-cli:');
foreach (['queue.php', 'job.php'] as $scenario) {
    if (!(include __DIR__ . '/' . $scenario)) {
        throw new RuntimeException("$scenario failed");
    }
}
