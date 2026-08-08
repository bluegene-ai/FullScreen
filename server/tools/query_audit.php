<?php

declare(strict_types=1);

require_once __DIR__ . '/bootstrap.php';

$options = getopt('', ['action:', 'device-id:', 'limit:']);
$action = isset($options['action']) ? trim((string)$options['action']) : null;
$deviceId = isset($options['device-id']) ? trim((string)$options['device-id']) : null;
$limit = (int)($options['limit'] ?? 50);
if ($limit <= 0) {
    $limit = 50;
}
if ($limit > 500) {
    $limit = 500;
}

try {
    $svc = build_management_service();
    $entries = $svc->queryAudit($action, $deviceId, $limit);
    cli_json(['ok' => true, 'count' => count($entries), 'entries' => $entries]);
    exit(0);
} catch (Throwable $e) {
    cli_json(['ok' => false, 'error' => $e->getMessage()]);
    exit(1);
}
