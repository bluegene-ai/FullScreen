<?php

declare(strict_types=1);

namespace FullScreenServer;

final class Auth
{
    private Storage $storage;

    public function __construct(Storage $storage)
    {
        $this->storage = $storage;
    }

    public function consumeRegisterCode(string $code, string $deviceId): ?string
    {
        $error = null;

        $this->storage->updateJson('register_codes.json', function (array $current) use ($code, $deviceId, &$error): array {
            $codes = $current['codes'] ?? [];
            if (!is_array($codes)) {
                $codes = [];
            }

            $found = false;
            $updated = [];
            $now = time();

            foreach ($codes as $item) {
                if (!is_array($item)) {
                    continue;
                }
                $itemCode = (string)($item['code'] ?? '');
                if ($itemCode !== $code) {
                    $updated[] = $item;
                    continue;
                }

                $found = true;
                $used = (bool)($item['used'] ?? false);
                $expiresAt = (int)($item['expires_at'] ?? 0);

                if ($used) {
                    $error = 'invalid_register_code';
                    $updated[] = $item;
                    continue;
                }

                if ($expiresAt > 0 && $expiresAt < $now) {
                    $error = 'register_code_expired';
                    $updated[] = $item;
                    continue;
                }

                $item['used'] = true;
                $item['used_by_device'] = $deviceId;
                $item['used_at'] = $now;
                $updated[] = $item;
            }

            if (!$found) {
                $error = 'invalid_register_code';
            }

            return ['codes' => $updated];
        });

        return $error;
    }

    public function createRegisterCode(string $code, int $expiresAt): bool
    {
        if ($code === '' || $expiresAt <= time()) {
            return false;
        }

        $added = false;
        $this->storage->updateJson('register_codes.json', function (array $current) use ($code, $expiresAt, &$added): array {
            $codes = $current['codes'] ?? [];
            if (!is_array($codes)) {
                $codes = [];
            }

            foreach ($codes as $item) {
                if (is_array($item) && (string)($item['code'] ?? '') === $code) {
                    return ['codes' => $codes]; // already exists
                }
            }

            $codes[] = [
                'code' => $code,
                'expires_at' => $expiresAt,
                'used' => false,
                'created_at' => time(),
            ];
            $added = true;
            return ['codes' => $codes];
        });

        return $added;
    }

    public function issueDeviceToken(string $deviceId): string
    {
        $token = 'dtk_' . bin2hex(random_bytes(24));
        $tokenHash = hash('sha256', $token);

        $this->storage->updateJson('devices.json', function (array $current) use ($deviceId, $tokenHash): array {
            $devices = $current['devices'] ?? [];
            if (!is_array($devices)) {
                $devices = [];
            }

            $existing = $devices[$deviceId] ?? [];
            if (!is_array($existing)) {
                $existing = [];
            }

            $devices[$deviceId] = [
                'token_hash' => $tokenHash,
                'status' => (string)($existing['status'] ?? 'active'),
                'group_id' => (string)($existing['group_id'] ?? 'default'),
                'last_seen_at' => (int)($existing['last_seen_at'] ?? 0),
                'created_at' => (int)($existing['created_at'] ?? time()),
            ];

            return ['devices' => $devices];
        });

        return $token;
    }

    public function verifyDeviceToken(string $deviceId, string $token): bool
    {
        $data = $this->storage->readJson('devices.json', ['devices' => []]);
        $devices = $data['devices'] ?? [];
        if (!is_array($devices) || !isset($devices[$deviceId]) || !is_array($devices[$deviceId])) {
            return false;
        }

        $device = $devices[$deviceId];
        if (($device['status'] ?? 'active') !== 'active') {
            return false;
        }

        $tokenHash = hash('sha256', $token);
        return hash_equals((string)($device['token_hash'] ?? ''), $tokenHash);
    }

    public function getDeviceTokenHash(string $deviceId): string
    {
        $data = $this->storage->readJson('devices.json', ['devices' => []]);
        $devices = $data['devices'] ?? [];
        if (!is_array($devices) || !isset($devices[$deviceId]) || !is_array($devices[$deviceId])) {
            return '';
        }

        return (string)($devices[$deviceId]['token_hash'] ?? '');
    }

    public function touchDeviceSeen(string $deviceId): void
    {
        $this->storage->updateJson('devices.json', function (array $current) use ($deviceId): array {
            $devices = $current['devices'] ?? [];
            if (!is_array($devices) || !isset($devices[$deviceId]) || !is_array($devices[$deviceId])) {
                return $current;
            }

            $devices[$deviceId]['last_seen_at'] = time();
            return ['devices' => $devices];
        });
    }

    public function getDeviceGroup(string $deviceId): string
    {
        $data = $this->storage->readJson('devices.json', ['devices' => []]);
        $devices = $data['devices'] ?? [];
        if (!is_array($devices) || !isset($devices[$deviceId]) || !is_array($devices[$deviceId])) {
            return 'default';
        }

        $groupId = (string)($devices[$deviceId]['group_id'] ?? 'default');
        return $groupId !== '' ? $groupId : 'default';
    }

    public function setDeviceGroup(string $deviceId, string $groupId): bool
    {
        if ($groupId === '') {
            return false;
        }

        $found = false;
        $this->storage->updateJson('devices.json', function (array $current) use ($deviceId, $groupId, &$found): array {
            $devices = $current['devices'] ?? [];
            if (!is_array($devices) || !isset($devices[$deviceId]) || !is_array($devices[$deviceId])) {
                return $current;
            }

            $devices[$deviceId]['group_id'] = $groupId;
            $found = true;
            return ['devices' => $devices];
        });

        return $found;
    }

    public function signPayloadForDevice(string $deviceId, array $payload): string
    {
        $secret = $this->getDeviceTokenHash($deviceId);
        if ($secret === '') {
            // No registered token hash for this device: cannot sign. Fail
            // closed instead of falling back to a hardcoded shared secret.
            return '';
        }
        return $this->signPayloadWithSecret($payload, $secret);
    }

    private function signPayloadWithSecret(array $payload, string $secret): string
    {
        if ($secret === '') {
            return '';
        }

        $copy = $payload;
        unset($copy['signature']);
        $json = json_encode($copy, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
        if (!is_string($json)) {
            $json = '{}';
        }

        return hash_hmac('sha256', $json, $secret);
    }
}
