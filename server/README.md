# FullScreen Remote Config Server (MVP)

This is the phase-1 PHP + JSON backend for device registration, merged config pull, and apply acknowledgements.

## Endpoints

- GET /health
- POST /api/v1/device/register
- GET /api/v1/config/merged
- POST /api/v1/config/ack

## Management scripts (CLI)

From `server/` directory:

1. Precheck publish payload

php tools/precheck_config.php --file examples/publish-global.json

2. Publish config (full rollout by global scope)

php tools/publish_config.php --file examples/publish-global.json

Or use dedicated full-rollout wrapper (requires scope=global):

php tools/publish_full_rollout.php --file examples/publish-global.json

3. Rollback by history revision

php tools/rollback_config.php --scope global --revision 2 --operator ops-admin --note "rollback test"

4. Query audit entries

php tools/query_audit.php --limit 50
php tools/query_audit.php --action config_published --limit 20
php tools/query_audit.php --device-id dev-001 --limit 20

5. Enqueue remote password update for a device

php tools/enqueue_password_update.php --device-id dev-001 --password NewPass123 --operator ops-admin --note "rotate password"

6. Initialize demo data for local integration testing

php tools/init_demo_data.php
php tools/init_demo_data.php --register-code DEMO-001 --device-id dev-001 --force

## Run (dev)

Use PHP built-in server from the server directory:

php -S 127.0.0.1:8080 -t public

## Storage files

- storage/devices.json
- storage/register_codes.json
- storage/config_global.json
- storage/config_group.json
- storage/config_device.json
- storage/config_history.json
- storage/password_updates.json
- storage/audit_logs.json
- storage/server_secret.json

## Notes

- Register codes are one-time and short-lived.
- Device tokens are returned once and stored as hash server-side.
- Merged config responses are signed with HMAC-SHA256.
- JSON writes use lock + atomic replace.
