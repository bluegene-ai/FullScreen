<?php

declare(strict_types=1);

namespace FullScreenServer;

/**
 * Publishes a client exe into the self-hosted auto-update directory
 * (public/update/) and (re)writes latest.txt (version / sha256 / filename).
 *
 * Shared by the CLI tool (tools/publish_update.php) and the web admin
 * (/FS-RM/update) so both keep identical behavior.
 */
final class UpdatePublisher
{
    private string $updateDir;

    public function __construct(string $publicDir)
    {
        $this->updateDir = rtrim($publicDir, '/\\') . '/update';
    }

    public function updateDir(): string
    {
        return $this->updateDir;
    }

    /**
     * Version from a filename like "FullScreenBrowser-2026.08.10.exe".
     * Returns '' when the name carries no version.
     */
    public static function inferVersionFromFilename(string $name): string
    {
        if (preg_match('/^FullScreenBrowser[-_](.+?)\.exe$/i', $name, $m)) {
            return trim($m[1]);
        }
        return '';
    }

    /**
     * Copy $exeSource into the update dir and rewrite latest.txt.
     *
     * @return array{ok:bool, version?:string, sha256?:string, error?:string}
     */
    public function publish(string $exeSource, string $version): array
    {
        $version = trim($version);
        if ($version === '') {
            return ['ok' => false, 'error' => '版本号不能为空'];
        }
        if (!is_file($exeSource)) {
            return ['ok' => false, 'error' => 'exe 文件不存在'];
        }
        if (!is_dir($this->updateDir) && !mkdir($this->updateDir, 0775, true)) {
            return ['ok' => false, 'error' => '无法创建 update 目录: ' . $this->updateDir];
        }

        $dest = $this->updateDir . '/FullScreenBrowser.exe';
        if (!copy($exeSource, $dest)) {
            return ['ok' => false, 'error' => '无法将 exe 复制到 update 目录（检查写权限）'];
        }

        $sha = hash_file('sha256', $dest);
        if ($sha === false) {
            return ['ok' => false, 'error' => '计算 SHA-256 失败'];
        }

        $manifest = $version . "\n" . $sha . "\nFullScreenBrowser.exe\n";
        if (file_put_contents($this->updateDir . '/latest.txt', $manifest) === false) {
            return ['ok' => false, 'error' => '写入 latest.txt 失败'];
        }

        return ['ok' => true, 'version' => $version, 'sha256' => $sha];
    }

    /**
     * @return array{exists:bool, version:string, sha256:string, file:string}
     */
    public function currentManifest(): array
    {
        $path = $this->updateDir . '/latest.txt';
        if (!is_file($path)) {
            return ['exists' => false, 'version' => '', 'sha256' => '', 'file' => ''];
        }
        $raw = file($path, FILE_IGNORE_NEW_LINES);
        $lines = [];
        foreach (is_array($raw) ? $raw : [] as $line) {
            $line = trim($line);
            if ($line !== '') {
                $lines[] = $line;
            }
        }
        return [
            'exists' => true,
            'version' => $lines[0] ?? '',
            'sha256' => $lines[1] ?? '',
            'file' => $lines[2] ?? '',
        ];
    }
}
