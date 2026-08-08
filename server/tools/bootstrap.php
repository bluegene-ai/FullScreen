<?php

declare(strict_types=1);

use FullScreenServer\ManagementService;
use FullScreenServer\Storage;

require_once __DIR__ . '/../src/Storage.php';
require_once __DIR__ . '/../src/AuditService.php';
require_once __DIR__ . '/../src/ConfigValidator.php';
require_once __DIR__ . '/../src/ManagementService.php';
require_once __DIR__ . '/../src/PasswordPayloadCodec.php';

function build_management_service(): ManagementService
{
    $baseDir = dirname(__DIR__);
    $storage = new Storage($baseDir . '/storage', $baseDir . '/runtime');
    return new ManagementService($storage);
}

function read_json_file(string $path): array
{
    if (!is_file($path)) {
        throw new RuntimeException('file not found: ' . $path);
    }

    $raw = file_get_contents($path);
    if (!is_string($raw)) {
        throw new RuntimeException('cannot read file: ' . $path);
    }

    $decoded = json_decode($raw, true);
    if (!is_array($decoded)) {
        throw new RuntimeException('invalid json: ' . $path);
    }

    return $decoded;
}

function cli_json(array $data): void
{
    echo json_encode($data, JSON_PRETTY_PRINT | JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES) . PHP_EOL;
}
