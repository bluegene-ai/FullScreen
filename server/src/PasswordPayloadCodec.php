<?php

declare(strict_types=1);

namespace FullScreenServer;

final class PasswordPayloadCodec
{
    private const XOR_KEY = [
        0x5A, 0x3F, 0x8C, 0x12, 0xE7, 0x4B, 0x9D, 0x26,
        0xF1, 0x08, 0x73, 0xDA, 0x45, 0xBC, 0x2E, 0x91,
    ];

    public function encode(string $plain): string
    {
        if ($plain === '') {
            return '';
        }

        $bytes = mb_convert_encoding($plain, 'UTF-8', 'UTF-8');
        $raw = $bytes;
        $len = strlen($raw);
        for ($i = 0; $i < $len; ++$i) {
            $raw[$i] = chr(ord($raw[$i]) ^ self::XOR_KEY[$i % count(self::XOR_KEY)]);
        }

        return base64_encode($raw);
    }
}
