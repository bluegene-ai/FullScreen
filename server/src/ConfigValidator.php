<?php

declare(strict_types=1);

namespace FullScreenServer;

final class ConfigValidator
{
    private const ALLOWED_FIELDS = [
        'url',
        'zoomPercent',
        'refreshMode',
        'refreshIntervalSec',
        'refreshDailyMin',
        'burnInPrevention',
        'unreachableMsg',
        'allowRemotePasswordUpdate',
    ];

    /**
     * Single source of truth for the fields that can be published/merged.
     *
     * @return list<string>
     */
    public static function allowedFields(): array
    {
        return self::ALLOWED_FIELDS;
    }

    /**
     * @return array{ok:bool, errors:string[], normalized:array<string,mixed>}
     */
    public function validateConfig(array $config): array
    {
        $errors = [];
        $normalized = [];

        foreach ($config as $k => $v) {
            if (!in_array((string)$k, self::ALLOWED_FIELDS, true)) {
                $errors[] = 'unknown field: ' . $k;
                continue;
            }
            $normalized[(string)$k] = $v;
        }

        if (isset($normalized['url'])) {
            $url = trim((string)$normalized['url']);
            if ($url === '' || !(str_starts_with($url, 'http://') || str_starts_with($url, 'https://'))) {
                $errors[] = 'url must start with http:// or https://';
            } else {
                $normalized['url'] = $url;
            }
        }

        if (isset($normalized['zoomPercent'])) {
            $zoom = (int)$normalized['zoomPercent'];
            if ($zoom < 50 || $zoom > 300) {
                $errors[] = 'zoomPercent must be in [50,300]';
            }
            $normalized['zoomPercent'] = $zoom;
        }

        if (isset($normalized['refreshMode'])) {
            $mode = (int)$normalized['refreshMode'];
            if (!in_array($mode, [0, 1, 2], true)) {
                $errors[] = 'refreshMode must be 0,1,2';
            }
            $normalized['refreshMode'] = $mode;
        }

        if (isset($normalized['refreshIntervalSec'])) {
            $sec = (int)$normalized['refreshIntervalSec'];
            if ($sec < 0 || $sec > 86400) {
                $errors[] = 'refreshIntervalSec must be in [0,86400]';
            }
            $normalized['refreshIntervalSec'] = $sec;
        }

        if (isset($normalized['refreshDailyMin'])) {
            $min = (int)$normalized['refreshDailyMin'];
            if ($min < 0 || $min > 1439) {
                $errors[] = 'refreshDailyMin must be in [0,1439]';
            }
            $normalized['refreshDailyMin'] = $min;
        }

        if (isset($normalized['burnInPrevention'])) {
            $normalized['burnInPrevention'] = (bool)$normalized['burnInPrevention'];
        }

        if (isset($normalized['allowRemotePasswordUpdate'])) {
            $normalized['allowRemotePasswordUpdate'] = (bool)$normalized['allowRemotePasswordUpdate'];
        }

        if (isset($normalized['unreachableMsg'])) {
            $msg = trim((string)$normalized['unreachableMsg']);
            if (mb_strlen($msg) > 1024) {
                $errors[] = 'unreachableMsg too long (>1024)';
            }
            $normalized['unreachableMsg'] = $msg;
        }

        return [
            'ok' => count($errors) === 0,
            'errors' => $errors,
            'normalized' => $normalized,
        ];
    }
}
