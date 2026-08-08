<?php

declare(strict_types=1);

require_once __DIR__ . '/bootstrap.php';

$options = getopt('', ['device-id:', 'password:', 'operator:', 'note:', 'delay-sec:', 'ttl-sec:']);
$deviceId = trim((string)($options['device-id'] ?? ''));
$password = (string)($options['password'] ?? '');
$operator = trim((string)($options['operator'] ?? 'ops'));
$note = isset($options['note']) ? trim((string)$options['note']) : '';
$delaySec = isset($options['delay-sec']) ? (int)$options['delay-sec'] : 0;
$ttlSec = isset($options['ttl-sec']) ? (int)$options['ttl-sec'] : 3600;

if ($deviceId === '' || $password === '' || $operator === '') {
    fwrite(STDERR, "Usage: php tools/enqueue_password_update.php --device-id <id> --password <plain> --operator <name> [--note <text>] [--delay-sec <n>] [--ttl-sec <n>]\n");
    exit(2);
}

$now = time();
$effectiveAt = $now + max(0, $delaySec);
$expireAt = $effectiveAt + max(60, $ttlSec);

try {
    $svc = build_management_service();
    $result = $svc->enqueuePasswordUpdate($deviceId, $password, $operator, $note, $effectiveAt, $expireAt);
    cli_json($result);
    exit(0);
} catch (Throwable $e) {
    cli_json(['ok' => false, 'error' => $e->getMessage()]);
    exit(1);
}
