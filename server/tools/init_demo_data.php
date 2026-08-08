<?php

declare(strict_types=1);

require_once __DIR__ . '/bootstrap.php';

$options = getopt('', ['register-code:', 'device-id:', 'force']);
$registerCode = isset($options['register-code']) ? trim((string)$options['register-code']) : 'DEMO-REGISTER-CODE';
$deviceId = isset($options['device-id']) ? trim((string)$options['device-id']) : 'dev-001';
$force = array_key_exists('force', $options);

$baseDir = dirname(__DIR__);
$storage = new FullScreenServer\Storage($baseDir . '/storage', $baseDir . '/runtime');
$svc = build_management_service();

try {
    $storage->updateJson('register_codes.json', function (array $current) use ($registerCode): array {
        $codes = $current['codes'] ?? [];
        if (!is_array($codes)) {
            $codes = [];
        }

        foreach ($codes as $item) {
            if (is_array($item) && (string)($item['code'] ?? '') === $registerCode) {
                return ['codes' => $codes];
            }
        }

        $codes[] = [
            'code' => $registerCode,
            'expires_at' => time() + 86400 * 30,
            'used' => false,
        ];
        return ['codes' => $codes];
    });

    $global = $storage->readJson('config_global.json', ['revision' => 1, 'config' => []]);
    $globalCfg = is_array($global['config'] ?? null) ? $global['config'] : [];
    if ($force || count($globalCfg) === 0) {
        $svc->publish([
            'scope' => 'global',
            'scopeId' => '',
            'operator' => 'demo-init',
            'note' => 'seed demo global config',
            'config' => [
                'url' => 'https://example.com/dashboard',
                'zoomPercent' => 110,
                'refreshMode' => 1,
                'refreshIntervalSec' => 60,
                'refreshDailyMin' => 0,
                'burnInPrevention' => true,
                'unreachableMsg' => '网络异常，请稍后重试',
                'allowRemotePasswordUpdate' => true,
            ],
        ]);
    }

    $device = $storage->readJson('config_device.json', ['devices' => []]);
    $deviceNode = $device['devices'][$deviceId] ?? null;
    if ($force || !is_array($deviceNode)) {
        $svc->publish([
            'scope' => 'device',
            'scopeId' => $deviceId,
            'operator' => 'demo-init',
            'note' => 'seed demo device override',
            'config' => [
                'url' => 'https://example.com/device-001',
            ],
        ]);
    }

    cli_json([
        'ok' => true,
        'registerCode' => $registerCode,
        'deviceId' => $deviceId,
        'message' => 'demo data initialized',
    ]);
    exit(0);
} catch (Throwable $e) {
    cli_json(['ok' => false, 'error' => $e->getMessage()]);
    exit(1);
}
