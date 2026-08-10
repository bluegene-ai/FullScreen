# Self-hosted auto-update directory

This directory is served as the client's **self-hosted update source**.

## Layout (convention)

```
public/update/
├── FullScreenBrowser.exe   # the newest client build
└── latest.txt               # manifest: version / sha256 / filename
```

`latest.txt` is a plain three-line text file:

```
2026.08.10
<64 lowercase hex chars = SHA-256 of FullScreenBrowser.exe>
FullScreenBrowser.exe
```

The client fetches `<updateDirUrl>/latest.txt`, and only applies the update
when:

1. the version is newer than the running exe, AND
2. the downloaded exe's SHA-256 matches the manifest.

## Publish a new build

```bash
# from server/
php tools/publish_update.php --exe ../build/FullScreenBrowser.exe --version 2026.08.10
```

The script copies the exe, computes the SHA-256 and rewrites `latest.txt`.
The PHP server (php -S / Apache) serves these files statically — no PHP
changes are needed.

## Notes

- Keep the version in `latest.txt` identical to the one embedded in the exe
  (`src/version.h`, injected by the CI build) so version comparison is exact.
- The exe filename is fixed (`FullScreenBrowser.exe`) so the client can
  download it; do not rename it.
