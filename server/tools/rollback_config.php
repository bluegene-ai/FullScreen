<?php

declare(strict_types=1);

require_once __DIR__ . '/bootstrap.php';

$options = getopt('', ['scope:', 'scope-id::', 'revision:', 'operator:', 'note:']);
$scope = trim((string)($options['scope'] ?? ''));
$scopeId = trim((string)($options['scope-id'] ?? ''));
$revision = (int)($options['revision'] ?? 0);
$operator = isset($options['operator']) ? trim((string)$options['operator']) : 'ops';
$note = isset($options['note']) ? trim((string)$options['note']) : '';

if ($scope === '' || $revision <= 0) {
    fwrite(STDERR, "Usage: php tools/rollback_config.php --scope <global|group|device> [--scope-id <id>] --revision <n> [--operator <name>] [--note <text>]\n");
    exit(2);
}

if (($scope === 'group' || $scope === 'device') && $scopeId === '') {
    fwrite(STDERR, "scope-id is required for group/device rollback\n");
    exit(2);
}

if ($scope === 'global') {
    $scopeId = '';
}

try {
    $svc = build_management_service();
    $result = $svc->rollback($scope, $scopeId, $revision, $operator, $note);
    cli_json($result);
    exit($result['ok'] ? 0 : 1);
} catch (Throwable $e) {
    cli_json(['ok' => false, 'error' => $e->getMessage()]);
    exit(1);
}
