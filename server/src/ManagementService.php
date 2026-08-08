<?php

declare(strict_types=1);

namespace FullScreenServer;

require_once __DIR__ . '/Storage.php';
require_once __DIR__ . '/AuditService.php';
require_once __DIR__ . '/Auth.php';
require_once __DIR__ . '/ConfigValidator.php';
require_once __DIR__ . '/PasswordPayloadCodec.php';

final class ManagementService
{
    private Storage $storage;
    private AuditService $audit;
    private Auth $auth;
    private ConfigValidator $validator;
    private PasswordPayloadCodec $passwordCodec;

    public function __construct(Storage $storage)
    {
        $this->storage = $storage;
        $this->audit = new AuditService($storage);
        $this->auth = new Auth($storage);
        $this->validator = new ConfigValidator();
        $this->passwordCodec = new PasswordPayloadCodec();
    }

    /**
     * @return array{ok:bool, errors:string[], normalized:array<string,mixed>}
     */
    public function precheck(array $publishSpec): array
    {
        $errors = [];

        $scope = trim((string)($publishSpec['scope'] ?? ''));
        $scopeId = trim((string)($publishSpec['scopeId'] ?? ''));
        $config = $publishSpec['config'] ?? null;

        if (!in_array($scope, ['global', 'group', 'device'], true)) {
            $errors[] = 'scope must be global|group|device';
        }

        if (($scope === 'group' || $scope === 'device') && $scopeId === '') {
            $errors[] = 'scopeId is required for group/device scope';
        }

        if (!is_array($config)) {
            $errors[] = 'config must be an object';
        }

        if (count($errors) > 0) {
            return ['ok' => false, 'errors' => $errors, 'normalized' => []];
        }

        $check = $this->validator->validateConfig($config);
        if (!$check['ok']) {
            return $check;
        }

        $normalized = [
            'scope' => $scope,
            'scopeId' => $scopeId,
            'operator' => trim((string)($publishSpec['operator'] ?? 'unknown')),
            'note' => trim((string)($publishSpec['note'] ?? '')),
            'config' => $check['normalized'],
        ];

        return ['ok' => true, 'errors' => [], 'normalized' => $normalized];
    }

    /**
     * @param array<string,mixed> $normalized
     * @return array{revision:int, scope:string, scopeId:string}
     */
    public function publish(array $normalized): array
    {
        $scope = (string)$normalized['scope'];
        $scopeId = (string)$normalized['scopeId'];
        $operator = (string)$normalized['operator'];
        $note = (string)$normalized['note'];
        $config = (array)$normalized['config'];

        $revision = 1;
        $before = [];
        $after = [];

        if ($scope === 'global') {
            $global = $this->storage->readJson('config_global.json', ['revision' => 1, 'config' => []]);
            $before = is_array($global['config'] ?? null) ? $global['config'] : [];
            $revision = ((int)($global['revision'] ?? 1)) + 1;
            $after = array_merge($before, $config);

            $this->storage->updateJson('config_global.json', function () use ($revision, $after): array {
                return ['revision' => $revision, 'config' => $after];
            });
        } elseif ($scope === 'group') {
            $groups = $this->storage->readJson('config_group.json', ['groups' => []]);
            $node = $groups['groups'][$scopeId] ?? ['revision' => 0, 'config' => []];
            if (!is_array($node)) {
                $node = ['revision' => 0, 'config' => []];
            }
            $before = is_array($node['config'] ?? null) ? $node['config'] : [];
            $revision = ((int)($node['revision'] ?? 0)) + 1;
            $after = array_merge($before, $config);

            $this->storage->updateJson('config_group.json', function (array $current) use ($scopeId, $revision, $after): array {
                $all = $current['groups'] ?? [];
                if (!is_array($all)) {
                    $all = [];
                }
                $all[$scopeId] = ['revision' => $revision, 'config' => $after];
                return ['groups' => $all];
            });
        } else {
            $devices = $this->storage->readJson('config_device.json', ['devices' => []]);
            $node = $devices['devices'][$scopeId] ?? ['revision' => 0, 'config' => []];
            if (!is_array($node)) {
                $node = ['revision' => 0, 'config' => []];
            }
            $before = is_array($node['config'] ?? null) ? $node['config'] : [];
            $revision = ((int)($node['revision'] ?? 0)) + 1;
            $after = array_merge($before, $config);

            $this->storage->updateJson('config_device.json', function (array $current) use ($scopeId, $revision, $after): array {
                $all = $current['devices'] ?? [];
                if (!is_array($all)) {
                    $all = [];
                }
                $all[$scopeId] = ['revision' => $revision, 'config' => $after];
                return ['devices' => $all];
            });
        }

        $this->appendHistory([
            'time' => time(),
            'scope' => $scope,
            'scopeId' => $scopeId,
            'revision' => $revision,
            'operator' => $operator,
            'note' => $note,
            'before' => $before,
            'after' => $after,
        ]);

        $this->audit->append('config_published', [
            'scope' => $scope,
            'scopeId' => $scopeId,
            'revision' => $revision,
            'operator' => $operator,
            'note' => $note,
        ]);

        return ['revision' => $revision, 'scope' => $scope, 'scopeId' => $scopeId];
    }

