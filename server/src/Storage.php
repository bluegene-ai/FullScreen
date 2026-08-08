<?php

declare(strict_types=1);

namespace FullScreenServer;

final class Storage
{
    private string $storageDir;
    private string $runtimeDir;

    public function __construct(string $storageDir, string $runtimeDir)
    {
        $this->storageDir = rtrim($storageDir, DIRECTORY_SEPARATOR);
        $this->runtimeDir = rtrim($runtimeDir, DIRECTORY_SEPARATOR);

        $this->ensureDir($this->storageDir);
        $this->ensureDir($this->runtimeDir);
        $this->ensureDir($this->runtimeDir . DIRECTORY_SEPARATOR . 'locks');
        $this->ensureDir($this->runtimeDir . DIRECTORY_SEPARATOR . 'tmp');

        $this->bootstrapDefaults();
    }

    public function readJson(string $fileName, array $default): array
    {
        $path = $this->path($fileName);
        if (!is_file($path)) {
            return $default;
        }

        $content = file_get_contents($path);
        if (!is_string($content) || trim($content) === '') {
            return $default;
        }

        $decoded = json_decode($content, true);
        return is_array($decoded) ? $decoded : $default;
    }

    public function updateJson(string $fileName, callable $updater): array
    {
        return $this->withLock($fileName, function () use ($fileName, $updater): array {
            $current = $this->readJson($fileName, []);
            $next = $updater($current);
            if (!is_array($next)) {
                $next = $current;
            }
            $this->writeJsonAtomic($fileName, $next);
            return $next;
        });
    }

    public function writeJsonAtomic(string $fileName, array $data): void
    {
        $path = $this->path($fileName);
        $tmpPath = $this->runtimeDir . DIRECTORY_SEPARATOR . 'tmp' . DIRECTORY_SEPARATOR . $fileName . '.tmp';

        $encoded = json_encode($data, JSON_PRETTY_PRINT | JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
        if (!is_string($encoded)) {
            throw new \RuntimeException('json_encode failed');
        }

        if (file_put_contents($tmpPath, $encoded) === false) {
            throw new \RuntimeException('failed to write temp file');
        }

        if (!rename($tmpPath, $path)) {
            @unlink($tmpPath);
            throw new \RuntimeException('failed to replace target file');
        }
    }

    public function withLock(string $fileName, callable $callback): array
    {
        $lockFile = $this->runtimeDir . DIRECTORY_SEPARATOR . 'locks' . DIRECTORY_SEPARATOR . $fileName . '.lock';
        $fh = fopen($lockFile, 'c+');
        if ($fh === false) {
            throw new \RuntimeException('cannot open lock file');
        }

        try {
            if (!flock($fh, LOCK_EX)) {
                throw new \RuntimeException('cannot acquire lock');
            }
            $result = $callback();
            flock($fh, LOCK_UN);
            return is_array($result) ? $result : [];
        } finally {
            fclose($fh);
        }
    }

    public function path(string $fileName): string
    {
        return $this->storageDir . DIRECTORY_SEPARATOR . $fileName;
    }

    private function ensureDir(string $dir): void
    {
        if (!is_dir($dir) && !mkdir($dir, 0775, true) && !is_dir($dir)) {
            throw new \RuntimeException('cannot create dir: ' . $dir);
        }
    }

    private function bootstrapDefaults(): void
    {
        $defaults = [
            'devices.json' => ['devices' => []],
            'register_codes.json' => ['codes' => []],
            'config_global.json' => ['revision' => 1, 'config' => []],
            'config_group.json' => ['groups' => []],
            'config_device.json' => ['devices' => []],
            'config_history.json' => ['entries' => []],
            'password_updates.json' => ['devices' => []],
            'audit_logs.json' => ['entries' => []],
            'admin_auth.json' => ['initialized' => false],
            'server_secret.json' => ['secret' => bin2hex(random_bytes(32))],
        ];

        foreach ($defaults as $name => $defaultData) {
            $path = $this->path($name);
            if (!is_file($path)) {
                $this->writeJsonAtomic($name, $defaultData);
            }
        }
    }
}
