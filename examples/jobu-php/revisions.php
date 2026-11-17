<?php
declare(strict_types=1);
require __DIR__ . '/common.php';

$client = exampleClient($argv, 0, 'revisions.php SOCKET');
try {
    $queue = $client->createQueue(exampleName('revisions'));
    $first = $client->getQueueById($queue->getId());
    $second = $client->getQueueById($queue->getId());

    $first->setWeight(2);
    $first->save();
    $second->setWeight(3);
    try {
        $second->save();
    } catch (JobU\RevisionConflictException $error) {
        // Keep the proposal separately before discarding local edits. Refresh
        // alone is forbidden while dirty, and discard alone does not fetch.
        $proposal = $second->getWeight();
        $second->discardChanges();
        $second->refresh();
        echo 'Committed weight: ', $second->getWeight(), "\n";
        echo 'Unapplied proposal: ', $proposal, "\n";
        echo "Choose how to reconcile; this example does not overwrite the winner.\n";
    } catch (JobU\OutcomeUnknownException $error) {
        // Keep the dirty object untouched. An independent lookup can help the
        // application reconcile, but never treat uncertainty as a failed write.
        echo 'Inspect queue: ', $queue->getId(), "\n";
        throw $error;
    }
} finally {
    $client->close();
}
