<?php

declare(strict_types=1);

namespace FullScreenServer;

require_once __DIR__ . '/Storage.php';
require_once __DIR__ . '/Auth.php';
require_once __DIR__ . '/ConfigValidator.php';

final class ConfigMergeService
{
    private Storage $storage;
    private Auth $auth;

    public function __construct(Storage $storage)
    {
        $this->storage = $storage;
        $this->auth = new Auth($storage);
    }

    public function buildMergedConfig(string $deviceId): array
    {
        $groupId = $this->auth->getDeviceGroup($deviceId);

        $global = $this->storage->readJson('config_global.json', ['revision' => 1, 'config' => []]);
        $groups = $this->storage->readJson('config_group.json', ['groups' => []]);
        $devices = $this->storage->readJson('config_device.json', ['devices' => []]);

        $globalCfg = is_array($global['config'] ?? null) ? $global['config'] : [];
        $globalRev = (int)($global['revision'] ?? 1);

        $groupNode = $groups['groups'][$groupId] ?? [];
        if (!is_array($groupNode)) {
            $groupNode = [];
        }
        $groupCfg = is_array($groupNode['config'] ?? null) ? $groupNode['config'] : [];
        $groupRev = (int)($groupNode['revision'] ?? 0);

        $deviceNode = $devices['devices'][$deviceId] ?? [];
        if (!is_array($deviceNode)) {
            $deviceNode = [];
        }
        $deviceCfg = is_array($deviceNode['config'] ?? null) ? $deviceNode['config'] : [];
        $deviceRev = (int)($deviceNode['revision'] ?? 0);

        $merged = array_merge($globalCfg, $groupCfg, $deviceCfg);
        $effective = $this->filterWhitelist($merged);
        $revision = max($globalRev, $groupRev, $deviceRev);

        return [
            'revision' => $revision,
            'config' => $effective,
        ];
    }

    private function filterWhitelist(array $config): array
    {
        $result = [];
        foreach (ConfigValidator::allowedFields() as $key) {
            if (array_key_exists($key, $config)) {
                $result[$key] = $config[$key];
            }
        }
        return $result;
    }
}
