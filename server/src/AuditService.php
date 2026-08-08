<?php

declare(strict_types=1);

namespace FullScreenServer;

final class AuditService
{
    private Storage $storage;

    public function __construct(Storage $storage)
    {
        $this->storage = $storage;
    }

    public function append(string $action, array $detail): void
    {
        $entry = [
            'time' => time(),
            'action' => $action,
            'detail' => $detail,
        ];

        $this->storage->updateJson('audit_logs.json', function (array $current) use ($entry): array {
            $entries = $current['entries'] ?? [];
            if (!is_array($entries)) {
                $entries = [];
            }

            $entries[] = $entry;
            if (count($entries) > 5000) {
                $entries = array_slice($entries, -5000);
            }

            return ['entries' => $entries];
        });
    }
}
