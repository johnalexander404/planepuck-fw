# PlanePuck documentation

Detail pages behind the [main README](../README.md).

## Concepts
- [Architecture](architecture.md) — the `puck::` HAL, code layers, adding an app or a board
- [Physical-button navigation](buttons.md)
- [Build & deploy](build-and-deploy.md) — envs, build, flash, `release.sh`, CI
- [OTA, versions & fleet](ota.md) — updates, per-board isolation, version channels
- [Backend infrastructure](backend.md) — Mosquitto + Caddy + operator tooling
- [Hardware & boards](hardware.md) — CoreS3, the Waveshare scaffold, first flash

## Apps
[Clock](apps/clock.md) · [Weather](apps/weather.md) · [Flight](apps/flight.md) ·
[Emoji](apps/emoji.md) · [Friends](apps/friends.md) · [Settings](apps/settings.md)

## Operator runbook
[`tools/OTA-SETUP.md`](../tools/OTA-SETUP.md) — the full one-time server + CI setup (DNS, TLS, ACLs,
the secret/variable list, enrollment).
