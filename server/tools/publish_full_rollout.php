<?php

declare(strict_types=1);

require_once __DIR__ . '/bootstrap.php';

$options = getopt('', ['file:']);
$file = (string)($options['file'] ?? '');

if ($file === '') {
    fwrite(STDERR, "Usage: php tools/publish_full_rollout.php --file <publish-global.json>\n");
    exit(2);
}

try {
    $spec = read_json_file($file);
    if ((string)($spec['scope'] ?? '') !== 'global') {
        cli_json(['ok' => false, 'error' => 'full rollout requires scope=global']);
        exit(1);
    }

    $svc = build_management_service();
    $check = $svc->precheck($spec);
    if (!$check['ok']) {
        cli_json(['ok' => false, 'stage' => 'precheck', 'errors' => $check['errors']]);
        exit(1);
    }

    $published = $svc->publish($check['normalized']);
    cli_json(['ok' => true, 'mode' => 'full_rollout', 'published' => $published]);
    exit(0);
} catch (Throwable $e) {
    cli_json(['ok' => false, 'error' => $e->getMessage()]);
    exit(1);
}