    /**
     * @return array{scope:string, scopeId:string, revision:int, config:array<string,mixed>, exists:bool}
     */
    public function getScopeConfig(string $scope, string $scopeId): array
    {
        if ($scope === 'global') {
            $global = $this->storage->readJson('config_global.json', ['revision' => 1, 'config' => []]);
            return [
                'scope' => 'global',
                'scopeId' => '',
                'revision' => (int)($global['revision'] ?? 1),
                'config' => is_array($global['config'] ?? null) ? $global['config'] : [],
            ];
        }

        if ($scope === 'group') {
            $groups = $this->storage->readJson('config_group.json', ['groups' => []]);
            $node = $groups['groups'][$scopeId] ?? null;
            if (!is_array($node)) {
                return ['scope' => 'group', 'scopeId' => $scopeId, 'revision' => 0, 'config' => []];
            }

            return [
                'scope' => 'group',
                'scopeId' => $scopeId,
                'revision' => (int)($node['revision'] ?? 0),
                'config' => is_array($node['config'] ?? null) ? $node['config'] : [],
            ];
        }

        if ($scope === 'device') {
            $devices = $this->storage->readJson('config_device.json', ['devices' => []]);
            $node = $devices['devices'][$scopeId] ?? null;
            if (!is_array($node)) {
                return ['scope' => 'device', 'scopeId' => $scopeId, 'revision' => 0, 'config' => []];
            }

            return [
                'scope' => 'device',
                'scopeId' => $scopeId,
                'revision' => (int)($node['revision'] ?? 0),
                'config' => is_array($node['config'] ?? null) ? $node['config'] : [],
            ];
        }

        return ['scope' => $scope, 'scopeId' => $scopeId, 'revision' => 0, 'config' => []];
    }

