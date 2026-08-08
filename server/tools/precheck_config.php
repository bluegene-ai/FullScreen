<?php

declare(strict_types=1);

require_once __DIR__ . '/bootstrap.php';

$options = getopt('', ['file:']);
$file = (string)($options['file'] ?? '');

if ($file === '') {
    fwrite(STDERR, "Usage: php tools/precheck_config.php --file <publish.json>\n");
    exit(2);
}

try {
    $spec = read_json_file($file);
    $svc = build_management_service();
    $result = $svc->precheck($spec);
    cli_json($result);
    exit($result['ok'] ? 0 : 1);
} catch (Throwable $e) {
    cli_json(['ok' => false, 'error' => $e->getMessage()]);
    exit(1);
}
