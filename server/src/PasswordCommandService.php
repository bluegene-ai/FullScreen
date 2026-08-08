<?php

declare(strict_types=1);

namespace FullScreenServer;

final class PasswordCommandService
{
    private Storage $storage;

    public function __construct(Storage $storage)
    {
        $this->storage = $storage;
    }

    public function getActiveCommands(string $deviceId, int $now): array
    {
        $data = $this->storage->readJson('password_updates.json', ['devices' => []]);
        $devices = $data['devices'] ?? [];
        if (!is_array($devices) || !isset($devices[$deviceId]) || !is_array($devices[$deviceId])) {
            return [];
        }

        $commands = [];
        foreach ($devices[$deviceId] as $cmd) {
            if (!is_array($cmd)) {
                continue;
            }

            $consumed = (bool)($cmd['consumed'] ?? false);
            $effectiveAt = (int)($cmd['effectiveAt'] ?? 0);
            $expireAt = (int)($cmd['expireAt'] ?? 0);

            if ($consumed) {
                continue;
            }
            if ($effectiveAt > 0 && $effectiveAt > $now) {
                continue;
            }
            if ($expireAt > 0 && $expireAt < $now) {
                continue;
            }

            $commands[] = [
                'type' => 'password_update',
                'commandId' => (string)($cmd['commandId'] ?? ''),
                'effectiveAt' => $effectiveAt,
                'expireAt' => $expireAt,
                'encryptedPassword' => (string)($cmd['encryptedPassword'] ?? ''),
            ];
        }

        return $commands;
    }

    public function markConsumed(string $deviceId, string $commandId, int $now): void
    {
        $this->storage->updateJson('password_updates.json', function (array $current) use ($deviceId, $commandId, $now): array {
            $devices = $current['devices'] ?? [];
            if (!is_array($devices) || !isset($devices[$deviceId]) || !is_array($devices[$deviceId])) {
                return $current;
            }

            foreach ($devices[$deviceId] as &$cmd) {
                if (!is_array($cmd)) {
                    continue;
                }
                if ((string)($cmd['commandId'] ?? '') === $commandId) {
                    $cmd['consumed'] = true;
                    $cmd['consumedAt'] = $now;
                }
            }
            unset($cmd);

            $current['devices'] = $devices;
            return $current;
        });
    }
}