    /**
     * @return array{scope:string, scopeId:string, revision:int, config:array<string,mixed>, groupId:string}
     */
    public function getEffectiveScopePreview(string $scope, string $scopeId): array
    {
        $global = $this->storage->readJson('config_global.json', ['revision' => 1, 'config' => []]);
        $globalCfg = is_array($global['config'] ?? null) ? $global['config'] : [];
        $globalRev = (int)($global['revision'] ?? 1);

        if ($scope === 'global') {
            return [
                'scope' => 'global',
                'scopeId' => '',
                'revision' => $globalRev,
                'config' => $this->filterWhitelist($globalCfg),
                'groupId' => '',
            ];
        }

        if ($scope === 'group') {
            $groups = $this->storage->readJson('config_group.json', ['groups' => []]);
            $node = $groups['groups'][$scopeId] ?? [];
            if (!is_array($node)) {
                $node = [];
            }
            $groupCfg = is_array($node['config'] ?? null) ? $node['config'] : [];
            $groupRev = (int)($node['revision'] ?? 0);

            return [
                'scope' => 'group',
                'scopeId' => $scopeId,
                'revision' => max($globalRev, $groupRev),
                'config' => $this->filterWhitelist(array_merge($globalCfg, $groupCfg)),
                'groupId' => $scopeId,
            ];
        }

        if ($scope === 'device') {
            $groupId = $this->auth->getDeviceGroup($scopeId);
            $groups = $this->storage->readJson('config_group.json', ['groups' => []]);
            $devices = $this->storage->readJson('config_device.json', ['devices' => []]);

            $groupNode = $groups['groups'][$groupId] ?? [];
            if (!is_array($groupNode)) {
                $groupNode = [];
            }
            $deviceNode = $devices['devices'][$scopeId] ?? [];
            if (!is_array($deviceNode)) {
                $deviceNode = [];
            }

            $groupCfg = is_array($groupNode['config'] ?? null) ? $groupNode['config'] : [];
            $deviceCfg = is_array($deviceNode['config'] ?? null) ? $deviceNode['config'] : [];
            $groupRev = (int)($groupNode['revision'] ?? 0);
            $deviceRev = (int)($deviceNode['revision'] ?? 0);

            return [
                'scope' => 'device',
                'scopeId' => $scopeId,
                'revision' => max($globalRev, $groupRev, $deviceRev),
                'config' => $this->filterWhitelist(array_merge($globalCfg, $groupCfg, $deviceCfg)),
                'groupId' => $groupId,
            ];
        }

        return ['scope' => $scope, 'scopeId' => $scopeId, 'revision' => 0, 'config' => [], 'groupId' => ''];
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

    /**
     * @return array{ok:bool, message:string}
     */
    public function rollback(string $scope, string $scopeId, int $revision, string $operator, string $note): array
    {
        $history = $this->storage->readJson('config_history.json', ['entries' => []]);
        $entries = $history['entries'] ?? [];
        if (!is_array($entries)) {
            $entries = [];
        }

        $target = null;
        foreach ($entries as $entry) {
            if (!is_array($entry)) {
                continue;
            }
            if ((string)($entry['scope'] ?? '') === $scope
                && (string)($entry['scopeId'] ?? '') === $scopeId
                && (int)($entry['revision'] ?? 0) === $revision) {
                $target = $entry;
                break;
            }
        }

        if (!is_array($target)) {
            return ['ok' => false, 'message' => 'target revision not found in history'];
        }

        $before = [];
        $after = is_array($target['before'] ?? null) ? $target['before'] : [];

        if ($scope === 'global') {
            $global = $this->storage->readJson('config_global.json', ['revision' => 1, 'config' => []]);
            $before = is_array($global['config'] ?? null) ? $global['config'] : [];
            $newRevision = ((int)($global['revision'] ?? 1)) + 1;

            $this->storage->updateJson('config_global.json', function () use ($newRevision, $after): array {
                return ['revision' => $newRevision, 'config' => $after];
            });

            $this->appendHistory([
                'time' => time(),
                'scope' => 'global',
                'scopeId' => '',
                'revision' => $newRevision,
                'operator' => $operator,
                'note' => 'rollback to history revision ' . $revision . ' ' . $note,
                'before' => $before,
                'after' => $after,
            ]);
        } elseif ($scope === 'group') {
            $groups = $this->storage->readJson('config_group.json', ['groups' => []]);
            $node = $groups['groups'][$scopeId] ?? ['revision' => 0, 'config' => []];
            if (!is_array($node)) {
                $node = ['revision' => 0, 'config' => []];
            }
            $before = is_array($node['config'] ?? null) ? $node['config'] : [];
            $newRevision = ((int)($node['revision'] ?? 0)) + 1;

            $this->storage->updateJson('config_group.json', function (array $current) use ($scopeId, $newRevision, $after): array {
                $all = $current['groups'] ?? [];
                if (!is_array($all)) {
                    $all = [];
                }
                $all[$scopeId] = ['revision' => $newRevision, 'config' => $after];
                return ['groups' => $all];
            });

            $this->appendHistory([
                'time' => time(),
                'scope' => 'group',
                'scopeId' => $scopeId,
                'revision' => $newRevision,
                'operator' => $operator,
                'note' => 'rollback to history revision ' . $revision . ' ' . $note,
                'before' => $before,
                'after' => $after,
            ]);
        } else {
            $devices = $this->storage->readJson('config_device.json', ['devices' => []]);
            $node = $devices['devices'][$scopeId] ?? ['revision' => 0, 'config' => []];
            if (!is_array($node)) {
                $node = ['revision' => 0, 'config' => []];
            }
            $before = is_array($node['config'] ?? null) ? $node['config'] : [];
            $newRevision = ((int)($node['revision'] ?? 0)) + 1;

            $this->storage->updateJson('config_device.json', function (array $current) use ($scopeId, $newRevision, $after): array {
                $all = $current['devices'] ?? [];
                if (!is_array($all)) {
                    $all = [];
                }
                $all[$scopeId] = ['revision' => $newRevision, 'config' => $after];
                return ['devices' => $all];
            });

            $this->appendHistory([
                'time' => time(),
                'scope' => 'device',
                'scopeId' => $scopeId,
                'revision' => $newRevision,
                'operator' => $operator,
                'note' => 'rollback to history revision ' . $revision . ' ' . $note,
                'before' => $before,
                'after' => $after,
            ]);
        }

        $this->audit->append('config_rollback', [
            'scope' => $scope,
            'scopeId' => $scopeId,
            'targetRevision' => $revision,
            'operator' => $operator,
            'note' => $note,
        ]);

        return ['ok' => true, 'message' => 'rollback applied'];
    }

    /**
     * @return array<int,array<string,mixed>>
     */
    public function queryAudit(?string $action, ?string $deviceId, int $limit): array
    {
        $logs = $this->storage->readJson('audit_logs.json', ['entries' => []]);
        $entries = $logs['entries'] ?? [];
        if (!is_array($entries)) {
            return [];
        }

        $filtered = [];
        for ($i = count($entries) - 1; $i >= 0; --$i) {
            $entry = $entries[$i] ?? null;
            if (!is_array($entry)) {
                continue;
            }

            if ($action !== null && $action !== '' && (string)($entry['action'] ?? '') !== $action) {
                continue;
            }

            if ($deviceId !== null && $deviceId !== '') {
                $detail = $entry['detail'] ?? [];
                if (!is_array($detail) || (string)($detail['deviceId'] ?? '') !== $deviceId) {
                    continue;
                }
            }

            $filtered[] = $entry;
            if (count($filtered) >= $limit) {
                break;
            }
        }

        return $filtered;
    }

    /**
     * @return array{ok:bool, commandId:string, effectiveAt:int, expireAt:int}
     */
    public function enqueuePasswordUpdate(string $deviceId,
                                          string $plainPassword,
                                          string $operator,
                                          string $note,
                                          int $effectiveAt,
                                          int $expireAt): array
    {
        if ($deviceId === '') {
            throw new \InvalidArgumentException('deviceId is required');
        }
        if ($plainPassword === '') {
            throw new \InvalidArgumentException('plainPassword is required');
        }

        $commandId = 'pwd-' . gmdate('YmdHis') . '-' . substr(bin2hex(random_bytes(4)), 0, 8);
        $payload = [
            'commandId' => $commandId,
            'effectiveAt' => $effectiveAt,
            'expireAt' => $expireAt,
            'encryptedPassword' => $this->passwordCodec->encode($plainPassword),
            'consumed' => false,
        ];

        $this->storage->updateJson('password_updates.json', function (array $current) use ($deviceId, $payload): array {
            $devices = $current['devices'] ?? [];
            if (!is_array($devices)) {
                $devices = [];
            }
            $commands = $devices[$deviceId] ?? [];
            if (!is_array($commands)) {
                $commands = [];
            }
            $commands[] = $payload;
            $devices[$deviceId] = $commands;
            return ['devices' => $devices];
        });

        $this->audit->append('password_update_enqueued', [
            'deviceId' => $deviceId,
            'commandId' => $commandId,
            'operator' => $operator,
            'note' => $note,
            'effectiveAt' => $effectiveAt,
            'expireAt' => $expireAt,
        ]);

        return [
            'ok' => true,
            'commandId' => $commandId,
            'effectiveAt' => $effectiveAt,
            'expireAt' => $expireAt,
        ];
    }

    private function appendHistory(array $entry): void
    {
        $this->storage->updateJson('config_history.json', function (array $current) use ($entry): array {
            $entries = $current['entries'] ?? [];
            if (!is_array($entries)) {
                $entries = [];
            }

            $entries[] = $entry;
            if (count($entries) > 2000) {
                $entries = array_slice($entries, -2000);
            }
            return ['entries' => $entries];
        });
    }
}
