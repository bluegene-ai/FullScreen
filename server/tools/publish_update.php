<?php

declare(strict_types=1);

use FullScreenServer\UpdatePublisher;

/**
 * Publish a client exe to the self-hosted auto-update directory.
 *
 * The client (src/auto_update.cpp) treats this directory as the update
 * source: it fetches latest.txt and, when the version is newer, downloads
 * FullScreenBrowser.exe and verifies its SHA-256 before applying it.
 *
 * This CLI shares the same core (UpdatePublisher) as the web admin
 * "更新发布" page (/FS-RM/update).
 *
 * Usage (from server/):
 *   php tools/publish_update.php --exe ../build/FullScreenBrowser.exe --version 2026.08.10
 *
 * --version is optional if the exe filename is versioned, e.g.
 *   FullScreenBrowser-2026.08.10.exe  ->  version 2026.08.10
 */

require_once __DIR__ . '/../src/UpdatePublisher.php';

$options = getopt('', ['exe:', 'version:']);
$exe = (string)($options['exe'] ?? '');
$version = (string)($options['version'] ?? '');

if ($exe === '' || !is_file($exe)) {
    fwrite(STDERR, "Usage: php tools/publish_update.php --exe <path-to-exe> [--version <ver>]\n");
    exit(2);
}

if ($version === '') {
    $version = UpdatePublisher::inferVersionFromFilename(basename($exe));
}
if ($version === '') {
    fwrite(STDERR, "Version required (--version or versioned filename).\n");
    exit(2);
}

$publisher = new UpdatePublisher(dirname(__DIR__) . '/public');
$result = $publisher->publish($exe, $version);

if (!(bool)($result['ok'] ?? false)) {
    fwrite(STDERR, "Publish failed: " . ($result['error'] ?? 'unknown') . "\n");
    exit(1);
}

echo "Published update {$result['version']} -> {$publisher->updateDir()}/FullScreenBrowser.exe\n";
echo "sha256: {$result['sha256']}\n";
echo "manifest: {$publisher->updateDir()}/latest.txt\n";
exit(0);
