<?php
declare(strict_types=1);

// Disposable local endpoint for enqueue-http.php; not an application server.
header('Content-Type: application/json');
if ($_SERVER['REQUEST_METHOD'] !== 'POST' || ($_SERVER['HTTP_AUTHORIZATION'] ?? '') !== 'synthetic-demo-token') {
    http_response_code(403);
    echo '{"accepted":false}';
    return;
}

$event = json_decode(file_get_contents('php://input'), associative: true, flags: JSON_THROW_ON_ERROR);
http_response_code(201);
echo json_encode(['received' => $event], JSON_THROW_ON_ERROR);
